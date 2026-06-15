#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cluster_config.h"
#include "cluster_host_vfs.h"

static int g_failures;

typedef void (*test_fn)(void);

/* mock_ctx 既保存模拟服务端的可控错误，也记录 cluster_vfs 实际发出的请求。
 * 测试用这些字段确认路径映射、fid 生命周期和错误处理是否符合预期。
 */
struct mock_ctx {
    int attach_count;
    int walk_count;
    int open_count;
    int read_count;
    int write_count;
    int stat_count;
    int clunk_count;
    char last_walk_path[M9P_MAX_PATH_LEN + 1u];
    uint16_t last_walk_newfid;
    uint16_t last_open_fid;
    uint8_t last_open_mode;
    uint16_t last_read_fid;
    uint32_t last_read_offset;
    uint16_t last_read_count;
    uint16_t last_write_fid;
    uint16_t last_write_count;
    uint16_t last_stat_fid;
    uint16_t last_clunk_fid;
    uint16_t stat_error_code;
    uint16_t clunk_error_code;
};

/* mini9P 协议使用小端序字段。测试里手写响应 payload，因此需要几个
 * 极小的编码/解码辅助函数，避免把协议细节散在每个测试分支里。
 */
static void put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xffu);
    dst[1] = (uint8_t)((value >> 8) & 0xffu);
}

static void put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xffu);
    dst[1] = (uint8_t)((value >> 8) & 0xffu);
    dst[2] = (uint8_t)((value >> 16) & 0xffu);
    dst[3] = (uint8_t)((value >> 24) & 0xffu);
}

static uint16_t get_le16(const uint8_t *src)
{
    return (uint16_t)src[0] | (uint16_t)((uint16_t)src[1] << 8);
}

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static void encode_qid(uint8_t *dst, uint8_t type, uint16_t version, uint32_t object_id)
{
    dst[0] = type;
    dst[1] = 0u;
    put_le16(dst + 2, version);
    put_le32(dst + 4, object_id);
}

/* 把模拟响应 payload 包成完整 mini9P 帧。真实客户端会校验 magic、长度、
 * tag、type 和 CRC，所以测试响应也必须走 m9p_encode_frame。
 */
static int send_frame(uint8_t type,
                      uint16_t tag,
                      const uint8_t *payload,
                      uint16_t payload_len,
                      uint8_t *rx,
                      size_t rx_cap,
                      size_t *rx_len)
{
    if (!m9p_encode_frame(type, tag, payload, payload_len, rx, rx_cap, rx_len))
    {
        return -(int)M9P_ERR_EMSIZE;
    }
    return 0;
}

/* 构造 Rerror，供 stat/clunk 等错误路径测试使用。 */
static int send_error(uint16_t tag,
                      uint16_t code,
                      uint8_t *rx,
                      size_t rx_cap,
                      size_t *rx_len)
{
    static const char msg[] = "mock error";
    uint8_t payload[3u + sizeof(msg)];

    put_le16(payload, code);
    payload[2] = (uint8_t)(sizeof(msg) - 1u);
    memcpy(payload + 3, msg, sizeof(msg) - 1u);
    return send_frame(M9P_RERROR, tag, payload, (uint16_t)(3u + sizeof(msg) - 1u), rx, rx_cap, rx_len);
}

/* 以下 send_r* 函数是一个最小 mini9P 服务端桩：只返回当前测试需要的字段。 */
static int send_rattach(uint16_t tag, uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
    uint8_t payload[16];

    put_le16(payload, M9P_DEFAULT_MSIZE);
    payload[2] = 16u;
    payload[3] = M9P_DEFAULT_INFLIGHT;
    put_le32(payload + 4, M9P_FEATURE_DIRECTORY_READ);
    encode_qid(payload + 8, M9P_QID_DIR, 1u, 1u);
    return send_frame(M9P_RATTACH, tag, payload, sizeof(payload), rx, rx_cap, rx_len);
}

static int send_rwalk(uint16_t tag, uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
    uint8_t payload[8];

    encode_qid(payload, M9P_QID_DEVICE, 1u, 42u);
    return send_frame(M9P_RWALK, tag, payload, sizeof(payload), rx, rx_cap, rx_len);
}

static int send_ropen(uint16_t tag, uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
    uint8_t payload[10];

    encode_qid(payload, M9P_QID_DEVICE, 2u, 42u);
    put_le16(payload + 8, 128u);
    return send_frame(M9P_ROPEN, tag, payload, sizeof(payload), rx, rx_cap, rx_len);
}

static int send_ropen_with_type(uint16_t tag, uint8_t qid_type, uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
    uint8_t payload[10];

    encode_qid(payload, qid_type, 2u, 42u);
    put_le16(payload + 8, 128u);
    return send_frame(M9P_ROPEN, tag, payload, sizeof(payload), rx, rx_cap, rx_len);
}

static int send_rread(uint16_t tag, uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
    static const uint8_t data[] = {'d', 'a', 't', 'a'};
    uint8_t payload[2u + sizeof(data)];

    put_le16(payload, (uint16_t)sizeof(data));
    memcpy(payload + 2, data, sizeof(data));
    return send_frame(M9P_RREAD, tag, payload, sizeof(payload), rx, rx_cap, rx_len);
}

static size_t encode_dirent(uint8_t *dst,
                            uint8_t type,
                            uint32_t object_id,
                            uint8_t flags,
                            const char *name)
{
    size_t name_len = strlen(name);

    encode_qid(dst, type, 1u, object_id);
    dst[8] = 0u;
    dst[9] = flags;
    dst[10] = (uint8_t)name_len;
    memcpy(dst + 11, name, name_len);
    return 11u + name_len;
}

static int send_rread_empty(uint16_t tag, uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
    uint8_t payload[2];

    put_le16(payload, 0u);
    return send_frame(M9P_RREAD, tag, payload, sizeof(payload), rx, rx_cap, rx_len);
}

static int send_rread_dir(uint16_t tag, uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
    uint8_t payload[64];
    size_t offset = 2u;

    offset += encode_dirent(payload + offset, M9P_QID_DEVICE, 51u, M9P_STAT_DEVICE, "temp");
    offset += encode_dirent(payload + offset, M9P_QID_DEVICE, 52u, M9P_STAT_DEVICE, "led");
    put_le16(payload, (uint16_t)(offset - 2u));
    return send_frame(M9P_RREAD, tag, payload, (uint16_t)offset, rx, rx_cap, rx_len);
}

static int send_rwrite(uint16_t tag, uint16_t count, uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
    uint8_t payload[2];

    put_le16(payload, count);
    return send_frame(M9P_RWRITE, tag, payload, sizeof(payload), rx, rx_cap, rx_len);
}

static int send_rstat(uint16_t tag, uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
    static const char name[] = "temp";
    uint8_t payload[19u + sizeof(name) - 1u];

    encode_qid(payload, M9P_QID_DEVICE, 3u, 42u);
    payload[8] = 0u;
    payload[9] = M9P_STAT_DEVICE;
    put_le32(payload + 10, 4u);
    put_le32(payload + 14, 123u);
    payload[18] = (uint8_t)(sizeof(name) - 1u);
    memcpy(payload + 19, name, sizeof(name) - 1u);
    return send_frame(M9P_RSTAT, tag, payload, sizeof(payload), rx, rx_cap, rx_len);
}

/* 模拟 transport 的核心：
 * 1. 解码客户端发来的 T* 请求；
 * 2. 记录路径、fid、mode 等关键参数；
 * 3. 返回对应 R* 响应或按 ctx 配置返回 Rerror。
 */
static int mock_transport(void *ctx_ptr,
                          const uint8_t *tx,
                          size_t tx_len,
                          uint8_t *rx,
                          size_t rx_cap,
                          size_t *rx_len)
{
    struct mock_ctx *ctx = (struct mock_ctx *)ctx_ptr;
    struct m9p_frame_view frame;

    if (!ctx || !m9p_decode_frame(tx, tx_len, &frame))
    {
        return -(int)M9P_ERR_EIO;
    }

    switch (frame.type)
    {
    case M9P_TATTACH:
        ctx->attach_count++;
        return send_rattach(frame.tag, rx, rx_cap, rx_len);

    case M9P_TWALK:
    {
        uint8_t path_len;

        if (frame.payload_len < 5u)
        {
            return send_error(frame.tag, M9P_ERR_EINVAL, rx, rx_cap, rx_len);
        }
        ctx->walk_count++;
        ctx->last_walk_newfid = get_le16(frame.payload + 2);
        path_len = frame.payload[4];
        if ((uint16_t)(5u + path_len) > frame.payload_len)
        {
            return send_error(frame.tag, M9P_ERR_EINVAL, rx, rx_cap, rx_len);
        }
        if (path_len == 0u)
        {
            strcpy(ctx->last_walk_path, "/");
        }
        else
        {
            memcpy(ctx->last_walk_path, frame.payload + 5, path_len);
            ctx->last_walk_path[path_len] = '\0';
        }
        return send_rwalk(frame.tag, rx, rx_cap, rx_len);
    }

    case M9P_TOPEN:
        if (frame.payload_len < 3u)
        {
            return send_error(frame.tag, M9P_ERR_EINVAL, rx, rx_cap, rx_len);
        }
        ctx->open_count++;
        ctx->last_open_fid = get_le16(frame.payload);
        ctx->last_open_mode = frame.payload[2];
        if (strcmp(ctx->last_walk_path, "/") == 0 ||
            strcmp(ctx->last_walk_path, "/dev") == 0)
        {
            return send_ropen_with_type(frame.tag, M9P_QID_DIR, rx, rx_cap, rx_len);
        }
        return send_ropen(frame.tag, rx, rx_cap, rx_len);

    case M9P_TREAD:
        if (frame.payload_len < 8u)
        {
            return send_error(frame.tag, M9P_ERR_EINVAL, rx, rx_cap, rx_len);
        }
        ctx->read_count++;
        ctx->last_read_fid = get_le16(frame.payload);
        ctx->last_read_offset = get_le32(frame.payload + 2);
        ctx->last_read_count = get_le16(frame.payload + 6);
        if (strcmp(ctx->last_walk_path, "/") == 0 ||
            strcmp(ctx->last_walk_path, "/dev") == 0)
        {
            if (ctx->last_read_offset == 0u)
            {
                return send_rread_dir(frame.tag, rx, rx_cap, rx_len);
            }
            return send_rread_empty(frame.tag, rx, rx_cap, rx_len);
        }
        return send_rread(frame.tag, rx, rx_cap, rx_len);

    case M9P_TWRITE:
        if (frame.payload_len < 8u)
        {
            return send_error(frame.tag, M9P_ERR_EINVAL, rx, rx_cap, rx_len);
        }
        ctx->write_count++;
        ctx->last_write_fid = get_le16(frame.payload);
        ctx->last_write_count = get_le16(frame.payload + 6);
        return send_rwrite(frame.tag, ctx->last_write_count, rx, rx_cap, rx_len);

    case M9P_TSTAT:
        if (frame.payload_len < 2u)
        {
            return send_error(frame.tag, M9P_ERR_EINVAL, rx, rx_cap, rx_len);
        }
        ctx->stat_count++;
        ctx->last_stat_fid = get_le16(frame.payload);
        if (ctx->stat_error_code != 0u)
        {
            return send_error(frame.tag, ctx->stat_error_code, rx, rx_cap, rx_len);
        }
        return send_rstat(frame.tag, rx, rx_cap, rx_len);

    case M9P_TCLUNK:
        if (frame.payload_len < 2u)
        {
            return send_error(frame.tag, M9P_ERR_EINVAL, rx, rx_cap, rx_len);
        }
        ctx->clunk_count++;
        ctx->last_clunk_fid = get_le16(frame.payload);
        if (ctx->clunk_error_code != 0u)
        {
            return send_error(frame.tag, ctx->clunk_error_code, rx, rx_cap, rx_len);
        }
        return send_frame(M9P_RCLUNK, frame.tag, NULL, 0u, rx, rx_cap, rx_len);

    default:
        return send_error(frame.tag, M9P_ERR_ENOTSUP, rx, rx_cap, rx_len);
    }
}

/* 简单断言工具。失败时累加 g_failures，让所有 case 都能跑完再统一返回。 */
static void expect_int(const char *name, int actual, int expected)
{
    if (actual != expected)
    {
        printf("FAIL %s: got %d expected %d\n", name, actual, expected);
        g_failures++;
    }
}

static void expect_u16(const char *name, uint16_t actual, uint16_t expected)
{
    if (actual != expected)
    {
        printf("FAIL %s: got %u expected %u\n", name, (unsigned)actual, (unsigned)expected);
        g_failures++;
    }
}

static void expect_str(const char *name, const char *actual, const char *expected)
{
    if (strcmp(actual, expected) != 0)
    {
        printf("FAIL %s: got \"%s\" expected \"%s\"\n", name, actual, expected);
        g_failures++;
    }
}

static void expect_mem(const char *name, const uint8_t *actual, const uint8_t *expected, size_t len)
{
    if (memcmp(actual, expected, len) != 0)
    {
        printf("FAIL %s: data mismatch\n", name);
        g_failures++;
    }
}

static void run_test(const char *name, test_fn fn)
{
    int failures_before = g_failures;

    printf("RUN  %s\n", name);
    fn();
    if (g_failures == failures_before)
    {
        printf("PASS %s\n", name);
    }
    else
    {
        printf("FAIL %s (%d new failure(s))\n", name, g_failures - failures_before);
    }
}

static void fill_uid(uint8_t uid[CLUSTER_VFS_UID_LEN], uint8_t seed)
{
    for (size_t i = 0; i < CLUSTER_VFS_UID_LEN; ++i)
    {
        uid[i] = (uint8_t)(seed + (uint8_t)i);
    }
}

static void setup_discovered_node(struct mock_ctx *ctx,
                                  struct m9p_client *client,
                                  uint8_t mesh_addr,
                                  uint8_t uid_seed,
                                  const char **out_name)
{
    uint8_t uid[CLUSTER_VFS_UID_LEN];
    bool reused = true;

    memset(ctx, 0, sizeof(*ctx));
    fill_uid(uid, uid_seed);
    expect_int("mesh host init", cluster_config_init_mesh_host(), 0);
    m9p_client_init(client, mock_transport, ctx);
    expect_int("discover node",
               cluster_config_on_node_discovered(mesh_addr, uid, client, out_name, &reused),
               0);
    expect_int("discover reused mapping", reused, 0);
}

/* 每个需要远端访问的测试都先通过 mesh discovery 建立 mcu1，并完成 attach。
 * 这样 case 本身可以专注验证 open/stat/read/write/close 的行为。
 */
static void setup_attached_node(struct mock_ctx *ctx, struct m9p_client *client)
{
    const char *name = NULL;

    setup_discovered_node(ctx, client, 0x11u, 0x70u, &name);
    expect_str("attached node name", name, "mcu1");
    expect_int("attach", cluster_vfs_attach(name), 0);
    expect_int("attach_count", ctx->attach_count, 1);
}

/* 同一 UID/地址重复发现应复用原节点名，而不是创建重复挂载点。 */
static void test_duplicate_discovery_reuses_route(void)
{
    struct mock_ctx ctx;
    struct m9p_client client;
    uint8_t uid[CLUSTER_VFS_UID_LEN];
    const char *name1 = NULL;
    const char *name2 = NULL;
    bool reused = false;

    memset(&ctx, 0, sizeof(ctx));
    fill_uid(uid, 0x60u);
    expect_int("duplicate init", cluster_config_init_mesh_host(), 0);
    m9p_client_init(&client, mock_transport, &ctx);
    expect_int("duplicate first discover",
               cluster_config_on_node_discovered(0x11u, uid, &client, &name1, &reused),
               0);
    expect_int("duplicate first reused", reused, 0);
    expect_int("duplicate second discover",
               cluster_config_on_node_discovered(0x11u, uid, &client, &name2, &reused),
               0);
    expect_int("duplicate second reused", reused, 1);
    expect_str("duplicate same name", name2, name1);
}

/* 同一 UID/地址/client 的重复 REGISTER 是心跳刷新，不能冲掉已 attach 会话。 */
static void test_duplicate_discovery_preserves_attached_session(void)
{
    struct mock_ctx ctx;
    struct m9p_client client;
    struct m9p_stat stat;
    uint8_t uid[CLUSTER_VFS_UID_LEN];
    const char *name = NULL;
    bool reused = false;

    setup_attached_node(&ctx, &client);
    fill_uid(uid, 0x70u);

    expect_int("duplicate attached rediscover",
               cluster_config_on_node_discovered(0x11u, uid, &client, &name, &reused),
               0);
    expect_int("duplicate attached reused", reused, 1);
    expect_str("duplicate attached same name", name, "mcu1");

    expect_int("duplicate attached attach", cluster_vfs_attach("mcu1"), 0);
    expect_int("duplicate attached no reattach", ctx.attach_count, 1);
    expect_int("duplicate attached stat", cluster_vfs_stat("/mcu1/dev/temp", &stat), 0);
    expect_int("duplicate attached attach count after stat", ctx.attach_count, 1);
}

/* runtime 可能因短暂链路不可达重置 client，会话下次访问必须自动重新 attach。 */
static void test_stale_attached_route_reattaches_client(void)
{
    static const uint8_t expected[] = {'d', 'a', 't', 'a'};
    struct mock_ctx ctx;
    struct m9p_client client;
    uint8_t buf[8] = {0};
    uint16_t len = sizeof(buf);

    setup_attached_node(&ctx, &client);
    m9p_client_reset_session(&client);

    expect_int("stale read_path", cluster_vfs_read_path("/mcu1/dev/temp", buf, &len), 0);
    expect_int("stale reattach count", ctx.attach_count, 2);
    expect_str("stale mapped path", ctx.last_walk_path, "/dev/temp");
    expect_u16("stale len", len, (uint16_t)sizeof(expected));
    expect_mem("stale data", buf, expected, sizeof(expected));
}

/* 新主机初始化不再预注册静态 mcu1，而是只启动 mesh cluster + VFS 桥接层。 */
static void test_mesh_host_init_starts_empty(void)
{
    struct cluster *mesh_cluster = NULL;

    expect_int("mesh host init", cluster_config_init_mesh_host(), 0);
    mesh_cluster = cluster_config_mesh_cluster();
    expect_int("mesh cluster exists", mesh_cluster != NULL, 1);
    expect_int("mesh host attach missing", cluster_vfs_attach("mcu1"), -(int)M9P_ERR_ENOENT);
}

/* 新发现的节点应分配新名字、保存 UID，并在 cluster 中可达。 */
static void test_discover_node_allocates_name_and_tracks_uid(void)
{
    struct mock_ctx ctx;
    struct m9p_client client;
    struct cluster *mesh_cluster;
    uint8_t uid[CLUSTER_VFS_UID_LEN];
    const char *name = NULL;
    bool reachable = false;

    fill_uid(uid, 0x10u);
    setup_discovered_node(&ctx, &client, 0x11u, 0x10u, &name);
    mesh_cluster = cluster_config_mesh_cluster();

    expect_str("discover name", name, "mcu1");
    expect_int("discover attach count", ctx.attach_count, 0);
    expect_int("discover reachable rc", cluster_can_reach(mesh_cluster, 0x11u, &reachable), 0);
    expect_int("discover reachable", reachable, 1);
    expect_int("discover attach", cluster_vfs_attach("mcu1"), 0);
}

/* 同一 UID 重连时应复用原节点名，但 9P 状态需要回到 NEW 并重新 attach。 */
static void test_rediscover_same_uid_reuses_name(void)
{
    struct mock_ctx ctx1;
    struct mock_ctx ctx2;
    struct m9p_client client1;
    struct m9p_client client2;
    uint8_t uid[CLUSTER_VFS_UID_LEN];
    const char *name1 = NULL;
    const char *name2 = NULL;
    bool reused = false;
    bool reachable = true;

    memset(&ctx1, 0, sizeof(ctx1));
    memset(&ctx2, 0, sizeof(ctx2));
    fill_uid(uid, 0x20u);

    expect_int("reuse init", cluster_config_init_mesh_host(), 0);
    m9p_client_init(&client1, mock_transport, &ctx1);
    expect_int("reuse discover first",
               cluster_config_on_node_discovered(0x11u, uid, &client1, &name1, &reused),
               0);
    expect_int("reuse first reused", reused, 0);
    expect_str("reuse first name", name1, "mcu1");
    expect_int("reuse first attach", cluster_vfs_attach(name1), 0);
    expect_int("reuse first attach count", ctx1.attach_count, 1);

    expect_int("reuse depart", cluster_config_on_node_departed(0x11u, &reachable), 0);
    expect_int("reuse depart reachable", reachable, 0);

    m9p_client_init(&client2, mock_transport, &ctx2);
    reused = false;
    expect_int("reuse discover second",
               cluster_config_on_node_discovered(0x22u, uid, &client2, &name2, &reused),
               0);
    expect_int("reuse second reused", reused, 1);
    expect_str("reuse second name", name2, "mcu1");
    expect_int("reuse second attach", cluster_vfs_attach("mcu1"), 0);
    expect_int("reuse second attach count", ctx2.attach_count, 1);
}

/* 节点离线后应摘图并把 VFS 会话回退到 NEW，同时使旧 fd 失效。 */
static void test_node_departure_marks_new_and_invalidates_fd(void)
{
    struct mock_ctx ctx;
    struct m9p_client client;
    const char *name = NULL;
    bool reachable = true;
    uint16_t fd = 0xffffu;

    setup_discovered_node(&ctx, &client, 0x11u, 0x30u, &name);
    expect_int("depart attach", cluster_vfs_attach(name), 0);
    expect_int("depart open", cluster_vfs_open("/mcu1/dev/temp", M9P_OREAD, &fd), 0);

    expect_int("depart event", cluster_config_on_node_departed(0x11u, &reachable), 0);
    expect_int("depart reachable", reachable, 0);
    expect_int("depart fd invalid", cluster_vfs_close(fd), -(int)M9P_ERR_EFID);

    fd = 0xffffu;
    expect_int("depart open blocked",
               cluster_vfs_open("/mcu1/dev/temp", M9P_OREAD, &fd),
               -(int)M9P_ERR_EAGAIN);
}

/* 链路图变化后应通过 mesh cluster 的连通性查询驱动 VFS 回退到离线/NEW。 */
static void test_refresh_connectivity_marks_offline_after_link_loss(void)
{
    struct mock_ctx ctx;
    struct m9p_client client;
    struct cluster *mesh_cluster;
    const char *name = NULL;
    bool reachable = true;

    setup_discovered_node(&ctx, &client, 0x11u, 0x40u, &name);
    expect_int("refresh attach", cluster_vfs_attach(name), 0);

    mesh_cluster = cluster_config_mesh_cluster();
    expect_int("refresh remove link",
               cluster_remove_link(mesh_cluster, 0x00u, 0x11u, true),
               0);
    expect_int("refresh rc", cluster_config_refresh_node_connectivity(0x11u, &reachable), 0);
    expect_int("refresh reachable", reachable, 0);
    expect_int("refresh attach blocked", cluster_vfs_attach("mcu1"), -(int)M9P_ERR_EAGAIN);
}

/* "/" 是 cluster_vfs 本地合成的虚拟根目录，不应该被转发给远端节点。 */
static void test_root_stat(void)
{
    struct m9p_stat stat;

    cluster_vfs_init();
    expect_int("root stat", cluster_vfs_stat("/", &stat), 0);
    expect_str("root stat name", stat.name, "/");
    expect_int("root stat dir flag", (stat.flags & M9P_STAT_DIR) != 0, 1);
}

/* 确认 /mcu10 不会因为前缀相同而误命中 mcu1。 */
static void test_path_boundary(void)
{
    struct mock_ctx ctx;
    struct m9p_client client;
    uint16_t fd = 0xffffu;

    setup_attached_node(&ctx, &client);
    expect_int("path boundary", cluster_vfs_open("/mcu10/dev/temp", M9P_OREAD, &fd), -(int)M9P_ERR_ENOENT);
    expect_int("boundary walk count", ctx.walk_count, 0);
}

/* 覆盖直连路由的主路径：
 * /mcu1/dev/temp 应剥掉 /mcu1 前缀，向远端发送 /dev/temp。
 */
static void test_open_read_close(void)
{
    static const uint8_t expected[] = {'d', 'a', 't', 'a'};
    struct mock_ctx ctx;
    struct m9p_client client;
    uint16_t fd = 0xffffu;
    uint8_t buf[8] = {0};
    uint16_t len = sizeof(buf);

    setup_attached_node(&ctx, &client);
    expect_int("open read path", cluster_vfs_open("/mcu1/dev/temp", M9P_OREAD, &fd), 0);
    expect_str("open mapped path", ctx.last_walk_path, "/dev/temp");
    expect_u16("open fid follows walk", ctx.last_open_fid, ctx.last_walk_newfid);
    expect_int("open mode", ctx.last_open_mode, M9P_OREAD);

    expect_int("read", cluster_vfs_read(fd, buf, &len), 0);
    expect_u16("read len", len, (uint16_t)sizeof(expected));
    expect_mem("read data", buf, expected, sizeof(expected));

    expect_int("close", cluster_vfs_close(fd), 0);
    expect_int("close clunk count", ctx.clunk_count, 1);
    expect_u16("close clunk fid", ctx.last_clunk_fid, ctx.last_open_fid);
    expect_int("second close", cluster_vfs_close(fd), -(int)M9P_ERR_EFID);
}

/* M9P_ORDWR 应同时允许读写。这里重点验证 write 走同一个远端 fid。 */
static void test_write_ordwr(void)
{
    struct mock_ctx ctx;
    struct m9p_client client;
    uint16_t fd = 0xffffu;
    uint16_t written = 0u;
    static const uint8_t data[] = {'o', 'k', '\n'};

    setup_attached_node(&ctx, &client);
    expect_int("open write path", cluster_vfs_open("/mcu1/dev/temp", M9P_ORDWR, &fd), 0);
    expect_int("write", cluster_vfs_write(fd, data, (uint16_t)sizeof(data), &written), 0);
    expect_u16("write count", written, (uint16_t)sizeof(data));
    expect_u16("write fid", ctx.last_write_fid, ctx.last_open_fid);
    expect_int("write close", cluster_vfs_close(fd), 0);
}

/* read_path 是 open/read/close 的便捷封装，应复用同一套路径映射和 fid 生命周期。 */
static void test_read_path_helper(void)
{
    static const uint8_t expected[] = {'d', 'a', 't', 'a'};
    struct mock_ctx ctx;
    struct m9p_client client;
    uint8_t buf[8] = {0};
    uint16_t len = sizeof(buf);

    setup_attached_node(&ctx, &client);
    expect_int("read_path", cluster_vfs_read_path("/mcu1/dev/temp", buf, &len), 0);
    expect_str("read_path mapped path", ctx.last_walk_path, "/dev/temp");
    expect_u16("read_path len", len, (uint16_t)sizeof(expected));
    expect_mem("read_path data", buf, expected, sizeof(expected));
    expect_int("read_path clunk count", ctx.clunk_count, 1);
    expect_u16("read_path clunk fid", ctx.last_clunk_fid, ctx.last_open_fid);
}

/* 对目录执行 read_path 应返回 EISDIR，而不是把目录项当作普通文件内容。 */
static void test_read_path_dir_returns_eisdir(void)
{
    struct mock_ctx ctx;
    struct m9p_client client;
    uint8_t buf[8] = {0};
    uint16_t len = sizeof(buf);

    setup_attached_node(&ctx, &client);
    expect_int("read_path dir", cluster_vfs_read_path("/mcu1/dev", buf, &len), -(int)M9P_ERR_EISDIR);
    expect_int("read_path dir no read", ctx.read_count, 0);
    expect_int("read_path dir clunk count", ctx.clunk_count, 1);
    expect_u16("read_path dir clunk fid", ctx.last_clunk_fid, ctx.last_open_fid);
}

/* write_path 是 open/write/close 的便捷封装，应使用写模式打开并自动释放 fid。 */
static void test_write_path_helper(void)
{
    static const uint8_t data[] = {'o', 'k', '\n'};
    struct mock_ctx ctx;
    struct m9p_client client;
    uint16_t written = 0u;

    setup_attached_node(&ctx, &client);
    expect_int("write_path", cluster_vfs_write_path("/mcu1/dev/temp", data, (uint16_t)sizeof(data), &written), 0);
    expect_str("write_path mapped path", ctx.last_walk_path, "/dev/temp");
    expect_int("write_path open mode", ctx.last_open_mode, M9P_OWRITE | M9P_OTRUNC);
    expect_u16("write_path count", written, (uint16_t)sizeof(data));
    expect_u16("write_path fid", ctx.last_write_fid, ctx.last_open_fid);
    expect_int("write_path clunk count", ctx.clunk_count, 1);
    expect_u16("write_path clunk fid", ctx.last_clunk_fid, ctx.last_open_fid);
}

/* 对目录执行 write_path 应返回 EISDIR，而不是向目录发起 write。 */
static void test_write_path_dir_returns_eisdir(void)
{
    static const uint8_t data[] = {'o', 'k', '\n'};
    struct mock_ctx ctx;
    struct m9p_client client;
    uint16_t written = 0u;

    setup_attached_node(&ctx, &client);
    expect_int("write_path dir",
               cluster_vfs_write_path("/mcu1/dev", data, (uint16_t)sizeof(data), &written),
               -(int)M9P_ERR_EISDIR);
    expect_int("write_path dir no write", ctx.write_count, 0);
    expect_int("write_path dir clunk count", ctx.clunk_count, 1);
    expect_u16("write_path dir clunk fid", ctx.last_clunk_fid, ctx.last_open_fid);
}

/* 根目录列表由 cluster_vfs 本地合成，展示已注册的集群挂载点。 */
static void test_list_root_mounts(void)
{
    struct mock_ctx ctx;
    struct m9p_client client;
    struct m9p_dirent entries[2];
    size_t count = 0u;

    memset(entries, 0, sizeof(entries));
    setup_discovered_node(&ctx, &client, 0x11u, 0x90u, NULL);

    expect_int("list root", cluster_vfs_list("/", entries, 2u, &count), 0);
    expect_int("list root count", (int)count, 1);
    expect_str("list root name", entries[0].name, "mcu1");
    expect_int("list root dir flag", (entries[0].flags & M9P_STAT_DIR) != 0, 1);
    expect_int("list root virtual flag", (entries[0].flags & M9P_STAT_VIRTUAL) != 0, 1);
    expect_int("list root no remote read", ctx.read_count, 0);
}

/* 首次访问远端挂载点时应自动 attach，而不是要求 shell 先手动 attach。 */
static void test_list_remote_root_auto_attaches(void)
{
    struct mock_ctx ctx;
    struct m9p_client client;
    struct m9p_dirent entries[4];
    size_t count = 0u;

    memset(entries, 0, sizeof(entries));
    setup_discovered_node(&ctx, &client, 0x11u, 0x91u, NULL);

    expect_int("list remote root", cluster_vfs_list("/mcu1/", entries, 4u, &count), 0);
    expect_int("list remote root attach count", ctx.attach_count, 1);
    expect_str("list remote root mapped path", ctx.last_walk_path, "/");
    expect_int("list remote root count", (int)count, 2);
    expect_str("list remote root first", entries[0].name, "temp");
    expect_str("list remote root second", entries[1].name, "led");
}

/* 远端目录列表通过 open/read/parse dirent/close 完成。 */
static void test_list_remote_dir(void)
{
    struct mock_ctx ctx;
    struct m9p_client client;
    struct m9p_dirent entries[4];
    size_t count = 0u;

    memset(entries, 0, sizeof(entries));
    setup_attached_node(&ctx, &client);

    expect_int("list remote", cluster_vfs_list("/mcu1/dev", entries, 4u, &count), 0);
    expect_str("list remote mapped path", ctx.last_walk_path, "/dev");
    expect_int("list remote count", (int)count, 2);
    expect_str("list remote first", entries[0].name, "temp");
    expect_str("list remote second", entries[1].name, "led");
    expect_int("list remote read count", ctx.read_count, 2);
    expect_int("list remote final offset", (int)ctx.last_read_offset, 2);
    expect_int("list remote clunk count", ctx.clunk_count, 1);
    expect_u16("list remote clunk fid", ctx.last_clunk_fid, ctx.last_open_fid);
}

/* 对普通文件执行 list 应返回 ENOTDIR，而不是把文件内容误解析为目录项。 */
static void test_list_file_returns_enotdir(void)
{
    struct mock_ctx ctx;
    struct m9p_client client;
    struct m9p_dirent entries[2];
    size_t count = 0u;

    setup_attached_node(&ctx, &client);
    expect_int("list file", cluster_vfs_list("/mcu1/dev/temp", entries, 2u, &count), -(int)M9P_ERR_ENOTDIR);
    expect_int("list file count", (int)count, 0);
    expect_int("list file no read", ctx.read_count, 0);
    expect_int("list file clunk count", ctx.clunk_count, 1);
}

/* stat 成功后使用的是临时 fid，必须在返回前 clunk 掉。 */
static void test_stat_success_clunks(void)
{
    struct mock_ctx ctx;
    struct m9p_client client;
    struct m9p_stat stat;

    setup_attached_node(&ctx, &client);
    expect_int("stat success", cluster_vfs_stat("/mcu1/dev/temp", &stat), 0);
    expect_str("stat mapped path", ctx.last_walk_path, "/dev/temp");
    expect_str("stat name", stat.name, "temp");
    expect_int("stat clunk count", ctx.clunk_count, 1);
    expect_u16("stat clunk fid", ctx.last_clunk_fid, ctx.last_stat_fid);
}

/* 即使远端 stat 返回 Rerror，cluster_vfs 也要释放 walk 出来的临时 fid。 */
static void test_stat_error_clunks(void)
{
    struct mock_ctx ctx;
    struct m9p_client client;
    struct m9p_stat stat;

    setup_attached_node(&ctx, &client);
    ctx.stat_error_code = M9P_ERR_EIO;
    expect_int("stat error", cluster_vfs_stat("/mcu1/dev/temp", &stat), -(int)M9P_ERR_EIO);
    expect_int("stat error clunk count", ctx.clunk_count, 1);
    expect_u16("stat error clunk fid", ctx.last_clunk_fid, ctx.last_stat_fid);
}

/* close 必须先释放本地 fd，同时把远端 clunk 错误返回给调用者。 */
static void test_close_returns_clunk_error(void)
{
    struct mock_ctx ctx;
    struct m9p_client client;
    uint16_t fd = 0xffffu;

    setup_attached_node(&ctx, &client);
    expect_int("open before clunk error", cluster_vfs_open("/mcu1/dev/temp", M9P_OWRITE, &fd), 0);
    ctx.clunk_error_code = M9P_ERR_EIO;
    expect_int("close clunk error", cluster_vfs_close(fd), -(int)M9P_ERR_EIO);
    expect_int("close freed fd", cluster_vfs_close(fd), -(int)M9P_ERR_EFID);
}

/* detach 也必须拒绝断开仍被打开 fd 使用的路由。 */
static void test_detach_busy_until_close(void)
{
    struct mock_ctx ctx;
    struct m9p_client client;
    uint16_t fd = 0xffffu;

    setup_attached_node(&ctx, &client);
    expect_int("detach busy open", cluster_vfs_open("/mcu1/dev/temp", M9P_OREAD, &fd), 0);
    expect_int("detach busy", cluster_vfs_detach("mcu1"), -(int)M9P_ERR_EBUSY);
    expect_int("detach busy close", cluster_vfs_close(fd), 0);
    expect_int("detach after close", cluster_vfs_detach("mcu1"), 0);
    expect_int("detach reattach", cluster_vfs_attach("mcu1"), 0);
}

int main(void)
{
    printf("cluster_vfs test runner start\n");

    run_test("test_duplicate_discovery_reuses_route", test_duplicate_discovery_reuses_route);
    run_test("test_duplicate_discovery_preserves_attached_session", test_duplicate_discovery_preserves_attached_session);
    run_test("test_stale_attached_route_reattaches_client", test_stale_attached_route_reattaches_client);
    run_test("test_mesh_host_init_starts_empty", test_mesh_host_init_starts_empty);
    run_test("test_discover_node_allocates_name_and_tracks_uid", test_discover_node_allocates_name_and_tracks_uid);
    run_test("test_rediscover_same_uid_reuses_name", test_rediscover_same_uid_reuses_name);
    run_test("test_node_departure_marks_new_and_invalidates_fd", test_node_departure_marks_new_and_invalidates_fd);
    run_test("test_refresh_connectivity_marks_offline_after_link_loss", test_refresh_connectivity_marks_offline_after_link_loss);
    run_test("test_root_stat", test_root_stat);
    run_test("test_path_boundary", test_path_boundary);
    run_test("test_open_read_close", test_open_read_close);
    run_test("test_write_ordwr", test_write_ordwr);
    run_test("test_read_path_helper", test_read_path_helper);
    run_test("test_read_path_dir_returns_eisdir", test_read_path_dir_returns_eisdir);
    run_test("test_write_path_helper", test_write_path_helper);
    run_test("test_write_path_dir_returns_eisdir", test_write_path_dir_returns_eisdir);
    run_test("test_list_root_mounts", test_list_root_mounts);
    run_test("test_list_remote_root_auto_attaches", test_list_remote_root_auto_attaches);
    run_test("test_list_remote_dir", test_list_remote_dir);
    run_test("test_list_file_returns_enotdir", test_list_file_returns_enotdir);
    run_test("test_stat_success_clunks", test_stat_success_clunks);
    run_test("test_stat_error_clunks", test_stat_error_clunks);
    run_test("test_close_returns_clunk_error", test_close_returns_clunk_error);
    run_test("test_detach_busy_until_close", test_detach_busy_until_close);

    if (g_failures != 0)
    {
        printf("cluster_vfs tests failed: %d\n", g_failures);
        return 1;
    }

    printf("cluster_vfs tests passed\n");
    return 0;
}
