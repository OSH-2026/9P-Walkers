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

#define PC_MASTER_DEFAULT_BAUD 1000000u
#define PC_MASTER_TIMEOUT_MS 1000
#define PC_MASTER_FRAME_CAP 512u
#define PC_MASTER_ROOT_FID 0u
#define PC_MASTER_HEALTH_FID 1u
#define PC_MASTER_BAD_FID 99u

static uint16_t get_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8);
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "usage: %s <serial-dev> [baud]\n", prog);
    fprintf(stderr, "example: %s /dev/ttyUSB0 1000000\n", prog);
}

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

    printf("-> %s (%zu bytes)\n", step, tx_len);
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
        fprintf(stderr, "<- failed to decode response for %s\n", step);
        return -1;
    }
    if (out_frame->tag != expected_tag) {
        fprintf(stderr, "<- tag mismatch for %s: expected %u, got %u\n", step, expected_tag, out_frame->tag);
        return -1;
    }

    printf("<- %s tag=%u payload=%u\n", type_name(out_frame->type), out_frame->tag, out_frame->payload_len);
    return 0;
}

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
