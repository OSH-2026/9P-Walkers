#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cluster_config.h"
#include "cluster_host_vfs.h"
#include "mesh_host_runtime.h"

#define TEST_FRAME_CAP (MESH_FRAME_OVERHEAD + MESH_MAX_PAYLOAD_LEN)
#define TEST_QUEUE_CAP 64u

static int g_failures;

typedef void (*test_fn)(void);

/*
 * fake_mesh_io 是本测试文件里的最小 raw transport 桩。
 *
 * 它不模拟真实串口时序，只提供两类能力：
 * 1. 记录 runtime 发出的完整 mesh 帧以及它选择的 next_hop；
 * 2. 让测试提前把“后续会从链路收到的 mesh 帧”按顺序压进 RX 队列。
 *
 * 这样我们就能精确验证：
 * - runtime 是否真的发出了 mesh MINI9P 帧；
 * - 发给最终节点时选到的 next_hop 是否正确；
 * - 在等待某个 R* 期间，是否还能消费途中夹进来的 REGISTER/LINK_STATE。
 */
struct fake_mesh_io {
    uint8_t rx_frames[TEST_QUEUE_CAP][TEST_FRAME_CAP];
    size_t rx_lens[TEST_QUEUE_CAP];
    uint8_t rx_ingress_ports[TEST_QUEUE_CAP];
    size_t rx_head;
    size_t rx_count;

    uint8_t tx_frames[TEST_QUEUE_CAP][TEST_FRAME_CAP];
    size_t tx_lens[TEST_QUEUE_CAP];
    uint8_t tx_next_hops[TEST_QUEUE_CAP];
    size_t tx_count;
};

static void failf(const char *label, const char *detail)
{
    ++g_failures;
    printf("FAIL %s: %s\n", label, detail);
}

static void expect_int(const char *label, int actual, int expected)
{
    if (actual != expected) {
        char detail[128];

        snprintf(detail, sizeof(detail), "actual=%d expected=%d", actual, expected);
        failf(label, detail);
    }
}

static void expect_u16(const char *label, uint16_t actual, uint16_t expected)
{
    if (actual != expected) {
        char detail[128];

        snprintf(detail, sizeof(detail), "actual=%u expected=%u", (unsigned)actual, (unsigned)expected);
        failf(label, detail);
    }
}

static void expect_mem(const char *label, const uint8_t *actual, const uint8_t *expected, size_t len)
{
    if (memcmp(actual, expected, len) != 0) {
        failf(label, "memory differs");
    }
}

static void run_test(const char *name, test_fn fn)
{
    printf("RUN  %s\n", name);
    fn();
    if (g_failures == 0) {
        printf("PASS %s\n", name);
    }
}

static void fake_mesh_io_reset(struct fake_mesh_io *io)
{
    memset(io, 0, sizeof(*io));
}

static void fake_mesh_io_clear_tx(struct fake_mesh_io *io)
{
    if (io == NULL) {
        return;
    }

    memset(io->tx_frames, 0, sizeof(io->tx_frames));
    memset(io->tx_lens, 0, sizeof(io->tx_lens));
    memset(io->tx_next_hops, 0, sizeof(io->tx_next_hops));
    io->tx_count = 0u;
}

static int fake_send_frame(
    void *transport_ctx,
    uint8_t next_hop,
    const uint8_t *tx_data,
    size_t tx_len)
{
    struct fake_mesh_io *io = (struct fake_mesh_io *)transport_ctx;

    if (io == NULL || tx_data == NULL || tx_len == 0u || tx_len > TEST_FRAME_CAP) {
        return -(int)M9P_ERR_EINVAL;
    }
    if (io->tx_count >= TEST_QUEUE_CAP) {
        return -(int)M9P_ERR_EBUSY;
    }

    memcpy(io->tx_frames[io->tx_count], tx_data, tx_len);
    io->tx_lens[io->tx_count] = tx_len;
    io->tx_next_hops[io->tx_count] = next_hop;
    ++io->tx_count;
    return 0;
}

static int fake_receive_frame(
    void *transport_ctx,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len,
    uint8_t *out_ingress_port)
{
    struct fake_mesh_io *io = (struct fake_mesh_io *)transport_ctx;
    size_t slot;

    if (io == NULL || rx_data == NULL || rx_len == NULL || out_ingress_port == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    if (io->rx_count == 0u) {
        *out_ingress_port = MESH_PROCESSER_INGRESS_PORT_NONE;
        return -(int)M9P_ERR_EAGAIN;
    }

    slot = io->rx_head % TEST_QUEUE_CAP;
    if (io->rx_lens[slot] > rx_cap) {
        return -(int)M9P_ERR_EMSIZE;
    }

    memcpy(rx_data, io->rx_frames[slot], io->rx_lens[slot]);
    *rx_len = io->rx_lens[slot];
    *out_ingress_port = io->rx_ingress_ports[slot];
    io->rx_head = (io->rx_head + 1u) % TEST_QUEUE_CAP;
    --io->rx_count;
    return 0;
}

static void push_rx_frame_from_port(
    struct fake_mesh_io *io,
    const uint8_t *frame,
    size_t frame_len,
    uint8_t ingress_port)
{
    size_t slot;

    if (io == NULL || frame == NULL || frame_len == 0u || frame_len > TEST_FRAME_CAP) {
        failf("push_rx_frame", "invalid input");
        return;
    }
    if (io->rx_count >= TEST_QUEUE_CAP) {
        failf("push_rx_frame", "rx queue full");
        return;
    }

    slot = (io->rx_head + io->rx_count) % TEST_QUEUE_CAP;
    memcpy(io->rx_frames[slot], frame, frame_len);
    io->rx_lens[slot] = frame_len;
    io->rx_ingress_ports[slot] = ingress_port;
    ++io->rx_count;
}

static void push_rx_frame(struct fake_mesh_io *io, const uint8_t *frame, size_t frame_len)
{
    push_rx_frame_from_port(io, frame, frame_len, MESH_PROCESSER_INGRESS_PORT_NONE);
}

static void fill_uid(uint8_t uid[MESH_UID_LEN], uint8_t seed)
{
    size_t i;

    for (i = 0u; i < MESH_UID_LEN; ++i) {
        uid[i] = (uint8_t)(seed + (uint8_t)i);
    }
}

static void test_qid(struct m9p_qid *qid, uint8_t type, uint32_t object_id)
{
    memset(qid, 0, sizeof(*qid));
    qid->type = type;
    qid->version = 1u;
    qid->object_id = object_id;
}

static void build_register_frame(
    uint8_t src,
    const uint8_t uid[MESH_UID_LEN],
    uint8_t *out_frame,
    size_t *out_len)
{
    struct mesh_register_payload payload;

    memset(&payload, 0, sizeof(payload));
    memcpy(payload.uid, uid, MESH_UID_LEN);
    payload.boot_nonce = 0x11223344u;
    payload.capability_bits = 0x0001u;
    payload.port_bitmap = 0x01u;
    if (!mesh_build_register(src, 0x1000u, 6u, &payload, out_frame, TEST_FRAME_CAP, out_len)) {
        failf("build_register_frame", "mesh_build_register failed");
    }
}

static void build_register_frame_with_wifi(
    uint8_t src,
    const uint8_t uid[MESH_UID_LEN],
    bool wifi_supported,
    uint8_t port_bitmap,
    uint8_t *out_frame,
    size_t *out_len)
{
    struct mesh_register_payload payload;

    memset(&payload, 0, sizeof(payload));
    memcpy(payload.uid, uid, MESH_UID_LEN);
    payload.boot_nonce = 0x11223344u;
    payload.capability_bits = 0x0001u;
    payload.port_bitmap = port_bitmap;
    payload.wifi_supported = wifi_supported;
    if (!mesh_build_register(src, 0x1000u, 6u, &payload, out_frame, TEST_FRAME_CAP, out_len)) {
        failf("build_register_frame", "mesh_build_register failed");
    }
}

static void build_link_state_frame_with_port(
    uint8_t src,
    uint8_t neighbor,
    uint8_t link_up,
    uint8_t local_port,
    uint8_t *out_frame,
    size_t *out_len)
{
    struct mesh_link_state_payload payload;

    memset(&payload, 0, sizeof(payload));
    payload.neighbor = neighbor;
    payload.link_up = link_up;
    payload.quality = 1u;
    payload.local_port = local_port;
    if (!mesh_build_link_state(src, 0x00u, 0x2000u, 6u, &payload, out_frame, TEST_FRAME_CAP, out_len)) {
        failf("build_link_state_frame", "mesh_build_link_state failed");
    }
}

static void build_link_state_frame(
    uint8_t src,
    uint8_t neighbor,
    uint8_t link_up,
    uint8_t *out_frame,
    size_t *out_len)
{
    build_link_state_frame_with_port(
        src,
        neighbor,
        link_up,
        CLUSTER_PORT_INVALID,
        out_frame,
        out_len);
}

static void build_neighbor_probe_frame(
    uint8_t type,
    uint8_t src,
    uint8_t dst,
    uint8_t *out_frame,
    size_t *out_len)
{
    bool ok;

    if (type == MESH_TYPE_NEIGHBOR_PROBE_REQUEST) {
        ok = mesh_build_neighbor_probe_request(src, 0x3000u, 6u, out_frame, TEST_FRAME_CAP, out_len);
    } else {
        ok = mesh_build_neighbor_probe_response(src, dst, 0x3000u, 6u, out_frame, TEST_FRAME_CAP, out_len);
    }
    if (!ok) {
        failf("build_neighbor_probe_frame", "mesh_build_neighbor_probe failed");
    }
}

static void build_mini9p_response_mesh_frame(
    uint8_t src,
    uint8_t *mini9p_frame,
    size_t mini9p_len,
    uint8_t *out_frame,
    size_t *out_len)
{
    if (!mesh_build_mini9p_frame(
            src,
            0x00u,
            0x9001u,
            6u,
            0u,
            mini9p_frame,
            (uint16_t)mini9p_len,
            out_frame,
            TEST_FRAME_CAP,
            out_len)) {
        failf("build_mini9p_response_mesh_frame", "mesh_build_mini9p_frame failed");
    }
}

static void queue_rattach(struct fake_mesh_io *io, uint8_t src, uint16_t tag)
{
    struct m9p_qid root_qid;
    uint8_t mini9p_frame[M9P_CLIENT_BUFFER_CAP];
    uint8_t mesh_frame[TEST_FRAME_CAP];
    size_t mini9p_len = 0u;
    size_t mesh_len = 0u;

    test_qid(&root_qid, M9P_QID_DIR, 1u);
    if (!m9p_build_rattach(
            tag,
            M9P_DEFAULT_MSIZE,
            16u,
            M9P_DEFAULT_INFLIGHT,
            M9P_FEATURE_DIRECTORY_READ,
            &root_qid,
            mini9p_frame,
            sizeof(mini9p_frame),
            &mini9p_len)) {
        failf("queue_rattach", "m9p_build_rattach failed");
        return;
    }

    build_mini9p_response_mesh_frame(src, mini9p_frame, mini9p_len, mesh_frame, &mesh_len);
    push_rx_frame(io, mesh_frame, mesh_len);
}

static void queue_rwalk(struct fake_mesh_io *io, uint8_t src, uint16_t tag, uint32_t object_id)
{
    struct m9p_qid qid;
    uint8_t mini9p_frame[M9P_CLIENT_BUFFER_CAP];
    uint8_t mesh_frame[TEST_FRAME_CAP];
    size_t mini9p_len = 0u;
    size_t mesh_len = 0u;

    test_qid(&qid, M9P_QID_DEVICE, object_id);
    if (!m9p_build_rwalk(tag, &qid, mini9p_frame, sizeof(mini9p_frame), &mini9p_len)) {
        failf("queue_rwalk", "m9p_build_rwalk failed");
        return;
    }

    build_mini9p_response_mesh_frame(src, mini9p_frame, mini9p_len, mesh_frame, &mesh_len);
    push_rx_frame(io, mesh_frame, mesh_len);
}

static void queue_ropen(struct fake_mesh_io *io, uint8_t src, uint16_t tag, uint32_t object_id)
{
    struct m9p_qid qid;
    uint8_t mini9p_frame[M9P_CLIENT_BUFFER_CAP];
    uint8_t mesh_frame[TEST_FRAME_CAP];
    size_t mini9p_len = 0u;
    size_t mesh_len = 0u;

    test_qid(&qid, M9P_QID_DEVICE, object_id);
    if (!m9p_build_ropen(tag, &qid, 128u, mini9p_frame, sizeof(mini9p_frame), &mini9p_len)) {
        failf("queue_ropen", "m9p_build_ropen failed");
        return;
    }

    build_mini9p_response_mesh_frame(src, mini9p_frame, mini9p_len, mesh_frame, &mesh_len);
    push_rx_frame(io, mesh_frame, mesh_len);
}

static void queue_rread(struct fake_mesh_io *io, uint8_t src, uint16_t tag, const uint8_t *data, uint16_t len)
{
    uint8_t mini9p_frame[M9P_CLIENT_BUFFER_CAP];
    uint8_t mesh_frame[TEST_FRAME_CAP];
    size_t mini9p_len = 0u;
    size_t mesh_len = 0u;

    if (!m9p_build_rread(tag, data, len, mini9p_frame, sizeof(mini9p_frame), &mini9p_len)) {
        failf("queue_rread", "m9p_build_rread failed");
        return;
    }

    build_mini9p_response_mesh_frame(src, mini9p_frame, mini9p_len, mesh_frame, &mesh_len);
    push_rx_frame(io, mesh_frame, mesh_len);
}

static void queue_rclunk(struct fake_mesh_io *io, uint8_t src, uint16_t tag)
{
    uint8_t mini9p_frame[M9P_CLIENT_BUFFER_CAP];
    uint8_t mesh_frame[TEST_FRAME_CAP];
    size_t mini9p_len = 0u;
    size_t mesh_len = 0u;

    if (!m9p_build_rclunk(tag, mini9p_frame, sizeof(mini9p_frame), &mini9p_len)) {
        failf("queue_rclunk", "m9p_build_rclunk failed");
        return;
    }

    build_mini9p_response_mesh_frame(src, mini9p_frame, mini9p_len, mesh_frame, &mesh_len);
    push_rx_frame(io, mesh_frame, mesh_len);
}

static void init_runtime(struct mesh_host_runtime *runtime, struct fake_mesh_io *io)
{
    struct mesh_host_runtime_config config;

    fake_mesh_io_reset(io);
    expect_int("cluster_config_init_mesh_host", cluster_config_init_mesh_host(), 0);
    mesh_host_runtime_get_default_config(&config);
    config.mesh_cluster = cluster_config_mesh_cluster();
    config.send_frame = fake_send_frame;
    config.receive_frame = fake_receive_frame;
    config.transport_ctx = io;
    expect_int("mesh_host_runtime_init", mesh_host_runtime_init(runtime, &config), 0);
}

static void process_register(struct mesh_host_runtime *runtime, uint8_t src, uint8_t seed)
{
    uint8_t uid[MESH_UID_LEN];
    uint8_t frame[TEST_FRAME_CAP];
    size_t frame_len = 0u;

    fill_uid(uid, seed);
    build_register_frame(src, uid, frame, &frame_len);
    expect_int("process register", mesh_host_runtime_process_frame(runtime, frame, frame_len), 0);
}

static void process_register_with_metadata(
    struct mesh_host_runtime *runtime,
    uint8_t src,
    uint8_t seed,
    bool wifi_supported,
    uint8_t port_bitmap)
{
    uint8_t uid[MESH_UID_LEN];
    uint8_t frame[TEST_FRAME_CAP];
    size_t frame_len = 0u;

    fill_uid(uid, seed);
    build_register_frame_with_wifi(
        src,
        uid,
        wifi_supported,
        port_bitmap,
        frame,
        &frame_len);
    expect_int("process register", mesh_host_runtime_process_frame(runtime, frame, frame_len), 0);
}

static void process_register_with_wifi(
    struct mesh_host_runtime *runtime,
    uint8_t src,
    uint8_t seed,
    bool wifi_supported)
{
    process_register_with_metadata(
        runtime,
        src,
        seed,
        wifi_supported,
        wifi_supported ? CLUSTER_PORT_WIFI_MASK : 0x01u);
}

static const struct cluster_node *find_cluster_node(const struct cluster *cluster, uint8_t addr)
{
    size_t i;

    if (cluster == NULL) {
        return NULL;
    }

    for (i = 0u; i < CLUSTER_MAX_NODES; ++i) {
        if (cluster->nodes[i].valid && cluster->nodes[i].addr == addr) {
            return &cluster->nodes[i];
        }
    }

    return NULL;
}

/* 解析 io 中第 index 帧为 ASSIGN，并回填 payload。失败时计入 failure。 */
static void expect_assign_tx(
    struct fake_mesh_io *io,
    size_t index,
    struct mesh_assign_payload *out_assign)
{
    struct mesh_frame_view view;

    if (index >= io->tx_count) {
        failf("expect_assign_tx", "no such tx frame");
        return;
    }
    if (!mesh_decode_frame(io->tx_frames[index], io->tx_lens[index], &view)) {
        failf("expect_assign_tx", "decode failed");
        return;
    }
    expect_int("assign tx type", (int)view.type, (int)MESH_TYPE_ASSIGN);
    expect_int("assign tx dst is bootstrap", (int)view.dst, (int)MESH_ADDR_UNASSIGNED);
    if (!mesh_parse_assign(&view, out_assign)) {
        failf("expect_assign_tx", "parse assign failed");
    }
}

/*
 * bootstrap REGISTER（src=0xFF）：主机分配地址、回发 ASSIGN，并立即把节点注册进
 * VFS。验证：
 * 1. 发出一帧 ASSIGN，dst=0xFF、addr=0x11、UID 与 REGISTER 一致；
 * 2. 占位地址 0xFF 不会被建成 cluster 节点（控制器在 shared cluster 处理器之前拦截）；
 * 3. mcu1 随即可见（attach 返回 EAGAIN：已登记但尚未可达）；
 * 4. 同一 UID 重复 bootstrap REGISTER 幂等——复发同一地址、不漂移；
 * 5. 不同 UID 分配下一个地址 0x22。
 */
static void test_bootstrap_register_assigns_and_registers(void)
{
    struct mesh_host_runtime runtime;
    struct fake_mesh_io io;
    struct mesh_assign_payload assign;
    uint8_t uid[MESH_UID_LEN];
    bool online = true;

    init_runtime(&runtime, &io);

    /* 1. 第一次 bootstrap REGISTER -> ASSIGN(addr=0x11) + 注册。 */
    process_register(&runtime, MESH_ADDR_UNASSIGNED, 0x05u);
    expect_int("first assign tx count", (int)io.tx_count, 1);
    expect_assign_tx(&io, 0u, &assign);
    expect_int("first assign addr", (int)assign.node_addr, 0x11);
    fill_uid(uid, 0x05u);
    expect_mem("first assign uid", assign.uid, uid, MESH_UID_LEN);

    /* 2. 0xFF 不应被建成 cluster 节点。 */
    expect_int("bootstrap addr not a node",
               cluster_get_node_online(cluster_config_mesh_cluster(), MESH_ADDR_UNASSIGNED, &online),
               -(int)MESH_ERR_NO_ROUTE);

    /* 3. mcu1 已登记可见（EAGAIN：尚未可达）。 */
    expect_int("mcu1 registered after assign", cluster_vfs_attach("mcu1"), -(int)M9P_ERR_EAGAIN);

    /* 4. 同一 UID 重复 REGISTER：幂等复用 0x11。 */
    fake_mesh_io_clear_tx(&io);
    process_register(&runtime, MESH_ADDR_UNASSIGNED, 0x05u);
    expect_int("re-register tx count", (int)io.tx_count, 1);
    expect_assign_tx(&io, 0u, &assign);
    expect_int("re-register reuses addr", (int)assign.node_addr, 0x11);

    /* 5. 不同 UID -> 下一个地址 0x22。 */
    fake_mesh_io_clear_tx(&io);
    process_register(&runtime, MESH_ADDR_UNASSIGNED, 0x09u);
    expect_int("second uid tx count", (int)io.tx_count, 1);
    expect_assign_tx(&io, 0u, &assign);
    expect_int("second uid gets 0x22", (int)assign.node_addr, 0x22);
}

static void process_link_state(
    struct mesh_host_runtime *runtime,
    uint8_t src,
    uint8_t neighbor,
    uint8_t link_up)
{
    uint8_t frame[TEST_FRAME_CAP];
    size_t frame_len = 0u;

    build_link_state_frame(src, neighbor, link_up, frame, &frame_len);
    expect_int("process link_state", mesh_host_runtime_process_frame(runtime, frame, frame_len), 0);
}

static void process_link_state_with_port(
    struct mesh_host_runtime *runtime,
    uint8_t src,
    uint8_t neighbor,
    uint8_t link_up,
    uint8_t local_port)
{
    uint8_t frame[TEST_FRAME_CAP];
    size_t frame_len = 0u;

    build_link_state_frame_with_port(src, neighbor, link_up, local_port, frame, &frame_len);
    expect_int("process link_state", mesh_host_runtime_process_frame(runtime, frame, frame_len), 0);
}

static void expect_route_update_present(
    struct fake_mesh_io *io,
    uint8_t mesh_dst,
    uint8_t route_dst,
    uint8_t route_next_hop)
{
    size_t i;

    for (i = 0u; i < io->tx_count; ++i) {
        struct mesh_frame_view frame;
        struct mesh_route_update_payload payload;

        if (!mesh_decode_frame(io->tx_frames[i], io->tx_lens[i], &frame)) {
            continue;
        }
        if (frame.type != MESH_TYPE_ROUTE_UPDATE || frame.dst != mesh_dst) {
            continue;
        }
        if (!mesh_parse_route_update(&frame, &payload)) {
            continue;
        }
        if (payload.action == MESH_ROUTE_SET &&
            payload.dst == route_dst &&
            payload.next_hop == route_next_hop) {
            return;
        }
    }

    failf("expect_route_update_present", "matching ROUTE_UPDATE not found");
}

static void decode_last_tx(
    struct fake_mesh_io *io,
    struct mesh_frame_view *out_mesh,
    struct m9p_frame_view *out_m9p)
{
    if (io->tx_count == 0u) {
        failf("decode_last_tx", "no tx frame");
        return;
    }
    if (!mesh_decode_frame(io->tx_frames[io->tx_count - 1u], io->tx_lens[io->tx_count - 1u], out_mesh)) {
        failf("decode_last_tx", "mesh_decode_frame failed");
        return;
    }
    if (!m9p_decode_frame(out_mesh->payload, out_mesh->payload_len, out_m9p)) {
        failf("decode_last_tx", "m9p_decode_frame failed");
    }
}

static bool tx_has_route_update(
    struct fake_mesh_io *io,
    uint8_t owner,
    uint8_t route_dst,
    uint8_t route_next_hop)
{
    size_t i;

    for (i = 0u; i < io->tx_count; ++i) {
        struct mesh_frame_view frame;
        struct mesh_route_update_payload payload;

        if (!mesh_decode_frame(io->tx_frames[i], io->tx_lens[i], &frame)) {
            continue;
        }
        if (frame.type != MESH_TYPE_ROUTE_UPDATE || frame.dst != owner) {
            continue;
        }
        if (!mesh_parse_route_update(&frame, &payload)) {
            continue;
        }
        if (payload.dst == route_dst &&
            payload.next_hop == route_next_hop &&
            payload.action == MESH_ROUTE_SET) {
            return true;
        }
    }

    return false;
}

static void expect_route_update(
    const char *label,
    struct fake_mesh_io *io,
    uint8_t owner,
    uint8_t route_dst,
    uint8_t route_next_hop)
{
    if (!tx_has_route_update(io, owner, route_dst, route_next_hop)) {
        failf(label, "route update not found");
    }
}

static void expect_no_route_update(
    const char *label,
    struct fake_mesh_io *io,
    uint8_t owner,
    uint8_t route_dst,
    uint8_t route_next_hop)
{
    if (tx_has_route_update(io, owner, route_dst, route_next_hop)) {
        failf(label, "unexpected route update found");
    }
}

static void test_neighbor_probe_request_gets_host_response_without_topology(void)
{
    struct mesh_host_runtime runtime;
    struct fake_mesh_io io;
    struct mesh_frame_view response;
    uint8_t frame[TEST_FRAME_CAP];
    size_t frame_len = 0u;
    bool reachable = true;

    init_runtime(&runtime, &io);
    build_neighbor_probe_frame(MESH_TYPE_NEIGHBOR_PROBE_REQUEST, 0x11u, MESH_ADDR_UNASSIGNED, frame, &frame_len);

    expect_int("probe request", mesh_host_runtime_process_frame(&runtime, frame, frame_len), 0);
    expect_int("probe response tx count", (int)io.tx_count, 1);
    expect_int("probe response next hop", io.tx_next_hops[0], 0x11u);
    if (!mesh_decode_frame(io.tx_frames[0], io.tx_lens[0], &response)) {
        failf("probe response decode", "mesh_decode_frame failed");
        return;
    }
    expect_int("probe response type", response.type, MESH_TYPE_NEIGHBOR_PROBE_RESPONSE);
    expect_int("probe response src", response.src, MESH_ADDR_HOST);
    expect_int("probe response dst", response.dst, MESH_ADDR_UNASSIGNED);
    expect_int("probe no topology rc", cluster_can_reach(cluster_config_mesh_cluster(), 0x11u, &reachable), 0);
    expect_int("probe no topology", reachable, 0);
}

static void test_neighbor_probe_request_poll_path_gets_host_response(void)
{
    struct mesh_host_runtime runtime;
    struct fake_mesh_io io;
    struct mesh_frame_view response;
    uint8_t frame[TEST_FRAME_CAP];
    size_t frame_len = 0u;

    init_runtime(&runtime, &io);
    build_neighbor_probe_frame(MESH_TYPE_NEIGHBOR_PROBE_REQUEST, 0x11u, MESH_ADDR_UNASSIGNED, frame, &frame_len);
    push_rx_frame(&io, frame, frame_len);

    expect_int("probe request poll", mesh_host_runtime_poll_once(&runtime), 0);
    expect_int("probe poll tx count", (int)io.tx_count, 1);
    if (!mesh_decode_frame(io.tx_frames[0], io.tx_lens[0], &response)) {
        failf("probe poll response decode", "mesh_decode_frame failed");
        return;
    }
    expect_int("probe poll response type", response.type, MESH_TYPE_NEIGHBOR_PROBE_RESPONSE);
    expect_int("probe poll response src", response.src, MESH_ADDR_HOST);
    expect_int("probe poll response dst", response.dst, MESH_ADDR_UNASSIGNED);
}

static void test_neighbor_probe_response_is_consumed_by_host(void)
{
    struct mesh_host_runtime runtime;
    struct fake_mesh_io io;
    uint8_t frame[TEST_FRAME_CAP];
    size_t frame_len = 0u;
    bool reachable = true;

    init_runtime(&runtime, &io);
    build_neighbor_probe_frame(MESH_TYPE_NEIGHBOR_PROBE_RESPONSE, 0x11u, MESH_ADDR_UNASSIGNED, frame, &frame_len);

    expect_int("probe response consume", mesh_host_runtime_process_frame(&runtime, frame, frame_len), 0);
    expect_int("probe response consume tx count", (int)io.tx_count, 0);
    expect_int("probe response no topology rc", cluster_can_reach(cluster_config_mesh_cluster(), 0x11u, &reachable), 0);
    expect_int("probe response no topology", reachable, 0);
}

/*
 * REGISTER 必须能在 runtime 中自动落到 VFS，
 * 但这里还不应该凭空伪造一条 host <-> node 直连边。
 *
 * 这条测试验证两个点：
 * 1. 广播式 REGISTER（dst=0xFF）现在确实会被主机处理；
 * 2. 仅靠 REGISTER 本身，不会把节点错误地直接标成“从 cluster 已可达”。
 */
static void test_register_broadcast_updates_vfs_without_direct_link(void)
{
    struct mesh_host_runtime runtime;
    struct fake_mesh_io io;
    bool reachable = true;

    init_runtime(&runtime, &io);
    process_register(&runtime, 0x11u, 0x10u);

    expect_int("register attach blocked", cluster_vfs_attach("mcu1"), -(int)M9P_ERR_EAGAIN);
    expect_int("register tx count", (int)io.tx_count, 0);
    expect_int("register reachable rc", cluster_can_reach(cluster_config_mesh_cluster(), 0x11u, &reachable), 0);
    expect_int("register reachable", reachable, 0);
}

/*
 * 真实 UART receive_frame 会告诉 runtime 这帧来自哪个物理口。
 * assigned REGISTER 若带着具体 ingress port 到达，就足以作为 host -> node
 * 的直连发送依据；否则 VFS 名字存在但 /mcu1/... 永远无法发起远端请求。
 */
static void test_assigned_register_poll_path_uses_ingress_as_direct_link(void)
{
    struct mesh_host_runtime runtime;
    struct fake_mesh_io io;
    uint8_t uid[MESH_UID_LEN];
    uint8_t frame[TEST_FRAME_CAP];
    size_t frame_len = 0u;
    bool reachable = false;

    init_runtime(&runtime, &io);
    fill_uid(uid, 0x12u);
    build_register_frame(0x11u, uid, frame, &frame_len);
    push_rx_frame_from_port(&io, frame, frame_len, 0u);

    expect_int("assigned register poll", mesh_host_runtime_poll_once(&runtime), 0);
    expect_int("assigned register reachable rc",
               cluster_can_reach(cluster_config_mesh_cluster(), 0x11u, &reachable),
               0);
    expect_int("assigned register reachable", reachable, 1);

    queue_rattach(&io, 0x11u, 1u);
    expect_int("assigned register attach", cluster_vfs_attach("mcu1"), 0);
    expect_int("assigned register attach next hop", io.tx_next_hops[io.tx_count - 1u], 0x11u);
}

static void test_assigned_register_restores_lost_direct_link(void)
{
    struct mesh_host_runtime runtime;
    struct fake_mesh_io io;
    struct cluster *mesh_cluster;
    uint8_t uid[MESH_UID_LEN];
    uint8_t frame[TEST_FRAME_CAP];
    size_t frame_len = 0u;
    bool reachable = true;

    init_runtime(&runtime, &io);
    process_register(&runtime, 0x11u, 0x13u);
    process_link_state(&runtime, 0x11u, 0x00u, 1u);
    mesh_cluster = cluster_config_mesh_cluster();

    expect_int("restore remove direct",
               cluster_remove_link(mesh_cluster, 0x00u, 0x11u, false),
               0);
    expect_int("restore rebuild", cluster_rebuild_routes(mesh_cluster), 0);
    expect_int("restore unreachable rc", cluster_can_reach(mesh_cluster, 0x11u, &reachable), 0);
    expect_int("restore unreachable", reachable, 0);

    fill_uid(uid, 0x13u);
    build_register_frame(0x11u, uid, frame, &frame_len);
    push_rx_frame_from_port(&io, frame, frame_len, 0u);

    expect_int("restore poll register", mesh_host_runtime_poll_once(&runtime), 0);
    expect_int("restore reachable rc", cluster_can_reach(mesh_cluster, 0x11u, &reachable), 0);
    expect_int("restore reachable", reachable, 1);
}

/*
 * 当节点通过 LINK_STATE 明确声明“我与 host 相连”后，
 * runtime 应推导出 host -> node 的反向可达边，并允许 VFS attach。
 *
 * 同时这条测试还验证：
 * - cluster_vfs_attach() 现在用的确实是 mesh-backed m9p_client；
 * - 发出去的是 mesh MINI9P 帧，而不是旧时代的 raw mini9P。
 */
static void test_link_state_to_host_enables_attach_over_mesh_client(void)
{
    struct mesh_host_runtime runtime;
    struct fake_mesh_io io;
    struct mesh_frame_view mesh_view;
    struct m9p_frame_view m9p_view;
    bool reachable = false;

    init_runtime(&runtime, &io);
    process_register(&runtime, 0x11u, 0x20u);
    process_link_state(&runtime, 0x11u, 0x00u, 1u);
    fake_mesh_io_clear_tx(&io);

    expect_int("direct reachable rc", cluster_can_reach(cluster_config_mesh_cluster(), 0x11u, &reachable), 0);
    expect_int("direct reachable", reachable, 1);

    queue_rattach(&io, 0x11u, 1u);
    expect_int("attach over mesh", cluster_vfs_attach("mcu1"), 0);
    expect_int("attach next hop", io.tx_next_hops[io.tx_count - 1u], 0x11u);

    decode_last_tx(&io, &mesh_view, &m9p_view);
    expect_int("attach mesh type", mesh_view.type, MESH_TYPE_MINI9P);
    expect_int("attach mesh dst", mesh_view.dst, 0x11u);
    expect_int("attach mini9p type", m9p_view.type, M9P_TATTACH);
    expect_u16("attach mini9p tag", m9p_view.tag, 1u);
}

static void test_duplicate_register_preserves_attached_client_session(void)
{
    static const uint8_t expected[] = {'o', 'k'};
    struct mesh_host_runtime runtime;
    struct fake_mesh_io io;
    struct mesh_frame_view mesh_view;
    struct m9p_frame_view m9p_view;
    uint8_t buf[8] = {0};
    uint16_t len = sizeof(buf);

    init_runtime(&runtime, &io);
    process_register(&runtime, 0x11u, 0x24u);
    process_link_state(&runtime, 0x11u, 0x00u, 1u);

    queue_rattach(&io, 0x11u, 1u);
    expect_int("dup register initial attach", cluster_vfs_attach("mcu1"), 0);
    fake_mesh_io_clear_tx(&io);

    process_register(&runtime, 0x11u, 0x24u);

    queue_rwalk(&io, 0x11u, 2u, 42u);
    queue_ropen(&io, 0x11u, 3u, 42u);
    queue_rread(&io, 0x11u, 4u, expected, (uint16_t)sizeof(expected));
    queue_rclunk(&io, 0x11u, 5u);

    expect_int("dup register read_path", cluster_vfs_read_path("/mcu1/sys/health", buf, &len), 0);
    expect_int("dup register tx count", (int)io.tx_count, 4);
    expect_u16("dup register read len", len, (uint16_t)sizeof(expected));
    expect_mem("dup register read data", buf, expected, sizeof(expected));

    if (!mesh_decode_frame(io.tx_frames[0], io.tx_lens[0], &mesh_view)) {
        failf("dup register first tx mesh decode", "mesh_decode_frame failed");
        return;
    }
    if (!m9p_decode_frame(mesh_view.payload, mesh_view.payload_len, &m9p_view)) {
        failf("dup register first tx mini9p decode", "m9p_decode_frame failed");
        return;
    }
    expect_int("dup register first tx type", m9p_view.type, M9P_TWALK);
    expect_u16("dup register first tx tag", m9p_view.tag, 2u);
}

static void test_attach_timeout_maps_mesh_busy_to_m9p_eagain(void)
{
    struct mesh_host_runtime runtime;
    struct fake_mesh_io io;

    init_runtime(&runtime, &io);
    process_register(&runtime, 0x11u, 0x21u);
    process_link_state(&runtime, 0x11u, 0x00u, 1u);

    expect_int("attach timeout", cluster_vfs_attach("mcu1"), -(int)M9P_ERR_EAGAIN);
}

/*
 * 这条测试覆盖真正的多跳访问主路径：
 * host -> mcu1 -> mcu2。
 *
 * 它验证：
 * 1. mcu2 虽然不是 host 直连，但在 mcu1 上报 11 -> 22 链路后仍可达；
 * 2. 对 /mcu2/... 的 attach/read_path 请求会按 cluster 选出的 next_hop=0x11 发送；
 * 3. VFS 拿到的数据来自 runtime 的 mesh-backed client，而不是 mock 直连客户端。
 */
static void test_routed_read_path_uses_cluster_next_hop(void)
{
    static const uint8_t expected[] = {'m', 'e', 's', 'h'};
    struct mesh_host_runtime runtime;
    struct fake_mesh_io io;
    uint8_t buf[8] = {0};
    uint16_t len = sizeof(buf);
    bool reachable = false;

    init_runtime(&runtime, &io);
    process_register(&runtime, 0x11u, 0x30u);
    process_link_state(&runtime, 0x11u, 0x00u, 1u);
    process_register(&runtime, 0x22u, 0x40u);
    process_link_state_with_port(&runtime, 0x11u, 0x22u, 1u, 1u);
    fake_mesh_io_clear_tx(&io);

    expect_int("route reachable rc", cluster_can_reach(cluster_config_mesh_cluster(), 0x22u, &reachable), 0);
    expect_int("route reachable", reachable, 1);

    queue_rattach(&io, 0x22u, 1u);
    queue_rwalk(&io, 0x22u, 2u, 42u);
    queue_ropen(&io, 0x22u, 3u, 42u);
    queue_rread(&io, 0x22u, 4u, expected, (uint16_t)sizeof(expected));
    queue_rclunk(&io, 0x22u, 5u);

    expect_int("route attach", cluster_vfs_attach("mcu2"), 0);
    expect_int("route read_path", cluster_vfs_read_path("/mcu2/dev/temp", buf, &len), 0);
    expect_u16("route read len", len, (uint16_t)sizeof(expected));
    expect_mem("route read data", buf, expected, sizeof(expected));
    expect_int("route attach next hop", io.tx_next_hops[io.tx_count - 5u], 0x11u);
    expect_int("route walk next hop", io.tx_next_hops[io.tx_count - 4u], 0x11u);
}

static void test_link_state_triggers_all_pairs_route_updates(void)
{
    struct mesh_host_runtime runtime;
    struct fake_mesh_io io;
    struct cluster *cluster;
    size_t baseline_tx_count;

    init_runtime(&runtime, &io);
    process_register(&runtime, 0x11u, 0x31u);
    process_register(&runtime, 0x22u, 0x32u);
    process_register(&runtime, 0x33u, 0x33u);

    cluster = cluster_config_mesh_cluster();
    expect_int("add host a", cluster_add_link(cluster, 0x00u, 0x11u, 1u, true), 0);
    expect_int("add a b", cluster_add_link(cluster, 0x11u, 0x22u, 1u, true), 0);

    baseline_tx_count = io.tx_count;
    process_link_state(&runtime, 0x22u, 0x33u, 1u);

    expect_int("route sync emitted", io.tx_count > baseline_tx_count, 1);
    expect_route_update("A to host", &io, 0x11u, 0x00u, 0x00u);
    expect_route_update("A to B", &io, 0x11u, 0x22u, 0x22u);
    expect_route_update("A to C", &io, 0x11u, 0x33u, 0x22u);
    expect_route_update("B to host", &io, 0x22u, 0x00u, 0x11u);
    expect_route_update("B to A", &io, 0x22u, 0x11u, 0x11u);
    expect_route_update("B to C", &io, 0x22u, 0x33u, 0x33u);
    expect_no_route_update("C no host before reverse", &io, 0x33u, 0x00u, 0x22u);
    expect_no_route_update("C no A before reverse", &io, 0x33u, 0x11u, 0x22u);
    expect_no_route_update("C no B before reverse", &io, 0x33u, 0x22u, 0x22u);

    baseline_tx_count = io.tx_count;
    process_link_state(&runtime, 0x33u, 0x22u, 1u);

    expect_int("reverse route sync emitted", io.tx_count > baseline_tx_count, 1);
    expect_route_update("C to host", &io, 0x33u, 0x00u, 0x22u);
    expect_route_update("C to A", &io, 0x33u, 0x11u, 0x22u);
    expect_route_update("C to B", &io, 0x33u, 0x22u, 0x22u);
}

/*
 * 同步请求不能把 runtime 变成“只会傻等自己响应”的单线程序列。
 *
 * 这里构造一种真实竞争场景：
 * - 主机正在等待 mcu1 的 Rattach；
 * - 中途链路上先到了一帧来自新节点的 REGISTER；
 * - runtime 必须先处理掉这个 REGISTER，把 mcu2 纳入 VFS，再继续等回 mcu1 的响应。
 */
static void test_request_loop_processes_register_before_response(void)
{
    struct mesh_host_runtime runtime;
    struct fake_mesh_io io;
    bool online = false;
    uint8_t uid2[MESH_UID_LEN];
    uint8_t register_frame[TEST_FRAME_CAP];
    size_t register_len = 0u;

    init_runtime(&runtime, &io);
    process_register(&runtime, 0x11u, 0x50u);
    process_link_state(&runtime, 0x11u, 0x00u, 1u);

    fill_uid(uid2, 0x60u);
    build_register_frame(0x22u, uid2, register_frame, &register_len);
    push_rx_frame(&io, register_frame, register_len);
    queue_rattach(&io, 0x11u, 1u);

    expect_int("interleave attach", cluster_vfs_attach("mcu1"), 0);
    expect_int("interleave node2 online rc", cluster_get_node_online(cluster_config_mesh_cluster(), 0x22u, &online), 0);
    expect_int("interleave node2 online", online, 1);
}

/*
 * topology 变化后不能只刷新消息源本身。
 *
 * 这里复用 host -> mcu1 -> mcu2 场景，再让 mcu1 上报 11 -> 22 断链。
 * runtime 在处理完 LINK_STATE 后必须批量刷新已知节点，使 mcu2 自动回退为：
 * - OFFLINE
 * - NEW
 */
static void test_link_loss_marks_downstream_node_offline(void)
{
    struct mesh_host_runtime runtime;
    struct fake_mesh_io io;
    bool reachable = true;

    init_runtime(&runtime, &io);
    process_register(&runtime, 0x11u, 0x70u);
    process_link_state(&runtime, 0x11u, 0x00u, 1u);
    process_register(&runtime, 0x22u, 0x80u);
    process_link_state(&runtime, 0x11u, 0x22u, 1u);

    queue_rattach(&io, 0x22u, 1u);
    expect_int("loss attach", cluster_vfs_attach("mcu2"), 0);

    process_link_state(&runtime, 0x11u, 0x22u, 0u);

    expect_int("loss node2 reachable rc", cluster_can_reach(cluster_config_mesh_cluster(), 0x22u, &reachable), 0);
    expect_int("loss node2 reachable", reachable, 0);
    expect_int("loss node2 attach blocked", cluster_vfs_attach("mcu2"), -(int)M9P_ERR_EAGAIN);
}

int main(void)
{
    printf("mesh_host_runtime test runner start\n");

    run_test("test_bootstrap_register_assigns_and_registers",
             test_bootstrap_register_assigns_and_registers);
    run_test("test_register_broadcast_updates_vfs_without_direct_link",
             test_register_broadcast_updates_vfs_without_direct_link);
    run_test("test_assigned_register_poll_path_uses_ingress_as_direct_link",
             test_assigned_register_poll_path_uses_ingress_as_direct_link);
    run_test("test_assigned_register_restores_lost_direct_link",
             test_assigned_register_restores_lost_direct_link);
    run_test("test_neighbor_probe_request_gets_host_response_without_topology",
             test_neighbor_probe_request_gets_host_response_without_topology);
    run_test("test_neighbor_probe_request_poll_path_gets_host_response",
             test_neighbor_probe_request_poll_path_gets_host_response);
    run_test("test_neighbor_probe_response_is_consumed_by_host",
             test_neighbor_probe_response_is_consumed_by_host);
    run_test("test_link_state_to_host_enables_attach_over_mesh_client",
             test_link_state_to_host_enables_attach_over_mesh_client);
    run_test("test_duplicate_register_preserves_attached_client_session",
             test_duplicate_register_preserves_attached_client_session);
    run_test("test_attach_timeout_maps_mesh_busy_to_m9p_eagain",
             test_attach_timeout_maps_mesh_busy_to_m9p_eagain);
    run_test("test_routed_read_path_uses_cluster_next_hop",
             test_routed_read_path_uses_cluster_next_hop);
    run_test("test_link_state_triggers_all_pairs_route_updates",
             test_link_state_triggers_all_pairs_route_updates);
    run_test("test_request_loop_processes_register_before_response",
             test_request_loop_processes_register_before_response);
    run_test("test_link_loss_marks_downstream_node_offline",
             test_link_loss_marks_downstream_node_offline);

    if (g_failures != 0) {
        printf("mesh_host_runtime tests failed: %d\n", g_failures);
        return 1;
    }

    printf("mesh_host_runtime tests passed\n");
    return 0;
}
