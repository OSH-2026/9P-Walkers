#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cluster_vfs.h"

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

static int send_rread(uint16_t tag, uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
    static const uint8_t data[] = {'d', 'a', 't', 'a'};
    uint8_t payload[2u + sizeof(data)];

    put_le16(payload, (uint16_t)sizeof(data));
    memcpy(payload + 2, data, sizeof(data));
    return send_frame(M9P_RREAD, tag, payload, sizeof(payload), rx, rx_cap, rx_len);
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
        return send_ropen(frame.tag, rx, rx_cap, rx_len);

    case M9P_TREAD:
        if (frame.payload_len < 8u)
        {
            return send_error(frame.tag, M9P_ERR_EINVAL, rx, rx_cap, rx_len);
        }
        ctx->read_count++;
        ctx->last_read_fid = get_le16(frame.payload);
        ctx->last_read_count = get_le16(frame.payload + 6);
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

/* 每个需要远端访问的测试都先建立一条 mcu1 直连路由，并完成 attach。
 * 这样 case 本身可以专注验证 open/stat/read/write/close 的行为。
 */
static void setup_attached_route(struct mock_ctx *ctx, struct m9p_client *client)
{
    memset(ctx, 0, sizeof(*ctx));
    cluster_vfs_init();
    m9p_client_init(client, mock_transport, ctx);
    expect_int("add_direct", cluster_vfs_add_direct("mcu1", client), 0);
    expect_int("attach", cluster_vfs_attach("mcu1"), 0);
    expect_int("attach_count", ctx->attach_count, 1);
}

/* 重复 target 会让 resolve_path 只命中第一条路由，后续条目变成不可达死路由。 */
static void test_duplicate_route(void)
{
    struct mock_ctx ctx;
    struct m9p_client client;

    memset(&ctx, 0, sizeof(ctx));
    cluster_vfs_init();
    m9p_client_init(&client, mock_transport, &ctx);
    expect_int("duplicate first add", cluster_vfs_add_direct("mcu1", &client), 0);
    expect_int("duplicate second add", cluster_vfs_add_direct("mcu1", &client), -(int)M9P_ERR_EBUSY);
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

    setup_attached_route(&ctx, &client);
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

    setup_attached_route(&ctx, &client);
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

    setup_attached_route(&ctx, &client);
    expect_int("open write path", cluster_vfs_open("/mcu1/dev/temp", M9P_ORDWR, &fd), 0);
    expect_int("write", cluster_vfs_write(fd, data, (uint16_t)sizeof(data), &written), 0);
    expect_u16("write count", written, (uint16_t)sizeof(data));
    expect_u16("write fid", ctx.last_write_fid, ctx.last_open_fid);
    expect_int("write close", cluster_vfs_close(fd), 0);
}

/* stat 成功后使用的是临时 fid，必须在返回前 clunk 掉。 */
static void test_stat_success_clunks(void)
{
    struct mock_ctx ctx;
    struct m9p_client client;
    struct m9p_stat stat;

    setup_attached_route(&ctx, &client);
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

    setup_attached_route(&ctx, &client);
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

    setup_attached_route(&ctx, &client);
    expect_int("open before clunk error", cluster_vfs_open("/mcu1/dev/temp", M9P_OWRITE, &fd), 0);
    ctx.clunk_error_code = M9P_ERR_EIO;
    expect_int("close clunk error", cluster_vfs_close(fd), -(int)M9P_ERR_EIO);
    expect_int("close freed fd", cluster_vfs_close(fd), -(int)M9P_ERR_EFID);
}

int main(void)
{
    printf("cluster_vfs test runner start\n");

    run_test("test_duplicate_route", test_duplicate_route);
    run_test("test_root_stat", test_root_stat);
    run_test("test_path_boundary", test_path_boundary);
    run_test("test_open_read_close", test_open_read_close);
    run_test("test_write_ordwr", test_write_ordwr);
    run_test("test_stat_success_clunks", test_stat_success_clunks);
    run_test("test_stat_error_clunks", test_stat_error_clunks);
    run_test("test_close_returns_clunk_error", test_close_returns_clunk_error);

    if (g_failures != 0)
    {
        printf("cluster_vfs tests failed: %d\n", g_failures);
        return 1;
    }

    printf("cluster_vfs tests passed\n");
    return 0;
}
