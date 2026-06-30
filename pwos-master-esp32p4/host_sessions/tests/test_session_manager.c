#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mini9p_protocol.h"
#include "pwos_rpc_protocol.h"
#include "rpc_client.h"
#include "session_manager.h"

static int g_failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        ++g_failures; \
        printf("CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

typedef struct {
    pthread_mutex_t manager_mutex;
    pthread_mutex_t client_mutex[PWOS_SESSION_MANAGER_MAX_SESSIONS];
    pthread_mutex_t event_mutex;
    pthread_cond_t event_cond[PWOS_SESSION_MANAGER_MAX_PENDING];
    uint8_t event_signaled[PWOS_SESSION_MANAGER_MAX_PENDING];
} test_sync_t;

typedef enum {
    FAKE_SEND_IMMEDIATE = 0,
    FAKE_SEND_BARRIER_TWO,
    FAKE_SEND_HOLD,
    FAKE_SEND_DROP,
    FAKE_SEND_BAD_STREAM,
} fake_send_mode_t;

typedef struct {
    pwos_session_manager_t *manager;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    fake_send_mode_t mode;
    uint32_t send_count;
    uint16_t wire_tags[16];
    uint8_t held_dst;
    uint8_t held_request[M9P_CLIENT_BUFFER_CAP];
    size_t held_request_len;
} fake_link_t;

typedef struct {
    pwos_session_manager_t *manager;
    uint8_t addr;
    uint32_t boot_id;
    uint32_t deadline_ms;
    int rc;
} attach_thread_arg_t;

typedef struct {
    pwos_session_manager_t manager;
    test_sync_t sync;
    fake_link_t link;
} test_env_t;

static void add_ms_to_timespec(struct timespec *ts, uint32_t timeout_ms)
{
    ts->tv_sec += (time_t)(timeout_ms / 1000u);
    ts->tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ++ts->tv_sec;
        ts->tv_nsec -= 1000000000L;
    }
}

static uint32_t monotonic_ms(void *ctx)
{
    struct timespec ts;

    (void)ctx;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static void manager_lock(void *ctx)
{
    test_sync_t *sync = (test_sync_t *)ctx;

    (void)pthread_mutex_lock(&sync->manager_mutex);
}

static void manager_unlock(void *ctx)
{
    test_sync_t *sync = (test_sync_t *)ctx;

    (void)pthread_mutex_unlock(&sync->manager_mutex);
}

static void client_lock(void *ctx, uint8_t session_index)
{
    test_sync_t *sync = (test_sync_t *)ctx;

    (void)pthread_mutex_lock(&sync->client_mutex[session_index]);
}

static void client_unlock(void *ctx, uint8_t session_index)
{
    test_sync_t *sync = (test_sync_t *)ctx;

    (void)pthread_mutex_unlock(&sync->client_mutex[session_index]);
}

static void pending_reset(void *ctx, uint8_t pending_index)
{
    test_sync_t *sync = (test_sync_t *)ctx;

    (void)pthread_mutex_lock(&sync->event_mutex);
    sync->event_signaled[pending_index] = 0u;
    (void)pthread_mutex_unlock(&sync->event_mutex);
}

static int pending_wait(void *ctx, uint8_t pending_index, uint32_t timeout_ms)
{
    test_sync_t *sync = (test_sync_t *)ctx;
    struct timespec deadline;
    int wait_rc = 0;

    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    add_ms_to_timespec(&deadline, timeout_ms);

    (void)pthread_mutex_lock(&sync->event_mutex);
    while (sync->event_signaled[pending_index] == 0u && wait_rc == 0) {
        wait_rc = pthread_cond_timedwait(
            &sync->event_cond[pending_index],
            &sync->event_mutex,
            &deadline);
    }
    if (sync->event_signaled[pending_index] != 0u) {
        sync->event_signaled[pending_index] = 0u;
        wait_rc = 0;
    }
    (void)pthread_mutex_unlock(&sync->event_mutex);
    return wait_rc == 0 ? 0 : PWOS_SESSION_ERR_DEADLINE;
}

static void pending_signal(void *ctx, uint8_t pending_index)
{
    test_sync_t *sync = (test_sync_t *)ctx;

    (void)pthread_mutex_lock(&sync->event_mutex);
    sync->event_signaled[pending_index] = 1u;
    (void)pthread_cond_signal(&sync->event_cond[pending_index]);
    (void)pthread_mutex_unlock(&sync->event_mutex);
}

static int build_attach_response(
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_cap,
    size_t *response_len)
{
    struct m9p_frame_view frame;
    struct m9p_qid root_qid;

    if (!m9p_decode_frame(request, request_len, &frame) || frame.type != M9P_TATTACH) {
        return -(int)M9P_ERR_EINVAL;
    }
    memset(&root_qid, 0, sizeof(root_qid));
    root_qid.type = M9P_QID_DIR;
    root_qid.object_id = 1u;
    return m9p_build_rattach(
        frame.tag,
        M9P_DEFAULT_MSIZE,
        16u,
        M9P_DEFAULT_INFLIGHT,
        M9P_FEATURE_DIRECTORY_READ,
        &root_qid,
        response,
        response_cap,
        response_len) ? 0 : -(int)M9P_ERR_EMSIZE;
}

static int fake_send(
    void *ctx,
    uint8_t data_type,
    uint8_t dst_addr,
    const uint8_t *payload,
    uint16_t payload_len)
{
    fake_link_t *link = (fake_link_t *)ctx;
    struct m9p_frame_view frame;
    uint8_t response[M9P_CLIENT_BUFFER_CAP];
    size_t response_len = 0u;
    fake_send_mode_t mode;
    uint32_t send_number;
    int rc;

    if (data_type == 0x81u) {
        pwos_rpc_frame_view_t request;
        size_t rpc_response_len = 0u;

        if (pwos_rpc_decode(payload, payload_len, &request) != 0) {
            return -(int)M9P_ERR_EINVAL;
        }
        (void)pthread_mutex_lock(&link->mutex);
        send_number = ++link->send_count;
        if (send_number <= sizeof(link->wire_tags) / sizeof(link->wire_tags[0])) {
            link->wire_tags[send_number - 1u] = request.call_id;
        }
        mode = link->mode;
        (void)pthread_cond_broadcast(&link->cond);
        (void)pthread_mutex_unlock(&link->mutex);
        if (request.kind == PWOS_RPC_KIND_CANCEL ||
            (request.flags & PWOS_RPC_FLAG_ONEWAY) != 0u ||
            mode == FAKE_SEND_DROP) {
            return 0;
        }
        if ((request.flags & PWOS_RPC_FLAG_STREAM) != 0u) {
            if (mode == FAKE_SEND_BAD_STREAM) {
                (void)pwos_session_manager_deliver_data_part(
                    link->manager,
                    data_type,
                    dst_addr,
                    request.call_id,
                    (const uint8_t *)"po",
                    2u,
                    0u,
                    0u);
                (void)pwos_session_manager_deliver_data_part(
                    link->manager,
                    data_type,
                    dst_addr,
                    request.call_id,
                    (const uint8_t *)"ng",
                    2u,
                    0u,
                    2u);
                return 0;
            }
            if (pwos_session_manager_deliver_data_part(
                    link->manager,
                    data_type,
                    dst_addr,
                    request.call_id,
                    (const uint8_t *)"po",
                    2u,
                    0u,
                    0u) != 1 ||
                pwos_session_manager_deliver_data_part(
                    link->manager,
                    data_type,
                    dst_addr,
                    request.call_id,
                    (const uint8_t *)"ng",
                    2u,
                    0u,
                    1u) != 1 ||
                pwos_session_manager_deliver_data_part(
                    link->manager,
                    data_type,
                    dst_addr,
                    request.call_id,
                    NULL,
                    0u,
                    1u,
                    PWOS_RPC_STATUS_OK) != 1) {
                return -(int)M9P_ERR_EIO;
            }
            return 0;
        }
        if (
            pwos_rpc_encode(
                PWOS_RPC_KIND_RESPONSE,
                0u,
                request.call_id,
                PWOS_RPC_STATUS_OK,
                0u,
                NULL,
                NULL,
                (const uint8_t *)"pong",
                4u,
                response,
                sizeof(response),
                &rpc_response_len) != 0) {
            return -(int)M9P_ERR_EINVAL;
        }
        return pwos_session_manager_deliver_data(
            link->manager,
            data_type,
            dst_addr,
            request.call_id,
            response,
            rpc_response_len) == 1 ? 0 : -(int)M9P_ERR_EIO;
    }
    if (data_type != 0x80u || !m9p_decode_frame(payload, payload_len, &frame)) {
        return -(int)M9P_ERR_EINVAL;
    }

    (void)pthread_mutex_lock(&link->mutex);
    send_number = ++link->send_count;
    if (send_number <= sizeof(link->wire_tags) / sizeof(link->wire_tags[0])) {
        link->wire_tags[send_number - 1u] = frame.tag;
    }
    mode = link->mode;

    if (mode == FAKE_SEND_HOLD) {
        link->held_dst = dst_addr;
        memcpy(link->held_request, payload, payload_len);
        link->held_request_len = payload_len;
        (void)pthread_cond_broadcast(&link->cond);
        (void)pthread_mutex_unlock(&link->mutex);
        return 0;
    }
    if (mode == FAKE_SEND_DROP) {
        (void)pthread_cond_broadcast(&link->cond);
        (void)pthread_mutex_unlock(&link->mutex);
        return 0;
    }
    if (mode == FAKE_SEND_BARRIER_TWO) {
        if (send_number < 2u) {
            while (link->send_count < 2u) {
                (void)pthread_cond_wait(&link->cond, &link->mutex);
            }
        } else {
            (void)pthread_cond_broadcast(&link->cond);
        }
    }
    (void)pthread_mutex_unlock(&link->mutex);

    rc = build_attach_response(
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

static int rpc_retag(uint8_t *frame, size_t frame_len, uint16_t wire_tag)
{
    return pwos_rpc_retag(frame, frame_len, wire_tag);
}

static void init_env(test_env_t *env)
{
    pwos_session_manager_config_t config;
    size_t i;

    memset(env, 0, sizeof(*env));
    CHECK(pthread_mutex_init(&env->sync.manager_mutex, NULL) == 0);
    CHECK(pthread_mutex_init(&env->sync.event_mutex, NULL) == 0);
    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_SESSIONS; ++i) {
        CHECK(pthread_mutex_init(&env->sync.client_mutex[i], NULL) == 0);
    }
    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_PENDING; ++i) {
        CHECK(pthread_cond_init(&env->sync.event_cond[i], NULL) == 0);
    }
    CHECK(pthread_mutex_init(&env->link.mutex, NULL) == 0);
    CHECK(pthread_cond_init(&env->link.cond, NULL) == 0);

    memset(&config, 0, sizeof(config));
    config.io_ctx = &env->link;
    config.send = fake_send;
    config.default_deadline_ms = 200u;
    config.sync_ctx = &env->sync;
    config.lock = manager_lock;
    config.unlock = manager_unlock;
    config.client_lock = client_lock;
    config.client_unlock = client_unlock;
    config.pending_reset = pending_reset;
    config.pending_wait = pending_wait;
    config.pending_signal = pending_signal;
    config.now_ms = monotonic_ms;
    CHECK(pwos_session_manager_init(&env->manager, &config) == 0);
    env->link.manager = &env->manager;
}

static void destroy_env(test_env_t *env)
{
    size_t i;

    (void)pthread_cond_destroy(&env->link.cond);
    (void)pthread_mutex_destroy(&env->link.mutex);
    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_PENDING; ++i) {
        (void)pthread_cond_destroy(&env->sync.event_cond[i]);
    }
    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_SESSIONS; ++i) {
        (void)pthread_mutex_destroy(&env->sync.client_mutex[i]);
    }
    (void)pthread_mutex_destroy(&env->sync.event_mutex);
    (void)pthread_mutex_destroy(&env->sync.manager_mutex);
}

static void *attach_thread(void *arg)
{
    attach_thread_arg_t *thread_arg = (attach_thread_arg_t *)arg;

    thread_arg->rc = pwos_session_manager_attach(
        thread_arg->manager,
        thread_arg->addr,
        thread_arg->boot_id,
        thread_arg->deadline_ms);
    return NULL;
}

static void wait_for_send_count(fake_link_t *link, uint32_t expected)
{
    struct timespec deadline;

    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    add_ms_to_timespec(&deadline, 1000u);
    (void)pthread_mutex_lock(&link->mutex);
    while (link->send_count < expected) {
        if (pthread_cond_timedwait(&link->cond, &link->mutex, &deadline) == ETIMEDOUT) {
            break;
        }
    }
    (void)pthread_mutex_unlock(&link->mutex);
}

static int deliver_held_response(fake_link_t *link, uint8_t src_addr, int tag_delta)
{
    uint8_t request[M9P_CLIENT_BUFFER_CAP];
    uint8_t response[M9P_CLIENT_BUFFER_CAP];
    size_t request_len;
    size_t response_len = 0u;
    struct m9p_frame_view frame;

    (void)pthread_mutex_lock(&link->mutex);
    request_len = link->held_request_len;
    memcpy(request, link->held_request, request_len);
    (void)pthread_mutex_unlock(&link->mutex);

    CHECK(build_attach_response(
        request,
        request_len,
        response,
        sizeof(response),
        &response_len) == 0);
    CHECK(m9p_decode_frame(response, response_len, &frame));
    if (tag_delta != 0) {
        CHECK(m9p_retag_frame(response, response_len, (uint16_t)(frame.tag + tag_delta)));
    }
    return pwos_session_manager_deliver_mini9p(
        link->manager,
        src_addr,
        response,
        response_len);
}

static void test_two_nodes_are_concurrent_and_tags_are_global(void)
{
    test_env_t env;
    attach_thread_arg_t arg1;
    attach_thread_arg_t arg2;
    pthread_t thread1;
    pthread_t thread2;
    pwos_session_manager_stats_t stats;

    init_env(&env);
    CHECK(pwos_session_manager_update_node(&env.manager, 1u, 10u) == 0);
    CHECK(pwos_session_manager_update_node(&env.manager, 2u, 20u) == 0);
    env.link.mode = FAKE_SEND_BARRIER_TWO;

    arg1 = (attach_thread_arg_t){&env.manager, 1u, 10u, 500u, -1};
    arg2 = (attach_thread_arg_t){&env.manager, 2u, 20u, 500u, -1};
    CHECK(pthread_create(&thread1, NULL, attach_thread, &arg1) == 0);
    CHECK(pthread_create(&thread2, NULL, attach_thread, &arg2) == 0);
    CHECK(pthread_join(thread1, NULL) == 0);
    CHECK(pthread_join(thread2, NULL) == 0);

    CHECK(arg1.rc == 0);
    CHECK(arg2.rc == 0);
    CHECK(env.link.send_count == 2u);
    CHECK(env.link.wire_tags[0] != 0u);
    CHECK(env.link.wire_tags[0] != env.link.wire_tags[1]);
    pwos_session_manager_get_stats(&env.manager, &stats);
    CHECK(stats.pending_peak == 2u);
    CHECK(stats.pending_delivered == 2u);
    destroy_env(&env);
}

static void test_response_requires_source_and_tag_match(void)
{
    test_env_t env;
    attach_thread_arg_t arg;
    pthread_t thread;
    pwos_session_manager_stats_t stats;

    init_env(&env);
    CHECK(pwos_session_manager_update_node(&env.manager, 3u, 30u) == 0);
    env.link.mode = FAKE_SEND_HOLD;
    arg = (attach_thread_arg_t){&env.manager, 3u, 30u, 500u, -1};
    CHECK(pthread_create(&thread, NULL, attach_thread, &arg) == 0);
    wait_for_send_count(&env.link, 1u);

    CHECK(deliver_held_response(&env.link, 2u, 0) == 0);
    CHECK(deliver_held_response(&env.link, 3u, 1) == 0);
    CHECK(deliver_held_response(&env.link, 3u, 0) == 1);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(arg.rc == 0);

    pwos_session_manager_get_stats(&env.manager, &stats);
    CHECK(stats.pending_unmatched == 2u);
    destroy_env(&env);
}

static void test_deadline_releases_pending_slot(void)
{
    test_env_t env;
    size_t i;

    init_env(&env);
    CHECK(pwos_session_manager_update_node(&env.manager, 4u, 40u) == 0);
    env.link.mode = FAKE_SEND_DROP;
    CHECK(pwos_session_manager_attach(&env.manager, 4u, 40u, 30u) ==
          PWOS_SESSION_ERR_DEADLINE);
    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_PENDING; ++i) {
        CHECK(env.manager.pending[i].used == 0u);
    }
    destroy_env(&env);
}

static void test_boot_change_cancels_old_request(void)
{
    test_env_t env;
    attach_thread_arg_t arg;
    pthread_t thread;
    pwos_session_manager_stats_t stats;

    init_env(&env);
    CHECK(pwos_session_manager_update_node(&env.manager, 5u, 50u) == 0);
    env.link.mode = FAKE_SEND_HOLD;
    arg = (attach_thread_arg_t){&env.manager, 5u, 50u, 500u, -1};
    CHECK(pthread_create(&thread, NULL, attach_thread, &arg) == 0);
    wait_for_send_count(&env.link, 1u);

    CHECK(pwos_session_manager_update_node(&env.manager, 5u, 51u) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(arg.rc == PWOS_SESSION_ERR_STALE_BOOT);
    pwos_session_manager_get_stats(&env.manager, &stats);
    CHECK(stats.pending_cancelled == 1u);
    CHECK(stats.resets == 1u);
    destroy_env(&env);
}

static void test_generic_rpc_request_uses_typed_pending(void)
{
    test_env_t env;
    uint8_t request[PWOS_RPC_MAX_FRAME_LEN];
    uint8_t response[PWOS_RPC_MAX_FRAME_LEN];
    pwos_rpc_frame_view_t response_view;
    size_t request_len = 0u;
    size_t response_len = 0u;
    uint16_t wire_tag = 0u;

    init_env(&env);
    CHECK(pwos_session_manager_update_node(&env.manager, 6u, 60u) == 0);
    CHECK(pwos_rpc_encode(
        PWOS_RPC_KIND_REQUEST,
        0u,
        0u,
        0u,
        100u,
        "system",
        "ping",
        NULL,
        0u,
        request,
        sizeof(request),
        &request_len) == 0);
    CHECK(pwos_session_manager_request_data(
        &env.manager,
        6u,
        60u,
        0x81u,
        request,
        request_len,
        rpc_retag,
        100u,
        response,
        sizeof(response),
        &response_len,
        &wire_tag) == 0);
    CHECK(wire_tag != 0u);
    CHECK(pwos_rpc_decode(response, response_len, &response_view) == 0);
    CHECK(response_view.call_id == wire_tag);
    CHECK(response_view.status == PWOS_RPC_STATUS_OK);
    CHECK(response_view.payload_len == 4u);
    CHECK(memcmp(response_view.payload, "pong", 4u) == 0);
    destroy_env(&env);
}

static void test_rpc_client_call_notify_and_timeout_cancel(void)
{
    test_env_t env;
    pwos_rpc_client_t client;
    pwos_rpc_client_stats_t stats;
    uint8_t response[32];
    uint16_t response_len = sizeof(response);
    uint16_t status = 0u;
    uint16_t chunk_count = 0u;

    init_env(&env);
    CHECK(pwos_session_manager_update_node(&env.manager, 7u, 70u) == 0);
    CHECK(pwos_rpc_client_init(&client, &env.manager) == 0);
    CHECK(pwos_rpc_client_call(
        &client,
        7u,
        70u,
        "system",
        "ping",
        (const uint8_t *)"hello",
        5u,
        50u,
        response,
        &response_len,
        &status) == 0);
    CHECK(status == PWOS_RPC_STATUS_OK);
    CHECK(response_len == 4u);
    CHECK(memcmp(response, "pong", 4u) == 0);
    CHECK(pwos_rpc_client_notify(
        &client,
        7u,
        70u,
        "system",
        "notify",
        NULL,
        0u,
        50u) == 0);
    response_len = sizeof(response);
    CHECK(pwos_rpc_client_stream(
        &client,
        7u,
        70u,
        "system",
        "stream",
        (const uint8_t *)"hello",
        5u,
        50u,
        response,
        &response_len,
        &status,
        &chunk_count) == 0);
    CHECK(status == PWOS_RPC_STATUS_OK);
    CHECK(chunk_count == 2u);
    CHECK(response_len == 4u);
    CHECK(memcmp(response, "pong", 4u) == 0);

    env.link.mode = FAKE_SEND_BAD_STREAM;
    response_len = sizeof(response);
    CHECK(pwos_rpc_client_stream(
        &client,
        7u,
        70u,
        "system",
        "stream",
        (const uint8_t *)"bad-order",
        9u,
        50u,
        response,
        &response_len,
        &status,
        &chunk_count) == -(int)M9P_ERR_EIO);

    env.link.mode = FAKE_SEND_DROP;
    response_len = sizeof(response);
    CHECK(pwos_rpc_client_call(
        &client,
        7u,
        70u,
        "system",
        "delay",
        (const uint8_t *)"500",
        3u,
        1u,
        response,
        &response_len,
        &status) == PWOS_SESSION_ERR_DEADLINE);
    response_len = sizeof(response);
    CHECK(pwos_rpc_client_stream(
        &client,
        7u,
        70u,
        "system",
        "stream",
        (const uint8_t *)"timeout",
        7u,
        1u,
        response,
        &response_len,
        &status,
        &chunk_count) == PWOS_SESSION_ERR_DEADLINE);
    pwos_rpc_client_get_stats(&client, &stats);
    CHECK(stats.unary_tx == 2u);
    CHECK(stats.unary_rx == 1u);
    CHECK(stats.oneway_tx == 1u);
    CHECK(stats.stream_tx == 3u);
    CHECK(stats.stream_rx == 1u);
    CHECK(stats.stream_chunks_rx == 2u);
    CHECK(stats.deadline_errors == 2u);
    CHECK(stats.cancel_tx == 2u);
    CHECK(stats.malformed_responses == 0u);
    destroy_env(&env);
}

int main(void)
{
    test_two_nodes_are_concurrent_and_tags_are_global();
    test_response_requires_source_and_tag_match();
    test_deadline_releases_pending_slot();
    test_boot_change_cancels_old_request();
    test_generic_rpc_request_uses_typed_pending();
    test_rpc_client_call_notify_and_timeout_cancel();

    if (g_failures != 0) {
        printf("pwos host session tests failed: %d\n", g_failures);
        return 1;
    }
    printf("pwos host session tests passed\n");
    return 0;
}
