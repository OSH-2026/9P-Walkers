/**
 * @file main.c
 * @brief PC 端 Mini9P 主控模拟器
 *
 * 通过串口与 STM32 从设备建立连接，执行完整的 Mini9P 协议测试序列：
 * attach → walk → open → read → clunk，并验证错误处理路径。
 *
 * @author OSH-2026 Team
 * @date 2026
 */

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

/** @brief 默认串口波特率（1 Mbps） */
#define PC_MASTER_DEFAULT_BAUD 1000000u

/** @brief 串口读/轮询超时时间，单位毫秒 */
#define PC_MASTER_TIMEOUT_MS 1000

/** @brief 收发缓冲区最大容量（字节） */
#define PC_MASTER_FRAME_CAP 512u

/** @brief Mini9P 根目录 fid */
#define PC_MASTER_ROOT_FID 0u

/** @brief /sys/health 文件测试用 fid */
#define PC_MASTER_HEALTH_FID 1u

/** @brief 故意传入的错误 fid，用于测试远端错误响应 */
#define PC_MASTER_BAD_FID 99u

/**
 * @brief 从字节缓冲区解码小端 16 位无符号整数
 * @param[in] data 指向两个字节的缓冲区
 * @return 解码后的 uint16_t 值
 */
static uint16_t get_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8);
}

/**
 * @brief 打印命令行用法信息到 stderr
 * @param[in] prog 程序名（argv[0]）
 */
static void print_usage(const char *prog)
{
    fprintf(stderr, "usage: %s <serial-dev> [baud]\n", prog);
    fprintf(stderr, "example: %s /dev/ttyUSB0 1000000\n", prog);
}

/**
 * @brief 将数值波特率转换为 termios speed_t
 * @param[in] baud 数值波特率（如 115200）
 * @param[out] out_speed 输出 termios 速率常量
 * @return true  支持并转换成功
 * @return false 不支持的波特率
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
 * @brief 打开并配置串口设备为原始 8N1 模式
 *
 * 关闭所有 termios 处理标志（回显、规范模式、流控等），
 * 设置非阻塞读、8 位数据位、无奇偶校验、1 位停止位。
 *
 * @param[in] path 串口设备路径（如 /dev/ttyUSB0）
 * @param[in] baud 波特率数值
 * @return 已配置串口的文件描述符，失败返回 -1
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
 * @brief 阻塞等待串口文件描述符变为可读（带超时）
 * @param[in] fd 串口文件描述符
 * @return 0 可读； -1 超时或 poll 出错
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
        fprintf(stderr, "serial read timeout\n");
        return -1;
    }
    if (rc < 0) {
        perror("poll");
        return -1;
    }
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        fprintf(stderr, "serial poll error: revents=0x%x\n", pfd.revents);
        return -1;
    }

    return 0;
}

/**
 * @brief 从文件描述符精确读取 len 字节（自动处理 EINTR / 短读）
 * @param[in] fd 文件描述符
 * @param[out] data 接收缓冲区
 * @param[in] len 期望读取的字节数
 * @return 0 成功； -1 失败
 */
static int read_exact(int fd, uint8_t *data, size_t len)
{
    size_t total = 0u;

    while (total < len) {
        ssize_t nread;

        if (wait_readable(fd) != 0) {
            return -1;
        }

        nread = read(fd, data + total, len - total);
        if (nread < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            perror("read");
            return -1;
        }
        if (nread == 0) {
            fprintf(stderr, "serial read returned EOF\n");
            return -1;
        }
        total += (size_t)nread;
    }

    return 0;
}

/**
 * @brief 向文件描述符写入全部数据并等待硬件发送完成（tcdrain）
 * @param[in] fd 文件描述符
 * @param[in] data 待写入数据
 * @param[in] len 数据长度
 * @return 0 成功； -1 失败
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
            return -1;
        }
        if (nwritten == 0) {
            fprintf(stderr, "serial write returned zero\n");
            return -1;
        }
        total += (size_t)nwritten;
    }

    if (tcdrain(fd) != 0) {
        perror("tcdrain");
        return -1;
    }
    return 0;
}

/**
 * @brief 读取一个完整的 Mini9P 帧（含 4 字节头部 + payload + CRC）
 *
 * 头部 magic 必须为 "9P"，随后根据帧长度字段读取剩余数据。
 *
 * @param[in] fd 串口文件描述符
 * @param[out] frame 帧数据缓冲区
 * @param[in] cap frame 缓冲区容量
 * @param[out] out_len 实际读取的帧长度（含 CRC）
 * @return 0 成功； -1 失败（magic 错误、长度非法、缓冲区不足或读出错）
 */
static int read_frame(int fd, uint8_t *frame, size_t cap, size_t *out_len)
{
    uint8_t header[4];
    uint16_t frame_len;
    size_t total_len;

    if (read_exact(fd, header, sizeof(header)) != 0) {
        return -1;
    }
    if (header[0] != (uint8_t)'9' || header[1] != (uint8_t)'P') {
        fprintf(stderr, "bad frame magic: %02x %02x\n", header[0], header[1]);
        return -1;
    }

    frame_len = get_le16(header + 2);
    if (frame_len < 4u) {
        fprintf(stderr, "bad frame length field: %u\n", frame_len);
        return -1;
    }

    total_len = (size_t)frame_len + 6u;
    if (total_len > cap) {
        fprintf(stderr, "frame too large: %zu > %zu\n", total_len, cap);
        return -1;
    }

    memcpy(frame, header, sizeof(header));
    if (read_exact(fd, frame + sizeof(header), total_len - sizeof(header)) != 0) {
        return -1;
    }

    *out_len = total_len;
    return 0;
}

/**
 * @brief 将 Mini9P 响应类型字节转换为可读的字符串常量
 * @param[in] type 响应类型字节
 * @return 对应的类型名称（如 "Rattach"），未知类型返回 "unknown"
 */
static const char *type_name(uint8_t type)
{
    switch (type) {
    case M9P_RATTACH:
        return "Rattach";
    case M9P_RWALK:
        return "Rwalk";
    case M9P_ROPEN:
        return "Ropen";
    case M9P_RREAD:
        return "Rread";
    case M9P_RCLUNK:
        return "Rclunk";
    case M9P_RERROR:
        return "Rerror";
    default:
        return "unknown";
    }
}

/**
 * @brief 发送一帧 Mini9P 请求并读取响应，进行 tag 匹配校验
 *
 * 输出方向标识 [PC→STM32] / [STM32→PC] 便于调试。
 *
 * @param[in] fd 串口文件描述符
 * @param[in] step 当前步骤描述字符串（用于日志）
 * @param[in] tx 待发送的完整帧数据
 * @param[in] tx_len 发送帧长度
 * @param[out] out_frame 解码后的响应帧视图
 * @param[out] rx 原始响应帧接收缓冲区（必须 >= rx_cap）
 * @param[in] rx_cap 接收缓冲区容量
 * @return 0 成功； -1 发送失败、读帧失败、解码失败或 tag 不匹配
 */
static int transact(int fd,
                    const char *step,
                    const uint8_t *tx,
                    size_t tx_len,
                    struct m9p_frame_view *out_frame,
                    uint8_t *rx,
                    size_t rx_cap)
{
    size_t rx_len = 0u;
    uint16_t expected_tag;

    printf("[PC→STM32] %s (%zu bytes)\n", step, tx_len);
    if (tx_len < M9P_FRAME_OVERHEAD) {
        fprintf(stderr, "request frame too short for %s\n", step);
        return -1;
    }
    expected_tag = get_le16(tx + 6u);

    if (write_all(fd, tx, tx_len) != 0) {
        return -1;
    }
    if (read_frame(fd, rx, rx_cap, &rx_len) != 0) {
        return -1;
    }
    if (!m9p_decode_frame(rx, rx_len, out_frame)) {
        fprintf(stderr, "[STM32→PC] failed to decode response for %s\n", step);
        return -1;
    }
    if (out_frame->tag != expected_tag) {
        fprintf(stderr, "[STM32→PC] tag mismatch for %s: expected %u, got %u\n", step, expected_tag, out_frame->tag);
        return -1;
    }

    printf("[STM32→PC] %s tag=%u payload=%u\n", type_name(out_frame->type), out_frame->tag, out_frame->payload_len);
    return 0;
}

/**
 * @brief 检查响应帧是否为 Rerror，若是则打印错误信息并返回 -1
 * @param[in] frame 解码后的响应帧视图
 * @return 0 不是 Rerror； -1 是 Rerror（已打印详情）
 */
static int fail_if_rerror(const struct m9p_frame_view *frame)
{
    struct m9p_error error;

    if (frame->type != M9P_RERROR) {
        return 0;
    }
    if (m9p_parse_rerror(frame, &error)) {
        fprintf(stderr, "remote error: %s (%u) %s\n", m9p_error_name(error.code), error.code, error.msg);
    } else {
        fprintf(stderr, "remote error: malformed Rerror\n");
    }
    return -1;
}

/**
 * @brief 断言响应帧为预期的 Rerror 并校验错误码
 * @param[in] frame 解码后的响应帧视图
 * @param[in] code 期望的 Mini9P 错误码
 * @return 0 错误码匹配； -1 类型不匹配或错误码不符
 */
static int expect_error(const struct m9p_frame_view *frame, uint16_t code)
{
    struct m9p_error error;

    if (frame->type != M9P_RERROR || !m9p_parse_rerror(frame, &error)) {
        fprintf(stderr, "expected Rerror %u, got %s\n", code, type_name(frame->type));
        return -1;
    }
    if (error.code != code) {
        fprintf(stderr, "expected Rerror %u, got %u (%s)\n", code, error.code, error.msg);
        return -1;
    }

    printf("   expected error: %s (%u)\n", m9p_error_name(error.code), error.code);
    return 0;
}

/**
 * @brief 执行完整的 Mini9P 测试序列
 *
 * 测试流程：
 *   1. Tattach  → 验证协商参数（msize、max_fids、feature bits）
 *   2. Twalk    → /sys/health
 *   3. Topen    → OREAD
 *   4. Tread    → 验证返回 "ok\n"
 *   5. Tclunk   → 释放 fid
 *   6. Twalk    → /missing（期望 ENOENT）
 *   7. Topen    → 非法 fid（期望 EFID）
 *
 * @param[in] fd 已打开的串口文件描述符
 * @return 0 全部测试通过； -1 任意步骤失败
 */
static int run_sequence(int fd)
{
    uint8_t tx[PC_MASTER_FRAME_CAP];
    uint8_t rx[PC_MASTER_FRAME_CAP];
    struct m9p_frame_view frame;
    size_t tx_len = 0u;
    uint16_t tag = 1u;

    struct m9p_attach_result attach_result;
    struct m9p_qid walk_qid;
    struct m9p_open_result open_result;
    uint8_t read_data[64];
    uint16_t read_count = 0u;

    if (!m9p_build_tattach(tag++, PC_MASTER_ROOT_FID, PC_MASTER_FRAME_CAP, 1u, 0u, tx, sizeof(tx), &tx_len)) {
        fprintf(stderr, "failed to build Tattach\n");
        return -1;
    }
    if (transact(fd, "Tattach", tx, tx_len, &frame, rx, sizeof(rx)) != 0 || fail_if_rerror(&frame) != 0) {
        return -1;
    }
    if (!m9p_parse_rattach(&frame, &attach_result)) {
        fprintf(stderr, "failed to parse Rattach\n");
        return -1;
    }
    printf("   msize=%u max_fids=%u inflight=%u features=0x%08lx\n",
           attach_result.negotiated_msize,
           attach_result.max_fids,
           attach_result.max_inflight,
           (unsigned long)attach_result.feature_bits);

    if (!m9p_build_twalk(tag++, PC_MASTER_ROOT_FID, PC_MASTER_HEALTH_FID, "/sys/health", tx, sizeof(tx), &tx_len)) {
        fprintf(stderr, "failed to build Twalk\n");
        return -1;
    }
    if (transact(fd, "Twalk /sys/health", tx, tx_len, &frame, rx, sizeof(rx)) != 0 || fail_if_rerror(&frame) != 0) {
        return -1;
    }
    if (!m9p_parse_rwalk(&frame, &walk_qid)) {
        fprintf(stderr, "failed to parse Rwalk\n");
        return -1;
    }
    printf("   qid type=0x%02x version=%u object=%lu\n",
           walk_qid.type,
           walk_qid.version,
           (unsigned long)walk_qid.object_id);

    if (!m9p_build_topen(tag++, PC_MASTER_HEALTH_FID, M9P_OREAD, tx, sizeof(tx), &tx_len)) {
        fprintf(stderr, "failed to build Topen\n");
        return -1;
    }
    if (transact(fd, "Topen OREAD", tx, tx_len, &frame, rx, sizeof(rx)) != 0 || fail_if_rerror(&frame) != 0) {
        return -1;
    }
    if (!m9p_parse_ropen(&frame, &open_result)) {
        fprintf(stderr, "failed to parse Ropen\n");
        return -1;
    }
    printf("   iounit=%u\n", open_result.iounit);

    if (!m9p_build_tread(tag++, PC_MASTER_HEALTH_FID, 0u, sizeof(read_data), tx, sizeof(tx), &tx_len)) {
        fprintf(stderr, "failed to build Tread\n");
        return -1;
    }
    if (transact(fd, "Tread /sys/health", tx, tx_len, &frame, rx, sizeof(rx)) != 0 || fail_if_rerror(&frame) != 0) {
        return -1;
    }
    if (!m9p_parse_rread(&frame, read_data, sizeof(read_data), &read_count)) {
        fprintf(stderr, "failed to parse Rread\n");
        return -1;
    }
    printf("   read %u byte(s): %.*s", read_count, (int)read_count, (const char *)read_data);
    if (read_count != 3u || memcmp(read_data, "ok\n", 3u) != 0) {
        fprintf(stderr, "unexpected /sys/health payload\n");
        return -1;
    }

    if (!m9p_build_tclunk(tag++, PC_MASTER_HEALTH_FID, tx, sizeof(tx), &tx_len)) {
        fprintf(stderr, "failed to build Tclunk\n");
        return -1;
    }
    if (transact(fd, "Tclunk", tx, tx_len, &frame, rx, sizeof(rx)) != 0 || fail_if_rerror(&frame) != 0) {
        return -1;
    }
    if (frame.type != M9P_RCLUNK || frame.payload_len != 0u) {
        fprintf(stderr, "expected empty Rclunk\n");
        return -1;
    }

    if (!m9p_build_twalk(tag++, PC_MASTER_ROOT_FID, 2u, "/missing", tx, sizeof(tx), &tx_len)) {
        fprintf(stderr, "failed to build missing Twalk\n");
        return -1;
    }
    if (transact(fd, "Twalk /missing", tx, tx_len, &frame, rx, sizeof(rx)) != 0 ||
        expect_error(&frame, M9P_ERR_ENOENT) != 0) {
        return -1;
    }

    if (!m9p_build_topen(tag++, PC_MASTER_BAD_FID, M9P_OREAD, tx, sizeof(tx), &tx_len)) {
        fprintf(stderr, "failed to build bad-fid Topen\n");
        return -1;
    }
    if (transact(fd, "Topen bad fid", tx, tx_len, &frame, rx, sizeof(rx)) != 0 ||
        expect_error(&frame, M9P_ERR_EFID) != 0) {
        return -1;
    }

    puts("pc_master_emulator: ok");
    return 0;
}

/**
 * @brief 程序入口：解析参数、打开串口、执行 Mini9P 测试序列
 *
 * 用法：pc_master_emulator <serial-dev> [baud]
 *
 * @param[in] argc 参数个数
 * @param[in] argv 参数数组，argv[1] 为串口路径，argv[2] 可选波特率
 * @return 0 测试全部通过； 1 运行时错误； 2 参数错误
 */
int main(int argc, char **argv)
{
    const char *serial_path;
    unsigned long baud = PC_MASTER_DEFAULT_BAUD;
    int fd;
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

    fd = open_serial(serial_path, baud);
    if (fd < 0) {
        return 1;
    }

    printf("pc_master_emulator: %s @ %lu baud\n", serial_path, baud);
    rc = run_sequence(fd);
    close(fd);
    return rc == 0 ? 0 : 1;
}
