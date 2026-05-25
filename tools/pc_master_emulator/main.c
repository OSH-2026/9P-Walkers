/**
 * @file main.c
 * @brief PC 端动态 Mesh 主控模拟器
 *
 * 通过串口接收 raw mesh 帧，驱动主机侧 mesh runtime 自动发现节点，
 * 再通过 cluster_vfs 的 /mcuN/... 路径访问远端 Mini9P 文件。
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

#define PC_MASTER_DEFAULT_BAUD 1000000u
#define PC_MASTER_TIMEOUT_MS 1000
#define PC_MASTER_FRAME_CAP (MESH_FRAME_OVERHEAD + MESH_MAX_PAYLOAD_LEN)
#define PC_MASTER_TARGET "mcu1"
#define PC_MASTER_HEALTH_PATH "/mcu1/sys/health"
#define PC_MASTER_DISCOVERY_POLLS 60
#define PC_MASTER_HEALTH_CAP 64u
#define PC_MASTER_LOCAL_ADDR 0x00u
#define PC_MASTER_FIRST_NODE_ADDR 0x11u
#define PC_MASTER_ASSIGN_LEASE_MS 60000u

struct pc_mesh_transport {
    int fd;
    uint8_t assigned_addr;
    uint16_t next_seq;
};

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

static int pc_mesh_send_frame(void *transport_ctx, uint8_t next_hop, const uint8_t *tx_data, size_t tx_len)
{
    struct pc_mesh_transport *transport = (struct pc_mesh_transport *)transport_ctx;

    if (transport == NULL || transport->fd < 0 || tx_data == NULL || tx_len == 0u) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    printf("[PC→mesh] next_hop=0x%02x %zu bytes\n", next_hop, tx_len);
    return write_all(transport->fd, tx_data, tx_len);
}

static uint16_t pc_mesh_next_seq(struct pc_mesh_transport *transport)
{
    uint16_t seq = transport->next_seq;

    ++transport->next_seq;
    if (transport->next_seq == 0u) {
        transport->next_seq = 1u;
    }
    return seq;
}

static int pc_mesh_assign_bootstrap_node(
    struct pc_mesh_transport *transport,
    const struct mesh_frame_view *register_frame,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    struct mesh_register_payload register_payload;
    struct mesh_assign_payload assign_payload;
    struct cluster *mesh_cluster;
    uint8_t assign_frame[PC_MASTER_FRAME_CAP];
    size_t assign_len = 0u;
    int rc;

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
    rc = write_all(transport->fd, assign_frame, assign_len);
    if (rc != 0) {
        return rc;
    }

    mesh_cluster = cluster_config_mesh_cluster();
    if (mesh_cluster == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    rc = cluster_set_node_online(mesh_cluster, transport->assigned_addr, true);
    if (rc != 0) {
        return rc;
    }
    rc = cluster_add_link(mesh_cluster, PC_MASTER_LOCAL_ADDR, transport->assigned_addr, 1u, false);
    if (rc != 0) {
        return rc;
    }

    if (!mesh_build_register(
            transport->assigned_addr,
            register_frame->seq,
            register_frame->hop,
            &register_payload,
            rx_data,
            rx_cap,
            rx_len)) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    printf("mesh runtime: bootstrap REGISTER assigned src=0x%02x\n", transport->assigned_addr);
    return 0;
}

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
        rc = pc_mesh_assign_bootstrap_node(transport, &view, rx_data, rx_cap, rx_len);
        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

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

static int wait_for_target(struct mesh_host_runtime *runtime, const char *target)
{
    int i;

    printf("mesh runtime: waiting for %s\n", target);
    for (i = 0; i < PC_MASTER_DISCOVERY_POLLS; ++i) {
        int rc = cluster_vfs_attach(target);

        if (rc == 0) {
            printf("mesh runtime: %s attached\n", target);
            return 0;
        }
        if (rc != -(int)M9P_ERR_ENOENT && rc != -(int)M9P_ERR_EAGAIN && rc != -(int)M9P_ERR_EIO) {
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
