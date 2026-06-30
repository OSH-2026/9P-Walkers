#include "cluster_vfs.h"

#include <stdio.h>
#include <string.h>

#include "host_coordinator.h"
#include "mini9p_protocol.h"

static int g_failures;

static const uint8_t k_long_diagnostics[] =
    "id=0 name=UART5 state=PEER rx=123 tx=456 hello=10/10 ack=10/10\\n"
    "id=0 dma=1 rx_bytes=1234 rx_frames=40 tx_bytes=5678 tx_frames=50\\n"
    "id=1 name=USART6 state=PEER rx=789 tx=987 hello=20/20 ack=20/20\\n"
    "id=1 dma=1 rx_bytes=4321 rx_frames=60 tx_bytes=8765 tx_frames=70\\n";

#define CHECK(expr) do { \
    if (!(expr)) { \
        ++g_failures; \
        printf("CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

typedef struct {
    pwos_session_manager_t *manager;
    uint8_t last_dst;
    uint8_t deadline_addr;
    uint8_t tx_buf[M9P_CLIENT_BUFFER_CAP];
    size_t tx_len;
    uint32_t send_count;
    uint32_t wait_count;
    uint32_t attach_count;
    uint32_t walk_count;
    uint32_t open_count;
    uint32_t read_count;
    uint32_t clunk_count;
    char last_walk_path[M9P_MAX_PATH_LEN + 1u];
} fake_link_t;

static int fake_build_response(
    fake_link_t *link,
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_cap,
    size_t *response_len);

typedef struct {
    pwos_host_coordinator_t coordinator;
    pwos_session_manager_t sessions;
    pwos_cluster_vfs_t vfs;
    fake_link_t link;
} test_env_t;

static pwos_mesh2_node_register_t make_register(
    uint32_t uid0,
    uint32_t boot_id,
    uint8_t upstream_port)
{
    pwos_mesh2_node_register_t reg;

    memset(&reg, 0, sizeof(reg));
    reg.uid[0] = uid0;
    reg.uid[1] = uid0 + 1u;
    reg.uid[2] = uid0 + 2u;
    reg.boot_id = boot_id;
    reg.caps = PWOS_MESH2_NODE_CAP_RELAY;
    reg.upstream_port = upstream_port;
    return reg;
}

static int fake_send(
    void *ctx,
    uint8_t data_type,
    uint8_t dst_addr,
    const uint8_t *payload,
    uint16_t payload_len)
{
    fake_link_t *link = (fake_link_t *)ctx;
    uint8_t response[M9P_CLIENT_BUFFER_CAP];
    size_t response_len = 0u;
    int rc;

    if (link == NULL || data_type != 0x80u || payload == NULL ||
        payload_len > sizeof(link->tx_buf)) {
        return -(int)M9P_ERR_EINVAL;
    }

    link->last_dst = dst_addr;
    memcpy(link->tx_buf, payload, payload_len);
    link->tx_len = payload_len;
    ++link->send_count;
    if (dst_addr == link->deadline_addr) {
        return 0;
    }

    rc = fake_build_response(
        link,
        payload,
        payload_len,
        response,
        sizeof(response),
        &response_len);
    if (rc != 0) {
        return rc;
    }
    return pwos_session_manager_deliver_mini9p(
        link->manager,
        dst_addr,
        response,
        response_len) == 1 ? 0 : -(int)M9P_ERR_EIO;
}

static int build_rattach(uint16_t tag, uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
    struct m9p_qid root_qid;

    memset(&root_qid, 0, sizeof(root_qid));
    root_qid.type = M9P_QID_DIR;
    root_qid.version = 1u;
    root_qid.object_id = 1u;
    return m9p_build_rattach(
        tag,
        M9P_DEFAULT_MSIZE,
        16u,
        M9P_DEFAULT_INFLIGHT,
        M9P_FEATURE_DIRECTORY_READ,
        &root_qid,
        rx,
        rx_cap,
        rx_len) ? 0 : -(int)M9P_ERR_EMSIZE;
}

static struct m9p_qid qid_for_path(const char *path)
{
    struct m9p_qid qid;

    memset(&qid, 0, sizeof(qid));
    qid.version = 1u;
    qid.object_id = 42u;
    if (strcmp(path, "/") == 0 || strcmp(path, "/sys") == 0) {
        qid.type = M9P_QID_DIR | M9P_QID_VIRTUAL;
    } else {
        qid.type = M9P_QID_VIRTUAL | M9P_QID_READONLY;
    }
    return qid;
}

static int fake_build_response(
    fake_link_t *link,
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_cap,
    size_t *response_len)
{
    struct m9p_frame_view frame;

    if (link == NULL || request == NULL || response == NULL || response_len == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    if (!m9p_decode_frame(request, request_len, &frame)) {
        return -(int)M9P_ERR_EIO;
    }

    ++link->wait_count;
    switch (frame.type) {
    case M9P_TATTACH:
        ++link->attach_count;
        return build_rattach(frame.tag, response, response_cap, response_len);

    case M9P_TWALK: {
        struct m9p_walk_request request;
        struct m9p_qid qid;

        if (!m9p_parse_twalk(&frame, &request)) {
            return m9p_build_rerror(frame.tag, M9P_ERR_EINVAL, "bad walk", response, response_cap, response_len) ?
                0 : -(int)M9P_ERR_EMSIZE;
        }
        ++link->walk_count;
        snprintf(link->last_walk_path, sizeof(link->last_walk_path), "%s", request.path);
        qid = qid_for_path(request.path);
        return m9p_build_rwalk(frame.tag, &qid, response, response_cap, response_len) ?
            0 : -(int)M9P_ERR_EMSIZE;
    }

    case M9P_TOPEN: {
        struct m9p_open_request request;
        struct m9p_qid qid;

        if (!m9p_parse_topen(&frame, &request)) {
            return m9p_build_rerror(frame.tag, M9P_ERR_EINVAL, "bad open", response, response_cap, response_len) ?
                0 : -(int)M9P_ERR_EMSIZE;
        }
        ++link->open_count;
        qid = qid_for_path(link->last_walk_path);
        return m9p_build_ropen(frame.tag, &qid, 128u, response, response_cap, response_len) ?
            0 : -(int)M9P_ERR_EMSIZE;
    }

    case M9P_TREAD: {
        static const uint8_t health[] = {'o', 'k'};
        struct m9p_read_request request;

        if (!m9p_parse_tread(&frame, &request)) {
            return m9p_build_rerror(frame.tag, M9P_ERR_EINVAL, "bad read", response, response_cap, response_len) ?
                0 : -(int)M9P_ERR_EMSIZE;
        }
        ++link->read_count;
        if (strcmp(link->last_walk_path, "/sys/health") == 0 && request.offset == 0u) {
            return m9p_build_rread(
                frame.tag,
                health,
                (uint16_t)sizeof(health),
                response,
                response_cap,
                response_len) ? 0 : -(int)M9P_ERR_EMSIZE;
        }
        if (strcmp(link->last_walk_path, "/sys/ports") == 0) {
            const uint32_t total = (uint32_t)(sizeof(k_long_diagnostics) - 1u);
            uint16_t count = 0u;

            if (request.offset < total) {
                uint32_t remaining = total - request.offset;

                count = request.count < remaining ?
                    request.count : (uint16_t)remaining;
            }
            return m9p_build_rread(
                frame.tag,
                count > 0u ? k_long_diagnostics + request.offset : NULL,
                count,
                response,
                response_cap,
                response_len) ? 0 : -(int)M9P_ERR_EMSIZE;
        }
        return m9p_build_rread(frame.tag, NULL, 0u, response, response_cap, response_len) ?
            0 : -(int)M9P_ERR_EMSIZE;
    }

    case M9P_TCLUNK:
        ++link->clunk_count;
        return m9p_build_rclunk(frame.tag, response, response_cap, response_len) ?
            0 : -(int)M9P_ERR_EMSIZE;

    default:
        return m9p_build_rerror(frame.tag, M9P_ERR_ENOTSUP, "unsupported", response, response_cap, response_len) ?
            0 : -(int)M9P_ERR_EMSIZE;
    }
}

static void init_env(test_env_t *env)
{
    pwos_session_manager_config_t session_config;
    pwos_mesh2_node_register_t reg1;
    pwos_mesh2_node_register_t reg2;
    pwos_mesh2_addr_assign_t assign;

    memset(env, 0, sizeof(*env));
    pwos_host_coordinator_init(&env->coordinator);

    reg1 = make_register(100u, 10u, 0u);
    reg2 = make_register(200u, 20u, 1u);
    CHECK(pwos_host_coordinator_handle_register(&env->coordinator, &reg1, &assign) == 0);
    CHECK(assign.addr == 1u);
    CHECK(pwos_host_coordinator_handle_register(&env->coordinator, &reg2, &assign) == 0);
    CHECK(assign.addr == 2u);

    memset(&session_config, 0, sizeof(session_config));
    session_config.io_ctx = &env->link;
    session_config.send = fake_send;
    session_config.default_deadline_ms = 1000u;
    CHECK(pwos_session_manager_init(&env->sessions, &session_config) == 0);
    env->link.manager = &env->sessions;
    CHECK(pwos_cluster_vfs_init(&env->vfs, &env->sessions) == 0);
    CHECK(pwos_cluster_vfs_sync_from_coordinator(&env->vfs, &env->coordinator) == 0);
}

static void test_sync_names_and_root_list(void)
{
    test_env_t env;
    struct m9p_dirent entries[4];
    size_t count = 0u;
    pwos_cluster_vfs_route_t route;

    init_env(&env);

    CHECK(pwos_cluster_vfs_get_route(&env.vfs, 0u, &route) == 0);
    CHECK(strcmp(route.target, "mcu1") == 0);
    CHECK(route.addr == 1u);
    CHECK(pwos_cluster_vfs_get_route(&env.vfs, 1u, &route) == 0);
    CHECK(strcmp(route.target, "mcu2") == 0);
    CHECK(route.addr == 2u);

    memset(entries, 0, sizeof(entries));
    CHECK(pwos_cluster_vfs_list(&env.vfs, "/", entries, 4u, &count, 1000u) == 0);
    CHECK(count == 2u);
    CHECK(strcmp(entries[0].name, "mcu1") == 0);
    CHECK(strcmp(entries[1].name, "mcu2") == 0);
}

static void test_read_path_uses_session_manager(void)
{
    test_env_t env;
    uint8_t buf[8];
    uint16_t len = sizeof(buf);

    init_env(&env);
    memset(buf, 0, sizeof(buf));

    CHECK(pwos_cluster_vfs_read_path(&env.vfs, "/mcu1/sys/health", buf, &len, 1000u) == 0);
    CHECK(len == 2u);
    CHECK(memcmp(buf, "ok", 2u) == 0);
    CHECK(env.link.last_dst == 1u);
    CHECK(strcmp(env.link.last_walk_path, "/sys/health") == 0);
    CHECK(env.link.attach_count == 1u);
    CHECK(env.link.open_count == 1u);
    CHECK(env.link.read_count == 2u); /* 数据块 + EOF */
    CHECK(env.link.clunk_count == 1u);
}

static void test_read_path_continues_until_eof(void)
{
    test_env_t env;
    uint8_t buf[512];
    uint16_t len = sizeof(buf);

    init_env(&env);
    memset(buf, 0, sizeof(buf));

    CHECK(pwos_cluster_vfs_read_path(
        &env.vfs, "/mcu1/sys/ports", buf, &len, 1000u) == 0);
    CHECK(len == sizeof(k_long_diagnostics) - 1u);
    CHECK(memcmp(buf, k_long_diagnostics, len) == 0);
    CHECK(env.link.read_count > 1u);
    CHECK(env.link.clunk_count == 1u);
}

static void test_boot_change_forces_reattach(void)
{
    test_env_t env;
    pwos_mesh2_node_register_t reg;
    pwos_mesh2_addr_assign_t assign;
    uint8_t buf[8];
    uint16_t len = sizeof(buf);

    init_env(&env);
    CHECK(pwos_cluster_vfs_read_path(&env.vfs, "/mcu1/sys/health", buf, &len, 1000u) == 0);
    CHECK(env.link.attach_count == 1u);

    reg = make_register(100u, 11u, 0u);
    CHECK(pwos_host_coordinator_handle_register(&env.coordinator, &reg, &assign) == 0);
    CHECK(assign.addr == 1u);
    CHECK(pwos_cluster_vfs_sync_from_coordinator(&env.vfs, &env.coordinator) == 0);

    len = sizeof(buf);
    CHECK(pwos_cluster_vfs_read_path(&env.vfs, "/mcu1/sys/health", buf, &len, 1000u) == 0);
    CHECK(env.link.attach_count == 2u);
}

static void test_offline_returns_no_route(void)
{
    test_env_t env;
    uint8_t buf[8];
    uint16_t len = sizeof(buf);

    init_env(&env);
    env.coordinator.nodes[0].valid = 0u;
    CHECK(pwos_cluster_vfs_sync_from_coordinator(&env.vfs, &env.coordinator) == 0);
    CHECK(pwos_cluster_vfs_read_path(&env.vfs, "/mcu1/sys/health", buf, &len, 1000u) ==
          PWOS_SESSION_ERR_NO_ROUTE);
}

static void test_deadline_is_not_eagain_retry(void)
{
    test_env_t env;
    uint8_t buf[8];
    uint16_t len = sizeof(buf);
    uint32_t before_send;

    init_env(&env);
    env.link.deadline_addr = 2u;
    before_send = env.link.send_count;
    CHECK(pwos_cluster_vfs_read_path(&env.vfs, "/mcu2/sys/health", buf, &len, 1000u) ==
          PWOS_SESSION_ERR_DEADLINE);
    CHECK(env.link.send_count == before_send + 1u);
}

int main(void)
{
    test_sync_names_and_root_list();
    test_read_path_uses_session_manager();
    test_read_path_continues_until_eof();
    test_boot_change_forces_reattach();
    test_offline_returns_no_route();
    test_deadline_is_not_eagain_retry();

    if (g_failures != 0) {
        printf("pwos host api tests failed: %d\n", g_failures);
        return 1;
    }
    printf("pwos host api tests passed\n");
    return 0;
}
