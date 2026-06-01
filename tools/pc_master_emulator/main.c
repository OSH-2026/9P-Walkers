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
 * 5. ASSIGN 成功发出后主机立即注册节点；旧固件的二次 REGISTER 仅作幂等刷新
 * 6. 从机通过 LINK_STATE 上报真实邻居边后，host runtime 下发 ROUTE_UPDATE
 * 7. cluster_vfs_attach("mcuN") 挂载节点，然后 cluster_vfs_read_path 读文件
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
#include "cluster_host_vfs.h"
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
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/** 默认波特率：1 Mbps（当前 STM32 Mini9P UART 默认值） */
#define PC_MASTER_DEFAULT_BAUD 1000000u

/** poll/recv 超时时间（毫秒） */
#define PC_MASTER_TIMEOUT_MS 1000

/** 单帧最大缓冲（含 6 字节帧头） */
#define PC_MASTER_FRAME_CAP (MESH_FRAME_OVERHEAD + MESH_MAX_PAYLOAD_LEN)

/** 默认等待注册并检查的节点数。 */
#define PC_MASTER_DEFAULT_NODE_COUNT 2u

/** 按 0x11、0x22... 分配时可用的最大目标节点数，避免碰到 0xff 未分配地址。 */
#define PC_MASTER_MAX_TARGET_NODES 14u

/** 节点名格式，与 cluster_vfs 的 /mcuN/... 命名空间对应。 */
#define PC_MASTER_NODE_NAME_FMT "mcu%zu"

/** 健康检查路径格式，先 walk 再 open read。 */
#define PC_MASTER_HEALTH_PATH_FMT "/%s/sys/health"

/** 从机 mesh 路由诊断路径。 */
#define PC_MASTER_ROUTES_PATH_FMT "/%s/sys/routes"

/** 从机通用诊断日志路径。 */
#define PC_MASTER_LOG_PATH_FMT "/%s/sys/log"

/** 发现阶段最大轮询次数（60 × 1s = 60s 超时） */
#define PC_MASTER_DISCOVERY_POLLS 60

/** 健康检查响应最大字节数 */
#define PC_MASTER_HEALTH_CAP 64u

/** 路由诊断响应最大字节数 */
#define PC_MASTER_ROUTES_CAP 512u

/** 诊断日志响应最大字节数 */
#define PC_MASTER_LOG_CAP 2048u

/** 本机（PC 主控）在 Mesh 网络中的虚拟地址 */
#define PC_MASTER_LOCAL_ADDR 0x00u

/** 节点地址租约有效期（毫秒），到期前需续约 */
#define PC_MASTER_ASSIGN_LEASE_MS 60000u

/**
 * @brief 本次运行内的节点分配槽位。
 *
 * 工具不持久化 UID 映射；同一进程内重复 REGISTER 会复用同一槽位。
 */
struct pc_master_node_slot {
    bool used;                  /**< 是否已分配给某个 UID */
    uint8_t uid[MESH_UID_LEN];  /**< REGISTER 中的硬件 UID */
    uint8_t mesh_addr;          /**< 本次运行分配的 mesh 地址 */
    char name[MESH_MAX_NODE_NAME + 1u]; /**< 本次运行分配的节点名，如 mcu1 */
};

/**
 * @brief PC 端 Mesh 传输层实例
 *
 * 封装串口文件描述符、分配的节点地址以及帧序号计数器。
 * 作为 transport_ctx 传给 mesh_host_runtime。
 */
struct pc_mesh_transport {
    int fd;                                  /**< 串口文件描述符，< 0 表示未打开 */
    uint16_t next_seq;                       /**< 下一个待发送帧的序列号（用于 Mesh 帧头） */
    size_t target_node_count;                /**< 本次 smoke test 期望的节点数量 */
    size_t assigned_node_count;              /**< 已分配过槽位的节点数量 */
    struct mesh_host_runtime *runtime;       /**< host runtime，用于 ASSIGN 后立即注册节点 */
    struct pc_master_node_slot nodes[PC_MASTER_MAX_TARGET_NODES]; /**< 节点分配表 */
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
    fprintf(stderr, "usage: %s <serial-dev> [baud] [node-count]\n", prog);
    fprintf(stderr, "example: %s /dev/ttyUSB0 1000000 2\n", prog);
    fprintf(stderr, "node-count range: 1..%u, default: %u\n",
            (unsigned)PC_MASTER_MAX_TARGET_NODES,
            (unsigned)PC_MASTER_DEFAULT_NODE_COUNT);
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
    int modem_bits;
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

    modem_bits = TIOCM_DTR | TIOCM_RTS;
    if (ioctl(fd, TIOCMBIC, &modem_bits) != 0) {
        perror("ioctl TIOCMBIC DTR/RTS");
        close(fd);
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
    tty.c_cflag &= (tcflag_t) ~HUPCL;
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
    case MESH_TYPE_NEIGHBOR_PROBE_REQUEST:
        return "NEIGHBOR_PROBE_REQUEST";
    case MESH_TYPE_NEIGHBOR_PROBE_RESPONSE:
        return "NEIGHBOR_PROBE_RESPONSE";
    case MESH_TYPE_ERROR:
        return "ERROR";
    default:
        return "unknown";
    }
}

static const char *mini9p_type_name(uint8_t type)
{
    switch (type) {
    case M9P_TATTACH:
        return "TATTACH";
    case M9P_RATTACH:
        return "RATTACH";
    case M9P_TWALK:
        return "TWALK";
    case M9P_RWALK:
        return "RWALK";
    case M9P_TOPEN:
        return "TOPEN";
    case M9P_ROPEN:
        return "ROPEN";
    case M9P_TREAD:
        return "TREAD";
    case M9P_RREAD:
        return "RREAD";
    case M9P_TWRITE:
        return "TWRITE";
    case M9P_RWRITE:
        return "RWRITE";
    case M9P_TSTAT:
        return "TSTAT";
    case M9P_RSTAT:
        return "RSTAT";
    case M9P_TCLUNK:
        return "TCLUNK";
    case M9P_RCLUNK:
        return "RCLUNK";
    case M9P_RERROR:
        return "RERROR";
    default:
        return "UNKNOWN";
    }
}

static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool parse_hex_u8(const char *text, size_t len, uint8_t *out_value)
{
    int hi;
    int lo;

    if (text == NULL || out_value == NULL || len < 2u) {
        return false;
    }

    hi = hex_nibble(text[0]);
    lo = hex_nibble(text[1]);
    if (hi < 0 || lo < 0) {
        return false;
    }

    *out_value = (uint8_t)(((uint8_t)hi << 4) | (uint8_t)lo);
    return true;
}

static bool line_matches_at(const char *line, size_t line_len, size_t offset, const char *prefix)
{
    size_t prefix_len;

    if (line == NULL || prefix == NULL || offset > line_len) {
        return false;
    }

    prefix_len = strlen(prefix);
    return prefix_len <= line_len - offset &&
           memcmp(line + offset, prefix, prefix_len) == 0;
}

static void print_annotated_log_line(const char *line, size_t line_len)
{
    size_t i = 0u;

    while (i < line_len) {
        uint8_t value;

        if (line_matches_at(line, line_len, i, "type=0x") &&
            parse_hex_u8(line + i + 7u, line_len - i - 7u, &value)) {
            printf("type=0x%02x(%s)", value, mesh_type_name(value));
            i += 9u;
            continue;
        }
        if (line_matches_at(line, line_len, i, "m9p=0x") &&
            parse_hex_u8(line + i + 6u, line_len - i - 6u, &value)) {
            printf("m9p=0x%02x(%s)", value, mini9p_type_name(value));
            i += 8u;
            continue;
        }

        putchar((unsigned char)line[i]);
        ++i;
    }
    putchar('\n');
}

static void print_annotated_log_text(const char *text)
{
    const char *line = text;

    if (text == NULL) {
        return;
    }

    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        size_t line_len;

        if (end == NULL) {
            print_annotated_log_line(line, strlen(line));
            break;
        }

        line_len = (size_t)(end - line);
        print_annotated_log_line(line, line_len);
        line = end + 1;
    }
}

static size_t pc_log_prefix_width(const char *prefix)
{
    size_t width = 0u;
    const unsigned char *p = (const unsigned char *)prefix;

    if (p == NULL) {
        return 0u;
    }

    while (*p != '\0') {
        if ((*p & 0xc0u) != 0x80u) {
            ++width;
        }
        ++p;
    }
    return width + 1u;
}

static void pc_log_detail_indent(size_t width)
{
    size_t i;

    for (i = 0u; i < width; ++i) {
        putchar(' ');
    }
}

static void pc_mini9p_log_frame_details(size_t indent_width, const struct mesh_frame_view *view)
{
    struct m9p_frame_view frame;

    if (view == NULL || view->type != MESH_TYPE_MINI9P) {
        return;
    }

    if (!m9p_decode_frame(view->payload, view->payload_len, &frame)) {
        pc_log_detail_indent(indent_width);
        printf("mini9p decode failed payload=%u\n", view->payload_len);
        return;
    }

    switch (frame.type) {
    case M9P_TATTACH: {
        struct m9p_attach_request request;

        if (m9p_parse_tattach(&frame, &request)) {
            pc_log_detail_indent(indent_width);
            printf(
                "mini9p %s tag=%u fid=%u msize=%u inflight=%u flags=0x%02x\n",
                mini9p_type_name(frame.type),
                frame.tag,
                request.fid,
                request.requested_msize,
                request.requested_inflight,
                request.attach_flags);
        }
        break;
    }
    case M9P_RATTACH: {
        struct m9p_attach_result result;

        if (m9p_parse_rattach(&frame, &result)) {
            pc_log_detail_indent(indent_width);
            printf(
                "mini9p %s tag=%u msize=%u max_fids=%u inflight=%u features=0x%08lx root_qid=0x%08lx\n",
                mini9p_type_name(frame.type),
                frame.tag,
                result.negotiated_msize,
                result.max_fids,
                result.max_inflight,
                (unsigned long)result.feature_bits,
                (unsigned long)result.root_qid.object_id);
        }
        break;
    }
    case M9P_TWALK: {
        struct m9p_walk_request request;

        if (m9p_parse_twalk(&frame, &request)) {
            pc_log_detail_indent(indent_width);
            printf(
                "mini9p %s tag=%u fid=%u newfid=%u path=%s\n",
                mini9p_type_name(frame.type),
                frame.tag,
                request.fid,
                request.newfid,
                request.path);
        }
        break;
    }
    case M9P_RWALK: {
        struct m9p_qid qid;

        if (m9p_parse_rwalk(&frame, &qid)) {
            pc_log_detail_indent(indent_width);
            printf(
                "mini9p %s tag=%u qid_type=0x%02x qid=0x%08lx\n",
                mini9p_type_name(frame.type),
                frame.tag,
                qid.type,
                (unsigned long)qid.object_id);
        }
        break;
    }
    case M9P_TOPEN: {
        struct m9p_open_request request;

        if (m9p_parse_topen(&frame, &request)) {
            pc_log_detail_indent(indent_width);
            printf(
                "mini9p %s tag=%u fid=%u mode=0x%02x\n",
                mini9p_type_name(frame.type),
                frame.tag,
                request.fid,
                request.mode);
        }
        break;
    }
    case M9P_ROPEN: {
        struct m9p_open_result result;

        if (m9p_parse_ropen(&frame, &result)) {
            pc_log_detail_indent(indent_width);
            printf(
                "mini9p %s tag=%u qid_type=0x%02x qid=0x%08lx iounit=%u\n",
                mini9p_type_name(frame.type),
                frame.tag,
                result.qid.type,
                (unsigned long)result.qid.object_id,
                result.iounit);
        }
        break;
    }
    case M9P_TREAD: {
        struct m9p_read_request request;

        if (m9p_parse_tread(&frame, &request)) {
            pc_log_detail_indent(indent_width);
            printf(
                "mini9p %s tag=%u fid=%u offset=%lu count=%u\n",
                mini9p_type_name(frame.type),
                frame.tag,
                request.fid,
                (unsigned long)request.offset,
                request.count);
        }
        break;
    }
    case M9P_RREAD: {
        uint16_t count;

        if (frame.payload_len >= 2u) {
            count = (uint16_t)frame.payload[0] | (uint16_t)((uint16_t)frame.payload[1] << 8);
            pc_log_detail_indent(indent_width);
            printf(
                "mini9p %s tag=%u count=%u\n",
                mini9p_type_name(frame.type),
                frame.tag,
                count);
        }
        break;
    }
    case M9P_TWRITE: {
        struct m9p_write_request request;

        if (m9p_parse_twrite(&frame, &request)) {
            pc_log_detail_indent(indent_width);
            printf(
                "mini9p %s tag=%u fid=%u offset=%lu count=%u\n",
                mini9p_type_name(frame.type),
                frame.tag,
                request.fid,
                (unsigned long)request.offset,
                request.count);
        }
        break;
    }
    case M9P_RWRITE: {
        uint16_t count = 0u;

        if (m9p_parse_rwrite(&frame, &count)) {
            pc_log_detail_indent(indent_width);
            printf(
                "mini9p %s tag=%u count=%u\n",
                mini9p_type_name(frame.type),
                frame.tag,
                count);
        }
        break;
    }
    case M9P_TSTAT: {
        uint16_t fid = 0u;

        if (m9p_parse_tstat(&frame, &fid)) {
            pc_log_detail_indent(indent_width);
            printf("mini9p %s tag=%u fid=%u\n", mini9p_type_name(frame.type), frame.tag, fid);
        }
        break;
    }
    case M9P_RSTAT: {
        struct m9p_stat stat;

        if (m9p_parse_rstat(&frame, &stat)) {
            pc_log_detail_indent(indent_width);
            printf(
                "mini9p %s tag=%u name=%s size=%lu flags=0x%02x\n",
                mini9p_type_name(frame.type),
                frame.tag,
                stat.name,
                (unsigned long)stat.size,
                stat.flags);
        }
        break;
    }
    case M9P_TCLUNK: {
        uint16_t fid = 0u;

        if (m9p_parse_tclunk(&frame, &fid)) {
            pc_log_detail_indent(indent_width);
            printf("mini9p %s tag=%u fid=%u\n", mini9p_type_name(frame.type), frame.tag, fid);
        }
        break;
    }
    case M9P_RCLUNK:
        pc_log_detail_indent(indent_width);
        printf("mini9p %s tag=%u\n", mini9p_type_name(frame.type), frame.tag);
        break;
    case M9P_RERROR: {
        struct m9p_error error;

        if (m9p_parse_rerror(&frame, &error)) {
            pc_log_detail_indent(indent_width);
            printf(
                "mini9p %s tag=%u code=%u (%s) msg=%s\n",
                mini9p_type_name(frame.type),
                frame.tag,
                error.code,
                m9p_error_name(error.code),
                error.msg);
        }
        break;
    }
    default:
        pc_log_detail_indent(indent_width);
        printf(
            "mini9p %s tag=%u payload=%u\n",
            mini9p_type_name(frame.type),
            frame.tag,
            frame.payload_len);
        break;
    }
}

static void pc_mesh_log_frame_details_ex(
    const char *prefix,
    const struct mesh_frame_view *view,
    const char *suffix)
{
    struct mesh_link_state_payload link_state;
    struct mesh_route_update_payload route_update;

    if (prefix == NULL || view == NULL) {
        return;
    }

    printf("%s %s src=0x%02x dst=0x%02x seq=%u payload=%u",
           prefix,
           mesh_type_name(view->type),
           view->src,
           view->dst,
           view->seq,
           view->payload_len);
    if (suffix != NULL) {
        printf(" %s", suffix);
    }
    putchar('\n');

    switch (view->type) {
    case MESH_TYPE_MINI9P:
        pc_mini9p_log_frame_details(pc_log_prefix_width(prefix), view);
        break;
    case MESH_TYPE_LINK_STATE:
        if (mesh_parse_link_state(view, &link_state)) {
            pc_log_detail_indent(pc_log_prefix_width(prefix));
            printf("LINK_STATE neighbor=0x%02x link_up=%u quality=%u\n",
                   link_state.neighbor, link_state.link_up, link_state.quality);
        }
        break;
    case MESH_TYPE_ROUTE_UPDATE:
        if (mesh_parse_route_update(view, &route_update)) {
            pc_log_detail_indent(pc_log_prefix_width(prefix));
            printf("ROUTE_UPDATE dst=0x%02x next_hop=0x%02x metric=%u version=%u action=%u\n",
                   route_update.dst,
                   route_update.next_hop,
                   route_update.metric,
                   route_update.route_version,
                   route_update.action);
        }
        break;
    case MESH_TYPE_NEIGHBOR_PROBE_REQUEST:
        pc_log_detail_indent(pc_log_prefix_width(prefix));
        printf("NEIGHBOR_PROBE_REQUEST\n");
        break;
    case MESH_TYPE_NEIGHBOR_PROBE_RESPONSE:
        pc_log_detail_indent(pc_log_prefix_width(prefix));
        printf("NEIGHBOR_PROBE_RESPONSE\n");
        break;
    default:
        break;
    }
}

static void pc_mesh_log_frame_details(const char *prefix, const struct mesh_frame_view *view)
{
    pc_mesh_log_frame_details_ex(prefix, view, NULL);
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
    struct mesh_frame_view view;

    if (transport == NULL || transport->fd < 0 || tx_data == NULL || tx_len == 0u) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    if (mesh_decode_frame(tx_data, tx_len, &view)) {
        char suffix[48];

        (void)snprintf(suffix, sizeof(suffix), "next_hop=0x%02x bytes=%zu", next_hop, tx_len);
        pc_mesh_log_frame_details_ex("[PC→mesh]", &view, suffix);
    } else {
        printf("[PC→mesh] next_hop=0x%02x bytes=%zu decode=failed\n", next_hop, tx_len);
    }
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

static uint8_t pc_master_node_addr_for_index(size_t index)
{
    size_t one_based = index + 1u;

    return (uint8_t)((one_based << 4u) | one_based);
}

static struct pc_master_node_slot *pc_master_find_node_by_uid(
    struct pc_mesh_transport *transport,
    const uint8_t uid[MESH_UID_LEN])
{
    size_t i;

    for (i = 0u; i < transport->assigned_node_count; ++i) {
        if (transport->nodes[i].used && memcmp(transport->nodes[i].uid, uid, MESH_UID_LEN) == 0) {
            return &transport->nodes[i];
        }
    }

    return NULL;
}

static struct pc_master_node_slot *pc_master_find_node_by_addr(
    struct pc_mesh_transport *transport,
    uint8_t mesh_addr)
{
    size_t i;

    for (i = 0u; i < transport->assigned_node_count; ++i) {
        if (transport->nodes[i].used && transport->nodes[i].mesh_addr == mesh_addr) {
            return &transport->nodes[i];
        }
    }

    return NULL;
}

static struct pc_master_node_slot *pc_master_find_node_by_name(
    struct pc_mesh_transport *transport,
    const char *name)
{
    size_t i;

    if (transport == NULL || name == NULL) {
        return NULL;
    }

    for (i = 0u; i < transport->assigned_node_count; ++i) {
        if (transport->nodes[i].used && strcmp(transport->nodes[i].name, name) == 0) {
            return &transport->nodes[i];
        }
    }

    return NULL;
}

static bool pc_master_target_routes_ready(struct pc_mesh_transport *transport, const char *target)
{
    struct cluster *mesh_cluster = cluster_config_mesh_cluster();
    struct pc_master_node_slot *slot;
    uint8_t next_hop = 0u;
    uint8_t metric = 0u;
    bool reachable = false;

    if (mesh_cluster == NULL) {
        return false;
    }
    slot = pc_master_find_node_by_name(transport, target);
    if (slot == NULL || !slot->used) {
        return false;
    }
    if (cluster_can_reach(mesh_cluster, slot->mesh_addr, &reachable) != 0 || !reachable) {
        return false;
    }
    if (cluster_compute_next_hop_from(
            mesh_cluster,
            slot->mesh_addr,
            PC_MASTER_LOCAL_ADDR,
            &next_hop,
            &metric) != 0) {
        return false;
    }

    return true;
}

static struct pc_master_node_slot *pc_master_allocate_node(
    struct pc_mesh_transport *transport,
    const uint8_t uid[MESH_UID_LEN])
{
    struct pc_master_node_slot *slot;
    size_t index;

    if (transport->assigned_node_count >= transport->target_node_count ||
        transport->assigned_node_count >= PC_MASTER_MAX_TARGET_NODES) {
        return NULL;
    }

    index = transport->assigned_node_count;
    slot = &transport->nodes[index];
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    memcpy(slot->uid, uid, MESH_UID_LEN);
    slot->mesh_addr = pc_master_node_addr_for_index(index);
    (void)snprintf(slot->name, sizeof(slot->name), PC_MASTER_NODE_NAME_FMT, index + 1u);

    ++transport->assigned_node_count;
    return slot;
}

static void pc_master_print_uid(const uint8_t uid[MESH_UID_LEN])
{
    size_t i;

    for (i = 0u; i < MESH_UID_LEN; ++i) {
        printf("%02x", uid[i]);
    }
}

/**
 * @brief 处理第一次 REGISTER（节点尚未分配地址）
 *
 * 当从机刚启动发出 REGISTER（src=dst=UNASSIGNED）时，PC 主控：
 * 1. 从帧中解析节点的 UID
 * 2. 按本次运行内的注册顺序分配或复用 Mesh 地址与节点名
 * 3. 构造并发送 ASSIGN 帧
 *
 * @param[in] transport     指向 pc_mesh_transport 的指针
 * @param[in] register_frame 已解码的 REGISTER 帧视图
 * @return 0 = 成功；负数错误码（-MESH_ERR_BAD_FRAME 表示帧解析失败）
 *
 * @note ASSIGN 成功发出后，PC 主控立即把该 UID/地址注册进 host runtime；
 *       后续二次 REGISTER 只作为旧固件的幂等刷新。
 */
static int pc_mesh_assign_bootstrap_node(
    struct pc_mesh_transport *transport,
    const struct mesh_frame_view *register_frame)
{
    struct mesh_register_payload register_payload;
    struct mesh_assign_payload assign_payload;
    struct pc_master_node_slot *slot;
    uint8_t assign_frame[PC_MASTER_FRAME_CAP];
    size_t assign_len = 0u;
    int rc;

    if (!mesh_parse_register(register_frame, &register_payload)) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    slot = pc_master_find_node_by_uid(transport, register_payload.uid);
    if (slot == NULL) {
        slot = pc_master_allocate_node(transport, register_payload.uid);
    }
    if (slot == NULL) {
        fprintf(stderr, "too many nodes registered; target node count is %zu\n", transport->target_node_count);
        return -(int)MESH_ERR_BUSY;
    }

    memset(&assign_payload, 0, sizeof(assign_payload));
    memcpy(assign_payload.uid, register_payload.uid, sizeof(assign_payload.uid));
    assign_payload.node_addr = slot->mesh_addr;
    assign_payload.lease_ms = PC_MASTER_ASSIGN_LEASE_MS;
    assign_payload.epoch = 1u;
    (void)snprintf(assign_payload.node_name, sizeof(assign_payload.node_name), "%s", slot->name);

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

    printf("[PC→mesh] ASSIGN %s addr=0x%02x uid=", slot->name, assign_payload.node_addr);
    pc_master_print_uid(slot->uid);
    printf(" (%zu bytes)\n", assign_len);
    rc = write_all(transport->fd, assign_frame, assign_len);
    if (rc != 0) {
        return rc;
    }

    if (transport->runtime != NULL) {
        rc = mesh_host_runtime_register_assigned_node(
            transport->runtime,
            slot->mesh_addr,
            slot->uid);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/**
 * @brief 兼容旧固件的二次 REGISTER 刷新
 *
 * 新协议不依赖二次 REGISTER；若旧固件仍发送，刷新 runtime/VFS 绑定即可。
 *
 * @param[in] transport  指向 pc_mesh_transport 的指针
 * @param[in] mesh_addr  从节点被分配的 Mesh 地址（MESH_ADDR_UNASSIGNED 为非法）
 * @return 0 = 成功；负数错误码
 */
static int pc_mesh_confirm_assigned_node(struct pc_mesh_transport *transport, uint8_t mesh_addr)
{
    struct cluster *mesh_cluster = cluster_config_mesh_cluster();
    struct pc_master_node_slot *slot;
    int rc;

    if (transport == NULL || mesh_addr == MESH_ADDR_UNASSIGNED) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    if (mesh_cluster == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    slot = pc_master_find_node_by_addr(transport, mesh_addr);
    if (slot == NULL) {
        fprintf(stderr, "confirmed REGISTER from unknown addr=0x%02x\n", mesh_addr);
        return -(int)MESH_ERR_BAD_FRAME;
    }

    rc = cluster_set_node_online(mesh_cluster, mesh_addr, true);
    if (rc != 0) {
        return rc;
    }
    if (transport->runtime != NULL) {
        rc = mesh_host_runtime_register_assigned_node(
            transport->runtime,
            slot->mesh_addr,
            slot->uid);
        if (rc != 0) {
            return rc;
        }
    }

    printf("[mesh→PC] refreshed %s addr=0x%02x from legacy REGISTER\n",
           slot->name,
           slot->mesh_addr);
    return 0;
}

/**
 * @brief 接收并处理一个 Mesh 帧（transport receive_frame 实现）
 *
 * 从串口读取一个完整的 Mesh 帧，解码帧头并打印诊断信息。
 * 对 REGISTER 有不同处理：
 * - 第一次（src=dst=UNASSIGNED）：分配地址并回送 ASSIGN，返回 -MESH_ERR_BUSY
 *   告知调用方本帧已被消费无需进一步处理。
 * - 旧固件二次 REGISTER（src≠UNASSIGNED, dst=UNASSIGNED）：幂等刷新，正常返回。
 *
 * @param[in]  transport_ctx 指向 pc_mesh_transport 的指针
 * @param[out] rx_data      接收缓冲区
 * @param[in]  rx_cap       接收缓冲区容量
 * @param[out] rx_len       成功时写入实际接收帧字节数；消费帧时清零
 * @return 0 = 成功，帧已放入 rx_data；-MESH_ERR_BUSY = 帧已消费（第一次 REGISTER）；
 *         负数 = 错误
 */
static int pc_mesh_receive_frame(
    void *transport_ctx,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len,
    uint8_t *out_ingress_port)
{
    struct pc_mesh_transport *transport = (struct pc_mesh_transport *)transport_ctx;
    struct mesh_frame_view view;
    int rc;

    if (transport == NULL || transport->fd < 0 || out_ingress_port == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    *out_ingress_port = MESH_PROCESSER_INGRESS_PORT_NONE;
    rc = pc_receive_mesh_frame(transport->fd, rx_data, rx_cap, rx_len);
    if (rc != 0) {
        return rc;
    }

    if (!mesh_decode_frame(rx_data, *rx_len, &view)) {
        fprintf(stderr, "[mesh→PC] failed to decode received mesh frame\n");
        return 0;
    }

        pc_mesh_log_frame_details("[mesh→PC]", &view);

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
        return rc;
    }

    transport->runtime = runtime;
    return 0;
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
static int wait_for_target(
    struct mesh_host_runtime *runtime,
    struct pc_mesh_transport *transport,
    const char *target)
{
    int i;

    printf("mesh runtime: waiting for %s; reset the slave now if it already booted\n", target);
    for (i = 0; i < PC_MASTER_DISCOVERY_POLLS; ++i) {
        int rc;

        if (!pc_master_target_routes_ready(transport, target)) {
            rc = mesh_host_runtime_poll_once(runtime);
            if (rc != 0 && rc != -(int)MESH_ERR_BUSY && rc != -(int)M9P_ERR_EBUSY) {
                fprintf(stderr, "mesh_host_runtime_poll_once failed: %d\n", rc);
                return rc;
            }
            continue;
        }

        rc = cluster_vfs_attach(target);

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

static int read_health_path(const char *target)
{
    char health_path[64];
    uint8_t health[PC_MASTER_HEALTH_CAP];
    uint16_t len = sizeof(health);
    int rc;

    rc = snprintf(health_path, sizeof(health_path), PC_MASTER_HEALTH_PATH_FMT, target);
    if (rc < 0 || (size_t)rc >= sizeof(health_path)) {
        fprintf(stderr, "health path too long for %s\n", target);
        return -(int)M9P_ERR_EINVAL;
    }

    rc = cluster_vfs_read_path(health_path, health, &len);
    if (rc != 0) {
        fprintf(stderr, "cluster_vfs_read_path(%s) failed: %d\n", health_path, rc);
        return rc;
    }

    printf("read %s: %.*s", health_path, (int)len, (const char *)health);
    if (len != 3u || memcmp(health, "ok\n", 3u) != 0) {
        fprintf(stderr, "unexpected %s payload\n", health_path);
        return -(int)M9P_ERR_EIO;
    }

    return 0;
}

static int read_routes_path(const char *target)
{
    char routes_path[64];
    uint8_t routes[PC_MASTER_ROUTES_CAP];
    uint16_t fd = 0u;
    uint16_t total = 0u;
    int rc;

    rc = snprintf(routes_path, sizeof(routes_path), PC_MASTER_ROUTES_PATH_FMT, target);
    if (rc < 0 || (size_t)rc >= sizeof(routes_path)) {
        fprintf(stderr, "routes path too long for %s\n", target);
        return -(int)M9P_ERR_EINVAL;
    }

    rc = cluster_vfs_open(routes_path, M9P_OREAD, &fd);
    if (rc != 0) {
        fprintf(stderr, "cluster_vfs_open(%s) failed: %d\n", routes_path, rc);
        return rc;
    }

    while (total < (uint16_t)(sizeof(routes) - 1u)) {
        uint16_t chunk = (uint16_t)(sizeof(routes) - 1u - total);

        if (chunk > 48u) {
            chunk = 48u;
        }

        rc = cluster_vfs_read(fd, routes + total, &chunk);
        if (rc != 0) {
            int close_rc = cluster_vfs_close(fd);

            (void)close_rc;
            fprintf(stderr, "cluster_vfs_read(%s) failed: %d\n", routes_path, rc);
            return rc;
        }
        if (chunk == 0u) {
            break;
        }
        total = (uint16_t)(total + chunk);
    }

    rc = cluster_vfs_close(fd);
    if (rc != 0) {
        fprintf(stderr, "cluster_vfs_close(%s) failed: %d\n", routes_path, rc);
        return rc;
    }

    routes[total] = '\0';
    printf("read %s:\n%s", routes_path, (const char *)routes);
    return 0;
}

static int read_text_file_chunked(
    const char *path,
    uint8_t *buffer,
    uint16_t buffer_cap,
    uint16_t max_chunk,
    uint16_t *out_len)
{
    uint16_t fd = 0u;
    uint16_t total = 0u;
    int rc;

    if (path == NULL || buffer == NULL || buffer_cap == 0u || out_len == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = cluster_vfs_open(path, M9P_OREAD, &fd);
    if (rc != 0) {
        return rc;
    }

    while (total < (uint16_t)(buffer_cap - 1u)) {
        uint16_t chunk = (uint16_t)(buffer_cap - 1u - total);

        if (chunk > max_chunk) {
            chunk = max_chunk;
        }

        rc = cluster_vfs_read(fd, buffer + total, &chunk);
        if (rc != 0) {
            (void)cluster_vfs_close(fd);
            return rc;
        }
        if (chunk == 0u) {
            break;
        }
        total = (uint16_t)(total + chunk);
    }

    rc = cluster_vfs_close(fd);
    if (rc != 0) {
        return rc;
    }

    buffer[total] = '\0';
    *out_len = total;
    return 0;
}

static int read_log_path(const char *target)
{
    char log_path[64];
    uint8_t log_text[PC_MASTER_LOG_CAP];
    uint16_t len = 0u;
    int rc;

    rc = snprintf(log_path, sizeof(log_path), PC_MASTER_LOG_PATH_FMT, target);
    if (rc < 0 || (size_t)rc >= sizeof(log_path)) {
        fprintf(stderr, "log path too long for %s\n", target);
        return -(int)M9P_ERR_EINVAL;
    }

    rc = read_text_file_chunked(log_path, log_text, sizeof(log_text), 48u, &len);
    if (rc != 0) {
        fprintf(stderr, "warning: cluster_vfs_read(%s) failed: %d\n", log_path, rc);
        return rc;
    }

    printf("read %s:\n", log_path);
    print_annotated_log_text((const char *)log_text);
    if (len == (uint16_t)(sizeof(log_text) - 1u)) {
        printf("warning: %s output truncated at %u bytes\n", log_path, (unsigned)len);
    }
    return 0;
}

static void read_all_logs(size_t target_node_count)
{
    size_t i;

    for (i = 0u; i < target_node_count; ++i) {
        char target[MESH_MAX_NODE_NAME + 1u];
        int rc = snprintf(target, sizeof(target), PC_MASTER_NODE_NAME_FMT, i + 1u);

        if (rc < 0 || (size_t)rc >= sizeof(target)) {
            fprintf(stderr, "target node name too long: index=%zu\n", i + 1u);
            continue;
        }

        (void)read_log_path(target);
    }
}

/**
 * @brief 执行 smoke test：attach + walk + open + read + clunk
 *
 * 依次：
 * 1. wait_for_target 等待每个目标节点被发现并挂载
 * 2. cluster_vfs_read_path("/mcuN/sys/health") 读取健康检查文件
 * 3. 验证每个响应内容为 "ok\n"，否则返回 EIO
 *
 * 成功时返回 0。
 *
 * @param[in] runtime 已初始化的 mesh_host_runtime
 * @param[in] target_node_count 本次要验证的目标节点数量
 * @return 0 = 测试通过；负数错误码
 */
static int run_dynamic_sequence(
    struct mesh_host_runtime *runtime,
    struct pc_mesh_transport *transport,
    size_t target_node_count)
{
    size_t i;

    for (i = 0u; i < target_node_count; ++i) {
        char target[MESH_MAX_NODE_NAME + 1u];
        int rc = snprintf(target, sizeof(target), PC_MASTER_NODE_NAME_FMT, i + 1u);

        if (rc < 0 || (size_t)rc >= sizeof(target)) {
            fprintf(stderr, "target node name too long: index=%zu\n", i + 1u);
            return -(int)M9P_ERR_EINVAL;
        }

        rc = wait_for_target(runtime, transport, target);
        if (rc != 0) {
            return rc;
        }

        rc = read_health_path(target);
        if (rc != 0) {
            return rc;
        }

        rc = read_routes_path(target);
        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

/**
 * @brief 程序入口
 *
 * 解析命令行参数，打开串口，初始化 runtime，执行 smoke test，最后清理。
 *
 * 用法：pc_master_emulator <serial-dev> [baud] [node-count]
 *   serial-dev：串口设备路径（必需）
 *   baud：波特率（可选，默认 1000000）
 *   node-count：要等待并检查的节点数（可选，默认 2）
 *
 * @param argc 命令行参数个数
 * @param argv 参数数组
 * @return 0 = 成功；1 = 运行失败；2 = 参数错误
 */
int main(int argc, char **argv)
{
    const char *serial_path;
    unsigned long baud = PC_MASTER_DEFAULT_BAUD;
    unsigned long node_count = PC_MASTER_DEFAULT_NODE_COUNT;
    struct mesh_host_runtime runtime;
    struct pc_mesh_transport transport;
    int rc;
    bool runtime_initialized = false;

    if (argc < 2 || argc > 4) {
        print_usage(argv[0]);
        return 2;
    }

    serial_path = argv[1];
    if (argc >= 3) {
        char *end = NULL;

        baud = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || baud == 0ul) {
            fprintf(stderr, "invalid baud rate: %s\n", argv[2]);
            return 2;
        }
    }
    if (argc == 4) {
        char *end = NULL;

        node_count = strtoul(argv[3], &end, 10);
        if (end == argv[3] || *end != '\0' ||
            node_count == 0ul ||
            node_count > (unsigned long)PC_MASTER_MAX_TARGET_NODES) {
            fprintf(stderr,
                    "invalid node-count: %s (range: 1..%u)\n",
                    argv[3],
                    (unsigned)PC_MASTER_MAX_TARGET_NODES);
            return 2;
        }
    }

    memset(&transport, 0, sizeof(transport));
    transport.fd = open_serial(serial_path, baud);
    transport.next_seq = 1u;
    transport.target_node_count = (size_t)node_count;
    if (transport.fd < 0) {
        return 1;
    }

    printf("pc_master_emulator: %s @ %lu baud, waiting for %lu node(s)\n",
           serial_path,
           baud,
           node_count);
    rc = init_runtime(&runtime, &transport);
    if (rc == 0) {
        runtime_initialized = true;
        rc = run_dynamic_sequence(&runtime, &transport, transport.target_node_count);
        read_all_logs(transport.target_node_count);
        if (rc == 0) {
            puts("pc_master_emulator: ok");
        }
        mesh_host_runtime_deinit(&runtime);
    }

    if (rc != 0 && runtime_initialized) {
        fprintf(stderr, "pc_master_emulator: failed rc=%d; logs were read before shutdown\n", rc);
    }

    close(transport.fd);
    return rc == 0 ? 0 : 1;
}
