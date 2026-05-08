#include "mini9p_protocol.h"
#include "mini9p_server.h"

#include <stdio.h>
#include <string.h>

/* ---- test configuration ---- */

#define TEST_BUFFER_CAP 512u
#define TEST_ROOT_FID 0u
#define TEST_HEALTH_FID 1u
#define TEST_SYS_FID 2u
#define TEST_ROOT_AGAIN_FID 3u
#define TEST_SINK_FID 4u
#define TEST_BAD_FID 99u

/* ---- fake file tree ---- */

struct fake_node {
    const char *path;
    struct m9p_stat stat;
    const char *content;
};

static const struct fake_node g_nodes[] = {
    {
        "/",
        {
            {M9P_QID_DIR, 0u, 1u, 1u},
            M9P_SERVER_PERM_READ,
            M9P_STAT_DIR | M9P_STAT_VIRTUAL,
            1u,
            0u,
            "/",
        },
        NULL,
    },
    {
        "/sys",
        {
            {M9P_QID_DIR, 0u, 1u, 2u},
            M9P_SERVER_PERM_READ,
            M9P_STAT_DIR | M9P_STAT_VIRTUAL,
            1u,
            0u,
            "sys",
        },
        NULL,
    },
    {
        "/sys/health",
        {
            {M9P_QID_VIRTUAL | M9P_QID_READONLY, 0u, 1u, 3u},
            M9P_SERVER_PERM_READ,
            M9P_STAT_VIRTUAL,
            3u,
            0u,
            "health",
        },
        "ok\n",
    },
    {
        "/sys/sink",
        {
            {M9P_QID_VIRTUAL, 0u, 1u, 4u},
            M9P_SERVER_PERM_WRITE,
            M9P_STAT_VIRTUAL,
            0u,
            0u,
            "sink",
        },
        "",
    },
};

/* ---- fake server ops (callback implementations) ---- */

static const struct fake_node *find_node(const char *path)
{
    size_t i;

    for (i = 0u; i < sizeof(g_nodes) / sizeof(g_nodes[0]); ++i) {
        if (strcmp(g_nodes[i].path, path) == 0) {
            return &g_nodes[i];
        }
    }

    return NULL;
}

static int fake_stat(void *ctx, const char *path, struct m9p_stat *out_stat)
{
    const struct fake_node *node;

    (void)ctx;
    if (path == NULL || out_stat == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    node = find_node(path);
    if (node == NULL) {
        return -(int)M9P_ERR_ENOENT;
    }

    *out_stat = node->stat;
    return 0;
}

static int fake_open(void *ctx, const char *path, uint8_t mode, struct m9p_qid *out_qid, uint16_t *out_iounit)
{
    const struct fake_node *node;

    (void)ctx;
    (void)mode;
    if (path == NULL || out_qid == NULL || out_iounit == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    node = find_node(path);
    if (node == NULL) {
        return -(int)M9P_ERR_ENOENT;
    }

    *out_qid = node->stat.qid;
    *out_iounit = 64u;
    return 0;
}

static int fake_read(void *ctx,
                     const char *path,
                     uint32_t offset,
                     uint8_t mode,
                     uint8_t *out_data,
                     uint16_t out_cap,
                     uint16_t *out_count)
{
    const struct fake_node *node;
    size_t content_len;
    size_t count;

    (void)ctx;
    (void)mode;
    if (path == NULL || out_data == NULL || out_count == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    node = find_node(path);
    if (node == NULL) {
        return -(int)M9P_ERR_ENOENT;
    }
    if (node->content == NULL) {
        return -(int)M9P_ERR_EISDIR;
    }

    content_len = strlen(node->content);
    if (offset >= content_len) {
        *out_count = 0u;
        return 0;
    }

    count = content_len - (size_t)offset;
    if (count > out_cap) {
        count = out_cap;
    }

    memcpy(out_data, node->content + offset, count);
    *out_count = (uint16_t)count;
    return 0;
}

static int g_fake_write_calls;
static int g_fake_clunk_calls;

static int fake_write(void *ctx,
                      const char *path,
                      uint32_t offset,
                      uint8_t mode,
                      const uint8_t *data,
                      uint16_t count,
                      uint16_t *out_count)
{
    (void)ctx;
    (void)path;
    (void)offset;
    (void)mode;
    (void)data;

    ++g_fake_write_calls;
    if (out_count == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    *out_count = count;
    return 0;
}

static int fake_clunk(void *ctx, const char *path, bool was_open)
{
    (void)ctx;
    (void)path;
    (void)was_open;

    ++g_fake_clunk_calls;
    return 0;
}

/* ---- ops vtable ---- */

static const struct m9p_server_ops g_fake_ops = {
    fake_stat,
    fake_open,
    fake_read,
    fake_write,
    fake_clunk,
};

/* ---- test infrastructure (assertions, helpers) ---- */

static int g_failures;

static void expect_true(bool condition, const char *message)
{
    if (!condition) {
        ++g_failures;
        printf("FAIL: %s\n", message);
    }
}

static void init_server(struct m9p_server *server)
{
    struct m9p_server_config config;

    g_fake_write_calls = 0;
    g_fake_clunk_calls = 0;
    m9p_server_get_default_config(&config);
    config.ops = &g_fake_ops;
    config.max_msize = TEST_BUFFER_CAP;
    config.default_iounit = 64u;
    config.root_qid = g_nodes[0].stat.qid;
    expect_true(m9p_server_init(server, &config) == 0, "server init");
}

static bool transact(struct m9p_server *server,
                     const uint8_t *request,
                     size_t request_len,
                     uint8_t *response,
                     size_t response_cap,
                     struct m9p_frame_view *out_frame)
{
    size_t response_len = 0u;
    int rc;

    rc = m9p_server_handle_frame(server, request, request_len, response, response_cap, &response_len);
    if (rc != 0) {
        printf("handler rc=%d\n", rc);
        return false;
    }

    return m9p_decode_frame(response, response_len, out_frame);
}

static bool attach_server(struct m9p_server *server, uint8_t *request, uint8_t *response)
{
    struct m9p_frame_view frame;
    struct m9p_attach_result result;
    size_t request_len = 0u;
    const uint16_t tag = 1u;

    if (!m9p_build_tattach(
            tag,
            TEST_ROOT_FID,
            TEST_BUFFER_CAP,
            1u,
            0u,
            request,
            TEST_BUFFER_CAP,
            &request_len)) {
        return false;
    }

    if (!transact(server, request, request_len, response, TEST_BUFFER_CAP, &frame)) {
        return false;
    }

    expect_true(frame.tag == tag, "Rattach tag matches");
    expect_true(frame.type == M9P_RATTACH, "Tattach returns Rattach");
    expect_true(m9p_parse_rattach(&frame, &result), "Rattach parses");
    expect_true(result.negotiated_msize == TEST_BUFFER_CAP, "Rattach msize");
    return frame.type == M9P_RATTACH;
}

static bool walk_health(struct m9p_server *server, uint8_t *request, uint8_t *response)
{
    struct m9p_frame_view frame;
    struct m9p_qid qid;
    size_t request_len = 0u;
    const uint16_t tag = 2u;

    if (!m9p_build_twalk(
            tag,
            TEST_ROOT_FID,
            TEST_HEALTH_FID,
            "/sys/health",
            request,
            TEST_BUFFER_CAP,
            &request_len)) {
        return false;
    }

    if (!transact(server, request, request_len, response, TEST_BUFFER_CAP, &frame)) {
        return false;
    }

    expect_true(frame.tag == tag, "Rwalk tag matches");
    expect_true(frame.type == M9P_RWALK, "Twalk returns Rwalk");
    expect_true(m9p_parse_rwalk(&frame, &qid), "Rwalk parses");
    expect_true(qid.object_id == 3u, "Rwalk qid");
    return frame.type == M9P_RWALK;
}

static bool open_health(struct m9p_server *server, uint8_t *request, uint8_t *response)
{
    struct m9p_frame_view frame;
    struct m9p_open_result result;
    size_t request_len = 0u;
    const uint16_t tag = 3u;

    if (!m9p_build_topen(
            tag,
            TEST_HEALTH_FID,
            M9P_OREAD,
            request,
            TEST_BUFFER_CAP,
            &request_len)) {
        return false;
    }

    if (!transact(server, request, request_len, response, TEST_BUFFER_CAP, &frame)) {
        return false;
    }

    expect_true(frame.tag == tag, "Ropen tag matches");
    expect_true(frame.type == M9P_ROPEN, "Topen returns Ropen");
    expect_true(m9p_parse_ropen(&frame, &result), "Ropen parses");
    expect_true(result.iounit == 64u, "Ropen iounit");
    return frame.type == M9P_ROPEN;
}

/* ---- test cases ---- */

static void test_happy_path(void)
{
    struct m9p_server server;
    uint8_t request[TEST_BUFFER_CAP];
    uint8_t response[TEST_BUFFER_CAP];
    struct m9p_frame_view frame;
    uint8_t data[16];
    uint16_t count;
    size_t request_len = 0u;

    init_server(&server);
    expect_true(attach_server(&server, request, response), "attach happy path");
    expect_true(walk_health(&server, request, response), "walk health");
    expect_true(open_health(&server, request, response), "open health");

    expect_true(m9p_build_tread(
                    4u,
                    TEST_HEALTH_FID,
                    0u,
                    sizeof(data),
                    request,
                    sizeof(request),
                    &request_len),
                "build Tread");
    expect_true(transact(&server, request, request_len, response, sizeof(response), &frame), "Tread transact");
    expect_true(frame.type == M9P_RREAD, "Tread returns Rread");
    count = sizeof(data);
    expect_true(m9p_parse_rread(&frame, data, sizeof(data), &count), "Rread parses");
    expect_true(count == 3u, "Rread count");
    expect_true(memcmp(data, "ok\n", 3u) == 0, "Rread data");

    expect_true(m9p_build_tclunk(5u, TEST_HEALTH_FID, request, sizeof(request), &request_len), "build Tclunk");
    expect_true(transact(&server, request, request_len, response, sizeof(response), &frame), "Tclunk transact");
    expect_true(frame.type == M9P_RCLUNK, "Tclunk returns Rclunk");
}

static void test_missing_path(void)
{
    struct m9p_server server;
    uint8_t request[TEST_BUFFER_CAP];
    uint8_t response[TEST_BUFFER_CAP];
    struct m9p_frame_view frame;
    struct m9p_error error;
    size_t request_len = 0u;

    init_server(&server);
    expect_true(attach_server(&server, request, response), "attach before missing path");
    expect_true(m9p_build_twalk(
                    10u,
                    TEST_ROOT_FID,
                    TEST_HEALTH_FID,
                    "/missing",
                    request,
                    sizeof(request),
                    &request_len),
                "build missing Twalk");
    expect_true(transact(&server, request, request_len, response, sizeof(response), &frame), "missing Twalk transact");
    expect_true(frame.type == M9P_RERROR, "missing path returns Rerror");
    expect_true(m9p_parse_rerror(&frame, &error), "missing Rerror parses");
    expect_true(error.code == M9P_ERR_ENOENT, "missing path code ENOENT");
}

static void test_invalid_fid(void)
{
    struct m9p_server server;
    uint8_t request[TEST_BUFFER_CAP];
    uint8_t response[TEST_BUFFER_CAP];
    struct m9p_frame_view frame;
    struct m9p_error error;
    size_t request_len = 0u;

    init_server(&server);
    expect_true(attach_server(&server, request, response), "attach before invalid fid");
    expect_true(m9p_build_topen(
                    20u,
                    TEST_BAD_FID,
                    M9P_OREAD,
                    request,
                    sizeof(request),
                    &request_len),
                "build invalid fid Topen");
    expect_true(transact(&server, request, request_len, response, sizeof(response), &frame), "invalid fid transact");
    expect_true(frame.type == M9P_RERROR, "invalid fid returns Rerror");
    expect_true(m9p_parse_rerror(&frame, &error), "invalid fid Rerror parses");
    expect_true(error.code == M9P_ERR_EFID, "invalid fid code EFID");
}

static void test_absolute_root_walk(void)
{
    struct m9p_server server;
    uint8_t request[TEST_BUFFER_CAP];
    uint8_t response[TEST_BUFFER_CAP];
    struct m9p_frame_view frame;
    struct m9p_qid qid;
    size_t request_len = 0u;

    init_server(&server);
    expect_true(attach_server(&server, request, response), "attach before root walk");
    expect_true(m9p_build_twalk(
                    30u,
                    TEST_ROOT_FID,
                    TEST_SYS_FID,
                    "/sys",
                    request,
                    sizeof(request),
                    &request_len),
                "build sys Twalk");
    expect_true(transact(&server, request, request_len, response, sizeof(response), &frame), "sys Twalk transact");
    expect_true(frame.type == M9P_RWALK, "sys Twalk returns Rwalk");

    expect_true(m9p_build_twalk(
                    31u,
                    TEST_SYS_FID,
                    TEST_ROOT_AGAIN_FID,
                    "/",
                    request,
                    sizeof(request),
                    &request_len),
                "build root Twalk");
    expect_true(transact(&server, request, request_len, response, sizeof(response), &frame), "root Twalk transact");
    expect_true(frame.type == M9P_RWALK, "root Twalk returns Rwalk");
    expect_true(m9p_parse_rwalk(&frame, &qid), "root Rwalk parses");
    expect_true(qid.object_id == 1u, "absolute root walk returns root qid");
}

static void test_write_small_response_does_not_call_backend(void)
{
    struct m9p_server server;
    uint8_t request[TEST_BUFFER_CAP];
    uint8_t response[TEST_BUFFER_CAP];
    uint8_t tiny_response[M9P_FRAME_OVERHEAD + 1u];
    struct m9p_frame_view frame;
    size_t request_len = 0u;
    size_t response_len = 0u;
    int rc;
    const uint8_t data[] = {'x'};

    init_server(&server);
    expect_true(attach_server(&server, request, response), "attach before small write response");
    expect_true(m9p_build_twalk(
                    40u,
                    TEST_ROOT_FID,
                    TEST_SINK_FID,
                    "/sys/sink",
                    request,
                    sizeof(request),
                    &request_len),
                "build sink Twalk");
    expect_true(transact(&server, request, request_len, response, sizeof(response), &frame), "sink Twalk transact");
    expect_true(frame.type == M9P_RWALK, "sink Twalk returns Rwalk");

    expect_true(m9p_build_topen(
                    41u,
                    TEST_SINK_FID,
                    M9P_OWRITE,
                    request,
                    sizeof(request),
                    &request_len),
                "build sink Topen");
    expect_true(transact(&server, request, request_len, response, sizeof(response), &frame), "sink Topen transact");
    expect_true(frame.type == M9P_ROPEN, "sink Topen returns Ropen");

    expect_true(m9p_build_twrite(
                    42u,
                    TEST_SINK_FID,
                    0u,
                    data,
                    sizeof(data),
                    request,
                    sizeof(request),
                    &request_len),
                "build sink Twrite");
    rc = m9p_server_handle_frame(
        &server,
        request,
        request_len,
        tiny_response,
        sizeof(tiny_response),
        &response_len);
    expect_true(rc == -(int)M9P_ERR_EMSIZE, "small Rwrite buffer returns EMSIZE");
    expect_true(g_fake_write_calls == 0, "small Rwrite buffer skips backend write");
}

static void test_clunk_small_response_keeps_fid(void)
{
    struct m9p_server server;
    uint8_t request[TEST_BUFFER_CAP];
    uint8_t response[TEST_BUFFER_CAP];
    uint8_t tiny_response[M9P_FRAME_OVERHEAD - 1u];
    struct m9p_frame_view frame;
    size_t request_len = 0u;
    size_t response_len = 0u;
    int rc;

    init_server(&server);
    expect_true(attach_server(&server, request, response), "attach before small clunk response");
    expect_true(walk_health(&server, request, response), "walk before small clunk response");
    expect_true(open_health(&server, request, response), "open before small clunk response");
    g_fake_clunk_calls = 0;

    expect_true(m9p_build_tclunk(50u, TEST_HEALTH_FID, request, sizeof(request), &request_len), "build small Tclunk");
    rc = m9p_server_handle_frame(
        &server,
        request,
        request_len,
        tiny_response,
        sizeof(tiny_response),
        &response_len);
    expect_true(rc == -(int)M9P_ERR_EMSIZE, "small Rclunk buffer returns EMSIZE");
    expect_true(g_fake_clunk_calls == 0, "small Rclunk buffer skips backend clunk");

    expect_true(m9p_build_tclunk(51u, TEST_HEALTH_FID, request, sizeof(request), &request_len), "build retry Tclunk");
    expect_true(transact(&server, request, request_len, response, sizeof(response), &frame), "retry Tclunk transact");
    expect_true(frame.type == M9P_RCLUNK, "retry Tclunk returns Rclunk");
    expect_true(g_fake_clunk_calls == 1, "retry Tclunk calls backend once");
}

/* ---- test runner ---- */

int main(void)
{
    /* run each test case; failures accumulate in g_failures */
    test_happy_path();
    test_missing_path();
    test_invalid_fid();
    test_absolute_root_walk();
    test_write_small_response_does_not_call_backend();
    test_clunk_small_response_keeps_fid();

    if (g_failures != 0) {
        printf("mini9p_server_test: %d failure(s)\n", g_failures);
        return 1;
    }

    puts("mini9p_server_test: ok");
    return 0;
}
