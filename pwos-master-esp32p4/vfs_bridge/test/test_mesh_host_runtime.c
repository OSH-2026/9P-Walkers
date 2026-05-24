#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cluster_config.h"
#include "cluster_vfs.h"
#include "mesh_host_runtime.h"

#define TEST_FRAME_CAP (MESH_FRAME_OVERHEAD + MESH_MAX_PAYLOAD_LEN)
#define TEST_QUEUE_CAP 32u

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
    size_t *rx_len)
{
    struct fake_mesh_io *io = (struct fake_mesh_io *)transport_ctx;
    size_t slot;

    if (io == NULL || rx_data == NULL || rx_len == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    if (io->rx_count == 0u) {
        return -(int)M9P_ERR_EAGAIN;
    }

    slot = io->rx_head % TEST_QUEUE_CAP;
    if (io->rx_lens[slot] > rx_cap) {
        return -(int)M9P_ERR_EMSIZE;
    }

    memcpy(rx_data, io->rx_frames[slot], io->rx_lens[slot]);
    *rx_len = io->rx_lens[slot];
    io->rx_head = (io->rx_head + 1u) % TEST_QUEUE_CAP;
    --io->rx_count;
    return 0;
}

static void push_rx_frame(struct fake_mesh_io *io, const uint8_t *frame, size_t frame_len)
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
    ++io->rx_count;
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

static void build_link_state_frame(
    uint8_t src,
    uint8_t neighbor,
    uint8_t link_up,
    uint8_t *out_frame,
    size_t *out_len)
{
    struct mesh_link_state_payload payload;

    memset(&payload, 0, sizeof(payload));
    payload.neighbor = neighbor;
    payload.link_up = link_up;
    payload.quality = 1u;
    if (!mesh_build_link_state(src, 0x00u, 0x2000u, 6u, &payload, out_frame, TEST_FRAME_CAP, out_len)) {
        failf("build_link_state_frame", "mesh_build_link_state failed");
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

    expect_int("direct reachable rc", cluster_can_reach(cluster_config_mesh_cluster(), 0x11u, &reachable), 0);
    expect_int("direct reachable", reachable, 1);

    queue_rattach(&io, 0x11u, 1u);
    expect_int("attach over mesh", cluster_vfs_attach("mcu1"), 0);
    expect_int("attach tx count", (int)io.tx_count, 1);
    expect_int("attach next hop", io.tx_next_hops[0], 0x11u);

    decode_last_tx(&io, &mesh_view, &m9p_view);
    expect_int("attach mesh type", mesh_view.type, MESH_TYPE_MINI9P);
    expect_int("attach mesh dst", mesh_view.dst, 0x11u);
    expect_int("attach mini9p type", m9p_view.type, M9P_TATTACH);
    expect_u16("attach mini9p tag", m9p_view.tag, 1u);
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
    process_link_state(&runtime, 0x11u, 0x22u, 1u);

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
    expect_int("route tx count", (int)io.tx_count, 5);
    expect_int("route attach next hop", io.tx_next_hops[0], 0x11u);
    expect_int("route walk next hop", io.tx_next_hops[1], 0x11u);
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

    run_test("test_register_broadcast_updates_vfs_without_direct_link",
             test_register_broadcast_updates_vfs_without_direct_link);
    run_test("test_link_state_to_host_enables_attach_over_mesh_client",
             test_link_state_to_host_enables_attach_over_mesh_client);
    run_test("test_routed_read_path_uses_cluster_next_hop",
             test_routed_read_path_uses_cluster_next_hop);
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
