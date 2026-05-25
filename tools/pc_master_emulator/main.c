/**
 * @file main.c
 * @brief PC 端动态 Mesh 主控模拟器 — 硬件 smoke test 工具
 *
 * 通过串口接收 Raw Mesh 帧，驱动主机侧 Mesh Runtime 完成节点发现，
 * 再通过 cluster_vfs 的 /mcuN/... 路径访问远端 Mini9P 文件。
 *
 * ## 典型工作流程
 *
 * 1. 打开串口，配置 8N1、无流控
 * 2. 初始化 mesh_host_runtime（将串口 fd 作为 transport_ctx 传入）
 * 3. 轮询 mesh_host_runtime_poll_once()，接收从机的 REGISTER 帧
 * 4. 收到第一次 REGISTER（src=dst=UNASSIGNED）时回送 ASSIGN，给节点分配地址
 * 5. 节点收到 ASSIGN 后会发送第二次 REGISTER（src=分配地址），确认上线
 * 6. cluster_vfs_attach("mcu1") 挂载节点，然后 cluster_vfs_read_path 读文件
 *
 * ## 帧格式（Mesh over UART）
 *
 * | 字段    | 长度   | 说明                    |
 * |---------|--------|------------------------|
 * | magic   | 2 字节 | 'M' 'H' (0x4D 0x48)    |
 * | length  | 2 字节 | little-endian，payload长度 |
 * | payload | n 字节 | 具体帧类型              |
 *
 * 所有多字节整数均为 little-endian。
 *
 * ## 依赖模块
 *
 * - pwos-shared/mini9p/          Mini9P 协议编解码
 * - pwos-shared/mesh/cluster/    集群节点管理与路由
 * - pwos-shared/mesh/envelope/   Mesh 帧封装/解封装
 * - pwos-shared/mesh/processer/ 帧处理器
 * - pwos-master-esp32p4/vfs_bridge/ cluster_vfs 集群虚拟文件系统
 */

#include "cluster_config.h"
#include "cluster_vfs.h"
#include "mesh_host_runtime.h"
#include "mesh_protocal.h"
#include "mini9p_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/** 默认波特率：1 Mbps（STM32F411 USART1 默认值） */
#define PC_MASTER_DEFAULT_BAUD 1000000u

/** poll/recv 超时时间（毫秒） */
#define PC_MASTER_TIMEOUT_MS 1000

/** 单帧最大缓冲（含 6 字节帧头） */
#define PC_MASTER_FRAME_CAP (MESH_FRAME_OVERHEAD + MESH_MAX_PAYLOAD_LEN)

/** 目标节点名称，与从机固件中的节点名对应 */
#define PC_MASTER_TARGET "mcu1"

/** 健康检查路径，先 walk 再 open read */
#define PC_MASTER_HEALTH_PATH "/mcu1/sys/health"

/** 发现阶段最大轮询次数（60 × 1s = 60s 超时） */
#define PC_MASTER_DISCOVERY_POLLS 60

/** 健康检查响应最大字节数 */
#define PC_MASTER_HEALTH_CAP 64u

/** 本机（PC 主控）在 Mesh 网络中的虚拟地址 */
#define PC_MASTER_LOCAL_ADDR 0x00u

/** 分配给第一个从节点的 Mesh 地址 */
#define PC_MASTER_FIRST_NODE_ADDR 0x11u

/** 节点地址租约有效期（毫秒），到期前需续约 */
#define PC_MASTER_ASSIGN_LEASE_MS 60000u

/**
 * @brief PC 端 Mesh 传输层实例
 *
 * 封装串口文件描述符、分配的节点地址以及帧序号计数器。
 * 作为 transport_ctx 传给 mesh_host_runtime。
 */
struct pc_mesh_transport {
    int fd;                     /**< 串口文件描述符，< 0 表示未打开 */
    uint8_t assigned_addr;      /**< 已分配给从节点的 Mesh 地址，未分配时为 MESH_ADDR_UNASSIGNED */
    uint16_t next_seq;          /**< 下一个待发送帧的序列号（用于 Mesh 帧头） */
};

/**
 * @brief 从字节数组读取 little-endian 16 位无符号整数
 * @param[in] data 至少包含 2 字节的输入缓冲区
 * @return 解析出的 16 位整数
 */
static uint16_t get_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8);
}

/**
 * @brief 打印命令行用法到 stderr
 * @param[in] prog 程序名称（argv[0]）
 */
static void print_usage(const char *prog)
{
    fprintf(stderr, "usage: %s <serial-dev> [baud]\n", prog);
    fprintf(stderr, "example: %s /dev/ttyUSB0 1000000\n", prog);
}

/**
 * @brief 将数字波特率转换为 POSIX termios speed_t 常量
 * @param[in] baud  目标波特率（如 115200、1000000）
 * @param[out] out_speed 转换结果输出指针
 * @return 转换成功返回 true；不支持的波特率返回 false，*out_speed 不修改
 *
 * @note 只支持常见的标准波特率，非标准值（如 500000）会返回 false。
 */
static bool baud_to_speed(unsigned long baud, speed_t *out_speed)
{
    if (out_speed == NULL) {
        return false;
    }

    switch (baud) {
    case 9600ul:
        *out_speed = B9600;
        return true;
    case 115200ul:
        *out_speed = B115200;
        return true;
#ifdef B230400
    case 230400ul:
        *out_speed = B230400;
        return true;
#endif
#ifdef B460800
    case 460800ul:
        *out_speed = B460800;
        return true;
#endif
#ifdef B921600
    case 921600ul:
        *out_speed = B921600;
        return true;
#endif
#ifdef B1000000
    case 1000000ul:
        *out_speed = B1000000;
        return true;
#endif
    default:
        return false;
    }
}

/**
 * @brief 打开并配置串口为 8N1、无流控
 *
 * 配置细节：
 * - 输入标志：禁用 IGNBRK、BRKINT、PARMRK、ISTRIP、INLCR、IGNCR、ICRNL、IXON
 * - 输出标志：禁用 OPOST（原始输出）
 * - 本地标志：禁用 ECHO、ECHONL、ICANON、ISIG、IEXTEN（原始模式）
 * - 控制标志：CS8 | CLOCAL | CREAD，无奇偶校验，1 位停止位
 * - 控制字符：VMIN=0, VTIME=0（立即返回，非阻塞）
 * - 打开后执行 tcflush 丢弃已接收但未读取的数据
 *
 * @param[in] path  串口设备路径，如 "/dev/ttyUSB0"
 * @param[in] baud  波特率（如 115200、1000000）
 * @return 成功返回文件描述符（≥ 0）；失败返回 -1，错误码见 errno
 */
static int open_serial(const char *path, unsigned long baud)
{
    struct termios tty;
    speed_t speed;
    int fd;

    if (!baud_to_speed(baud, &speed)) {
        fprintf(stderr, "unsupported baud rate: %lu\n", baud);
        return -1;
    }

    fd = open(path, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        perror("open serial");
        return -1;
    }

    if (tcgetattr(fd, &tty) != 0) {
        perror("tcgetattr");
        close(fd);
        return -1;
    }

    tty.c_iflag &= (tcflag_t) ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    tty.c_oflag &= (tcflag_t) ~OPOST;
    tty.c_lflag &= (tcflag_t) ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_cflag &= (tcflag_t) ~(CSIZE | PARENB | CSTOPB);
#ifdef CRTSCTS
    tty.c_cflag &= (tcflag_t) ~CRTSCTS;
#endif
    tty.c_cflag |= (tcflag_t)(CS8 | CLOCAL | CREAD);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (cfsetispeed(&tty, speed) != 0 || cfsetospeed(&tty, speed) != 0) {
        perror("cfset*speed");
        close(fd);
        return -1;
    }
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        close(fd);
        return -1;
    }

    tcflush(fd, TCIOFLUSH);
    return fd;
}

/**
 * @brief 等待串口可读（POLLIN）或超时
 *
 * 内部使用 poll(2)，对 EINTR 自动重试。超时返回 -MESH_ERR_BUSY；
 * poll 出错返回 -MESH_ERR_INVALID_STATE；检测到错误条件（断开、违规）
 * 返回 -MESH_ERR_INVALID_STATE 并打印详细诊断信息。
 *
 * @param[in] fd 已打开的串口文件描述符
 * @return 0 = 可读；-MESH_ERR_BUSY = 超时；-MESH_ERR_INVALID_STATE = 错误
 */
static int wait_readable(int fd)
{
    struct pollfd pfd;
    int rc;

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    do {
        rc = poll(&pfd, 1u, PC_MASTER_TIMEOUT_MS);
    } while (rc < 0 && errno == EINTR);

    if (rc == 0) {
        return -(int)MESH_ERR_BUSY;
    }
    if (rc < 0) {
        perror("poll");
        return -(int)MESH_ERR_INVALID_STATE;
    }
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        fprintf(stderr, "serial poll error: revents=0x%x\n", pfd.revents);
        return -(int)MESH_ERR_INVALID_STATE;
    }

    return 0;
}

/**
 * @brief 从串口精确读取指定字节数
 *
 * 循环调用 read(2) 直到恰好读取 len 字节。遇到超时、EIO 或 EOF 提前返回。
 * 对 EINTR 和 EAGAIN 自动重试。
 *
 * @param[in] fd    已打开的串口文件描述符
 * @param[out] data 输出缓冲区（至少 len 字节）
 * @param[in] len   要读取的字节数
 * @return 0 = 成功；负数错误码（如 -MESH_ERR_BUSY、-MESH_ERR_INVALID_STATE）
 */
static int read_exact(int fd, uint8_t *data, size_t len)
{
    size_t total = 0u;

    while (total < len) {
        ssize_t nread;
        int rc = wait_readable(fd);

        if (rc != 0) {
            return rc;
        }

        nread = read(fd, data + total, len - total);
        if (nread < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            perror("read");
            return -(int)MESH_ERR_INVALID_STATE;
        }
        if (nread == 0) {
            fprintf(stderr, "serial read returned EOF\n");
            return -(int)MESH_ERR_INVALID_STATE;
        }
        total += (size_t)nread;
    }

    return 0;
}

/**
 * @brief 将数据完整写入串口并等待发送完成
 *
 * 循环调用 write(2) 直到所有数据写出，最后调用 tcdrain(2) 确保串口
 * TX 寄存器清空（即数据已送出而非仅进入缓冲区）。
 *
 * @param[in] fd    已打开的串口文件描述符
 * @param[in] data  要发送的数据缓冲区
 * @param[in] len   数据长度（字节）
 * @return 0 = 成功；负数错误码
 */
static int write_all(int fd, const uint8_t *data, size_t len)
{
    size_t total = 0u;

    while (total < len) {
        ssize_t nwritten = write(fd, data + total, len - total);

        if (nwritten < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            perror("write");
            return -(int)MESH_ERR_INVALID_STATE;
        }
        if (nwritten == 0) {
            fprintf(stderr, "serial write returned zero\n");
            return -(int)MESH_ERR_INVALID_STATE;
        }
        total += (size_t)nwritten;
    }

    if (tcdrain(fd) != 0) {
        perror("tcdrain");
        return -(int)MESH_ERR_INVALID_STATE;
    }
    return 0;
}

/**
 * @brief 从串口读取一个完整的 Mesh 帧
 *
 * Mesh 帧格式：'M' 'H' + 2字节长度(little-endian) + payload。
 * 先读 4 字节帧头（magic + length），再根据 length 读剩余部分。
 * 长度字段描述 payload 长度，整个帧实际长度 = length + 6（帧头）。
 *
 * @param[in]  fd       已打开的串口文件描述符
 * @param[out] frame    接收缓冲区（调用方保证足够大）
 * @param[in]  cap      接收缓冲区总容量
 * @param[out] out_len  成功时写入实际接收的帧总字节数
 * @return 0 = 成功；负数错误码（-MESH_ERR_BAD_FRAME 表示帧格式错误）
 *
 * @note 如果帧长度字段 < 8（小于最小合法帧），判定为坏帧。
 */
static int pc_receive_mesh_frame(int fd, uint8_t *frame, size_t cap, size_t *out_len)
{
    uint8_t header[4];
    uint16_t frame_len;
    size_t total_len;
    int rc;

    if (frame == NULL || out_len == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    *out_len = 0u;
    rc = read_exact(fd, header, sizeof(header));
    if (rc != 0) {
        return rc;
    }
    if (header[0] != (uint8_t)'M' || header[1] != (uint8_t)'H') {
        fprintf(stderr, "bad mesh frame magic: %02x %02x\n", header[0], header[1]);
        return -(int)MESH_ERR_BAD_FRAME;
    }

    frame_len = get_le16(header + 2);
    if (frame_len < 8u) {
        fprintf(stderr, "bad mesh frame length field: %u\n", frame_len);
        return -(int)MESH_ERR_BAD_FRAME;
    }

    total_len = (size_t)frame_len + 6u;
    if (total_len > cap || total_len < MESH_FRAME_OVERHEAD) {
        fprintf(stderr, "mesh frame too large or too small: %zu\n", total_len);
        return -(int)MESH_ERR_BAD_FRAME;
    }

    memcpy(frame, header, sizeof(header));
    rc = read_exact(fd, frame + sizeof(header), total_len - sizeof(header));
    if (rc != 0) {
        return rc;
    }

    *out_len = total_len;
    return 0;
}

/**
 * @brief 将 Mesh 帧类型数值转换为可读字符串
 * @param[in] type MESH_TYPE_xxx 常量
 * @return 静态字符串指针，永不为 NULL
 */
static const char *mesh_type_name(uint8_t type)
{
    switch (type) {
    case MESH_TYPE_MINI9P:
        return "MINI9P";
    case MESH_TYPE_REGISTER:
        return "REGISTER";
    case MESH_TYPE_ASSIGN:
        return "ASSIGN";
    case MESH_TYPE_PING:
        return "PING";
    case MESH_TYPE_PONG:
        return "PONG";
    case MESH_TYPE_TIME_SYNC:
        return "TIME_SYNC";
    case MESH_TYPE_ROUTE_UPDATE:
        return "ROUTE_UPDATE";
    case MESH_TYPE_LINK_STATE:
        return "LINK_STATE";
    case MESH_TYPE_ERROR:
        return "ERROR";
    default:
        return "unknown";
    }
}

/**
 * @brief 发送 Mesh 帧回调（transport send_frame 实现）
 *
 * 内部调用 write_all 将帧完整写出，然后 tcdrain 等待 TX 完成。
 * 调用者保证 tx_data 是完整的 Mesh 帧（含帧头），tx_len 包含全部长度。
 *
 * @param[in] transport_ctx 指向 struct pc_mesh_transport 的指针
 * @param[in] next_hop      下一跳 Mesh 地址（当前实现未使用，帧已完整编码）
 * @param[in] tx_data       待发送的帧数据
 * @param[in] tx_len        帧长度（字节）
 * @return 0 = 成功；负数错误码
 */
static int pc_mesh_send_frame(void *transport_ctx, uint8_t next_hop, const uint8_t *tx_data, size_t tx_len)
{
    struct pc_mesh_transport *transport = (struct pc_mesh_transport *)transport_ctx;

    if (transport == NULL || transport->fd < 0 || tx_data == NULL || tx_len == 0u) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    printf("[PC→mesh] next_hop=0x%02x %zu bytes\n", next_hop, tx_len);
    return write_all(transport->fd, tx_data, tx_len);
}

/**
 * @brief 生成并返回下一个 Mesh 帧序列号
 *
 * 序列号从 1 开始，每次调用后递增；绕回时强制从 1 重新开始以避免 0 值。
 * 帧序号用于 mesh_host_runtime 的响应匹配。
 *
 * @param[in] transport 指向 pc_mesh_transport 的指针
 * @return 下一个有效的帧序列号（永不返回 0）
 */
static uint16_t pc_mesh_next_seq(struct pc_mesh_transport *transport)
{
    uint16_t seq = transport->next_seq;

    ++transport->next_seq;
    if (transport->next_seq == 0u) {
        transport->next_seq = 1u;
    }
    return seq;
}

/**
 * @brief 处理第一次 REGISTER（节点尚未分配地址）
 *
 * 当从机刚启动发出 REGISTER（src=dst=UNASSIGNED）时，PC 主控：
 * 1. 从帧中解析节点的 UID
 * 2. 分配一个固定的 Mesh 地址（PC_MASTER_FIRST_NODE_ADDR = 0x11）
 * 3. 构造并发送 ASSIGN 帧
 * 4. 将节点标记为 ONLINE 并建立双向 link
 *
 * @param[in] transport     指向 pc_mesh_transport 的指针
 * @param[in] register_frame 已解码的 REGISTER 帧视图
 * @return 0 = 成功；负数错误码（-MESH_ERR_BAD_FRAME 表示帧解析失败）
 *
 * @note ASSIGN 发送后节点地址尚未最终确认，实际在线状态要等节点
 *       收到 ASSIGN 后发送第二次 REGISTER（带分配到的地址）才算正式完成。
 */
static int pc_mesh_assign_bootstrap_node(
    struct pc_mesh_transport *transport,
    const struct mesh_frame_view *register_frame)
{
    struct mesh_register_payload register_payload;
    struct mesh_assign_payload assign_payload;
    uint8_t assign_frame[PC_MASTER_FRAME_CAP];
    size_t assign_len = 0u;

    if (!mesh_parse_register(register_frame, &register_payload)) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    if (transport->assigned_addr == MESH_ADDR_UNASSIGNED) {
        transport->assigned_addr = PC_MASTER_FIRST_NODE_ADDR;
    }

    memset(&assign_payload, 0, sizeof(assign_payload));
    memcpy(assign_payload.uid, register_payload.uid, sizeof(assign_payload.uid));
    assign_payload.node_addr = transport->assigned_addr;
    assign_payload.lease_ms = PC_MASTER_ASSIGN_LEASE_MS;
    assign_payload.epoch = 1u;
    (void)snprintf(assign_payload.node_name, sizeof(assign_payload.node_name), "%s", PC_MASTER_TARGET);

    if (!mesh_build_assign(
            PC_MASTER_LOCAL_ADDR,
            MESH_ADDR_UNASSIGNED,
            pc_mesh_next_seq(transport),
            MESH_PROCESSER_DEFAULT_HOP,
            &assign_payload,
            assign_frame,
            sizeof(assign_frame),
            &assign_len)) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    printf("[PC→mesh] ASSIGN %s addr=0x%02x (%zu bytes)\n", PC_MASTER_TARGET, assign_payload.node_addr, assign_len);
    return write_all(transport->fd, assign_frame, assign_len);
}

/**
 * @brief 确认已分配地址的从节点正式上线
 *
 * 当收到第二次 REGISTER（src=已分配地址，dst=UNASSIGNED）时调用。
 * 将节点标记为 ONLINE 并在本地节点与该从节点之间建立一条双向 link，
 * 表示集群拓扑中存在直接相连的邻居。
 *
 * @param[in] transport  指向 pc_mesh_transport 的指针
 * @param[in] mesh_addr  从节点被分配的 Mesh 地址（MESH_ADDR_UNASSIGNED 为非法）
 * @return 0 = 成功；负数错误码
 */
static int pc_mesh_confirm_assigned_node(struct pc_mesh_transport *transport, uint8_t mesh_addr)
{
    struct cluster *mesh_cluster = cluster_config_mesh_cluster();
    int rc;

    if (transport == NULL || mesh_addr == MESH_ADDR_UNASSIGNED) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    if (mesh_cluster == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    rc = cluster_set_node_online(mesh_cluster, mesh_addr, true);
    if (rc != 0) {
        return rc;
    }
    rc = cluster_add_link(mesh_cluster, PC_MASTER_LOCAL_ADDR, mesh_addr, 1u, false);
    if (rc != 0) {
        return rc;
    }

    transport->assigned_addr = mesh_addr;
    return 0;
}

/**
 * @brief 接收并处理一个 Mesh 帧（transport receive_frame 实现）
 *
 * 从串口读取一个完整的 Mesh 帧，解码帧头并打印诊断信息。
 * 对两次 REGISTER 有不同处理：
 * - 第一次（src=dst=UNASSIGNED）：分配地址并回送 ASSIGN，返回 -MESH_ERR_BUSY
 *   告知调用方本帧已被消费无需进一步处理。
 * - 第二次（src≠UNASSIGNED, dst=UNASSIGNED）：确认节点上线，正常返回。
 *
 * @param[in]  transport_ctx 指向 pc_mesh_transport 的指针
 * @param[out] rx_data      接收缓冲区
 * @param[in]  rx_cap       接收缓冲区容量
 * @param[out] rx_len       成功时写入实际接收帧字节数；消费帧时清零
 * @return 0 = 成功，帧已放入 rx_data；-MESH_ERR_BUSY = 帧已消费（第一次 REGISTER）；
 *         负数 = 错误
 */
static int pc_mesh_receive_frame(void *transport_ctx, uint8_t *rx_data, size_t rx_cap, size_t *rx_len)
{
    struct pc_mesh_transport *transport = (struct pc_mesh_transport *)transport_ctx;
    struct mesh_frame_view view;
    int rc;

    if (transport == NULL || transport->fd < 0) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    rc = pc_receive_mesh_frame(transport->fd, rx_data, rx_cap, rx_len);
    if (rc != 0) {
        return rc;
    }

    if (!mesh_decode_frame(rx_data, *rx_len, &view)) {
        fprintf(stderr, "[mesh→PC] failed to decode received mesh frame\n");
        return 0;
    }

    printf("[mesh→PC] %s src=0x%02x dst=0x%02x seq=%u payload=%u\n",
           mesh_type_name(view.type),
           view.src,
           view.dst,
           view.seq,
           view.payload_len);

    if (view.type == MESH_TYPE_REGISTER &&
        view.src == MESH_ADDR_UNASSIGNED &&
        view.dst == MESH_ADDR_UNASSIGNED) {
        rc = pc_mesh_assign_bootstrap_node(transport, &view);
        if (rc != 0) {
            return rc;
        }
        *rx_len = 0u;
        return -(int)MESH_ERR_BUSY;
    }
    if (view.type == MESH_TYPE_REGISTER &&
        view.src != MESH_ADDR_UNASSIGNED &&
        view.dst == MESH_ADDR_UNASSIGNED) {
        rc = pc_mesh_confirm_assigned_node(transport, view.src);
        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

/**
 * @brief 初始化 mesh_host_runtime
 *
 * 依次调用 cluster_config_init_mesh_host() 和 mesh_host_runtime_init()。
 * 配置项：
 * - mesh_cluster：从 cluster_config 获取
 * - transport_ctx：本模块的 pc_mesh_transport 指针
 * - send_frame：pc_mesh_send_frame
 * - receive_frame：pc_mesh_receive_frame
 *
 * @param[out] runtime  待初始化的 mesh_host_runtime 结构
 * @param[in] transport 已打开串口的 pc_mesh_transport
 * @return 0 = 成功；负数错误码
 */
static int init_runtime(struct mesh_host_runtime *runtime, struct pc_mesh_transport *transport)
{
    struct mesh_host_runtime_config config;
    int rc;

    rc = cluster_config_init_mesh_host();
    if (rc != 0) {
        fprintf(stderr, "cluster_config_init_mesh_host failed: %d\n", rc);
        return rc;
    }

    mesh_host_runtime_get_default_config(&config);
    config.mesh_cluster = cluster_config_mesh_cluster();
    config.transport_ctx = transport;
    config.send_frame = pc_mesh_send_frame;
    config.receive_frame = pc_mesh_receive_frame;

    rc = mesh_host_runtime_init(runtime, &config);
    if (rc != 0) {
        fprintf(stderr, "mesh_host_runtime_init failed: %d\n", rc);
    }
    return rc;
}

/**
 * @brief 等待目标节点完成 Mesh 发现并被 cluster_vfs 挂载
 *
 * 轮询 mesh_host_runtime_poll_once() 接收 Mesh 帧，同时尝试
 * cluster_vfs_attach(target)。
 *
 * 容忍的错误码：
 * - ENOENT：节点尚未被发现，continue
 * - EAGAIN： attach 未完成，continue
 * - EBUSY： mesh 忙，continue
 * - EIO： 传输错误（可能节点已上线但通信暂时异常），continue
 *
 * 其他错误码立即返回。
 *
 * @param[in] runtime mesh_host_runtime 指针
 * @param[in] target 节点名称字符串（如 "mcu1"）
 * @return 0 = 挂载成功；负数错误码（ENOTTY/EAGAIN 等）
 */
static int wait_for_target(struct mesh_host_runtime *runtime, const char *target)
{
    int i;

    printf("mesh runtime: waiting for %s; reset the slave now if it already booted\n", target);
    for (i = 0; i < PC_MASTER_DISCOVERY_POLLS; ++i) {
        int rc = cluster_vfs_attach(target);

        if (rc == 0) {
            printf("mesh runtime: %s attached\n", target);
            return 0;
        }
        if (rc != -(int)M9P_ERR_ENOENT &&
            rc != -(int)M9P_ERR_EAGAIN &&
            rc != -(int)M9P_ERR_EBUSY &&
            rc != -(int)M9P_ERR_EIO) {
            fprintf(stderr, "cluster_vfs_attach(%s) failed: %d\n", target, rc);
            return rc;
        }

        rc = mesh_host_runtime_poll_once(runtime);
        if (rc != 0 && rc != -(int)MESH_ERR_BUSY && rc != -(int)M9P_ERR_EBUSY) {
            fprintf(stderr, "mesh_host_runtime_poll_once failed: %d\n", rc);
            return rc;
        }
    }

    fprintf(stderr, "timed out waiting for %s\n", target);
    return -(int)M9P_ERR_EAGAIN;
}

/**
 * @brief 执行 smoke test：attach + walk + open + read + clunk
 *
 * 依次：
 * 1. wait_for_target 等待节点被发现并挂载
 * 2. cluster_vfs_read_path("/mcu1/sys/health") 读取健康检查文件
 * 3. 验证响应内容为 "ok\n"，否则返回 EIO
 *
 * 成功时打印 "pc_master_emulator: ok" 并返回 0。
 *
 * @param[in] runtime 已初始化的 mesh_host_runtime
 * @return 0 = 测试通过；负数错误码
 */
static int run_dynamic_sequence(struct mesh_host_runtime *runtime)
{
    uint8_t health[PC_MASTER_HEALTH_CAP];
    uint16_t len = sizeof(health);
    int rc;

    rc = wait_for_target(runtime, PC_MASTER_TARGET);
    if (rc != 0) {
        return rc;
    }

    rc = cluster_vfs_read_path(PC_MASTER_HEALTH_PATH, health, &len);
    if (rc != 0) {
        fprintf(stderr, "cluster_vfs_read_path(%s) failed: %d\n", PC_MASTER_HEALTH_PATH, rc);
        return rc;
    }

    printf("read %s: %.*s", PC_MASTER_HEALTH_PATH, (int)len, (const char *)health);
    if (len != 3u || memcmp(health, "ok\n", 3u) != 0) {
        fprintf(stderr, "unexpected %s payload\n", PC_MASTER_HEALTH_PATH);
        return -(int)M9P_ERR_EIO;
    }

    puts("pc_master_emulator: ok");
    return 0;
}

/**
 * @brief 程序入口
 *
 * 解析命令行参数，打开串口，初始化 runtime，执行 smoke test，最后清理。
 *
 * 用法：pc_master_emulator <serial-dev> [baud]
 *   serial-dev：串口设备路径（必需）
 *   baud：波特率（可选，默认 1000000）
 *
 * @param argc 命令行参数个数
 * @param argv 参数数组
 * @return 0 = 成功；1 = 运行失败；2 = 参数错误
 */
int main(int argc, char **argv)
{
    const char *serial_path;
    unsigned long baud = PC_MASTER_DEFAULT_BAUD;
    struct mesh_host_runtime runtime;
    struct pc_mesh_transport transport;
    int rc;

    if (argc < 2 || argc > 3) {
        print_usage(argv[0]);
        return 2;
    }

    serial_path = argv[1];
    if (argc == 3) {
        char *end = NULL;

        baud = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || baud == 0ul) {
            fprintf(stderr, "invalid baud rate: %s\n", argv[2]);
            return 2;
        }
    }

    transport.fd = open_serial(serial_path, baud);
    transport.assigned_addr = MESH_ADDR_UNASSIGNED;
    transport.next_seq = 1u;
    if (transport.fd < 0) {
        return 1;
    }

    printf("pc_master_emulator: %s @ %lu baud\n", serial_path, baud);
    rc = init_runtime(&runtime, &transport);
    if (rc == 0) {
        rc = run_dynamic_sequence(&runtime);
        mesh_host_runtime_deinit(&runtime);
    }

    close(transport.fd);
    return rc == 0 ? 0 : 1;
}