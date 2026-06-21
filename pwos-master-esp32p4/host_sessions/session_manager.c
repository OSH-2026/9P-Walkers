#include "session_manager.h"

#include <string.h>

#include "mini9p_protocol.h"

static uint32_t effective_deadline(
    const pwos_session_manager_t *manager,
    uint32_t requested_deadline_ms)
{
    if (requested_deadline_ms != 0u) {
        return requested_deadline_ms;
    }
    if (manager != NULL && manager->config.default_deadline_ms != 0u) {
        return manager->config.default_deadline_ms;
    }
    return PWOS_SESSION_DEFAULT_DEADLINE_MS;
}

static pwos_session_entry_t *find_session(
    pwos_session_manager_t *manager,
    uint8_t addr)
{
    size_t i;

    if (manager == NULL) {
        return NULL;
    }

    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_SESSIONS; ++i) {
        if (manager->sessions[i].used != 0u && manager->sessions[i].addr == addr) {
            return &manager->sessions[i];
        }
    }
    return NULL;
}

static pwos_session_entry_t *alloc_session(pwos_session_manager_t *manager)
{
    size_t i;

    if (manager == NULL) {
        return NULL;
    }

    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_SESSIONS; ++i) {
        if (manager->sessions[i].used == 0u) {
            return &manager->sessions[i];
        }
    }
    return NULL;
}

static void init_client_for_session(pwos_session_entry_t *session);

static void reset_session_state(pwos_session_entry_t *session)
{
    if (session == NULL) {
        return;
    }

    /*
     * node boot_id 变化或路由重新绑定时，远端 fid 表已经不可再信任。
     * 这里只重置 mini9P 会话语义，保留统计和 addr/boot_id 绑定。
     */
    m9p_client_reset_session(&session->client);
    init_client_for_session(session);
    session->attached = 0u;
}

static int session_transport(
    void *transport_ctx,
    const uint8_t *tx_data,
    size_t tx_len,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    pwos_session_entry_t *session = (pwos_session_entry_t *)transport_ctx;
    pwos_session_manager_t *manager;
    struct m9p_frame_view view;
    int rc;

    if (session == NULL || session->manager == NULL || tx_data == NULL ||
        tx_len == 0u || tx_len > 0xFFFFu || rx_len == NULL ||
        (rx_cap > 0u && rx_data == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }

    manager = session->manager;
    if (manager->config.send == NULL || manager->config.wait == NULL) {
        return -(int)M9P_ERR_ENOTSUP;
    }

    if (m9p_decode_frame(tx_data, tx_len, &view)) {
        session->last_tx_tag = view.tag;
        session->last_tx_type = view.type;
    }

    rc = manager->config.send(
        manager->config.io_ctx,
        session->addr,
        tx_data,
        (uint16_t)tx_len);
    if (rc != 0) {
        if (rc == PWOS_SESSION_ERR_NO_ROUTE) {
            ++session->no_route_errors;
            ++manager->stats.no_route_errors;
        } else if (rc == PWOS_SESSION_ERR_QUEUE_FULL) {
            ++session->queue_full_errors;
            ++manager->stats.queue_full_errors;
        } else {
            ++session->io_errors;
            ++manager->stats.io_errors;
        }
        return rc;
    }

    ++session->tx_requests;
    ++manager->stats.tx_requests;
    *rx_len = 0u;

    rc = manager->config.wait(
        manager->config.io_ctx,
        session->addr,
        session->deadline_ms,
        rx_data,
        rx_cap,
        rx_len);
    if (rc != 0) {
        if (rc == PWOS_SESSION_ERR_DEADLINE) {
            ++session->deadline_errors;
            ++manager->stats.deadline_errors;
        } else if (rc == PWOS_SESSION_ERR_NO_ROUTE) {
            ++session->no_route_errors;
            ++manager->stats.no_route_errors;
        } else if (rc == PWOS_SESSION_ERR_QUEUE_FULL) {
            ++session->queue_full_errors;
            ++manager->stats.queue_full_errors;
        } else {
            ++session->io_errors;
            ++manager->stats.io_errors;
        }
        return rc;
    }

    if (m9p_decode_frame(rx_data, *rx_len, &view)) {
        session->last_rx_tag = view.tag;
        session->last_rx_type = view.type;
    }
    ++session->rx_responses;
    ++manager->stats.rx_responses;
    return 0;
}

static void init_client_for_session(pwos_session_entry_t *session)
{
    if (session == NULL) {
        return;
    }

    m9p_client_init(&session->client, session_transport, session);
}

int pwos_session_manager_init(
    pwos_session_manager_t *manager,
    const pwos_session_manager_config_t *config)
{
    if (manager == NULL || config == NULL ||
        config->send == NULL || config->wait == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    memset(manager, 0, sizeof(*manager));
    manager->config = *config;
    manager->config.default_deadline_ms =
        effective_deadline(manager, config->default_deadline_ms);
    return 0;
}

int pwos_session_manager_update_node(
    pwos_session_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id)
{
    pwos_session_entry_t *session;

    if (manager == NULL || addr == 0u || addr == 0xFFu) {
        return PWOS_SESSION_ERR_NO_ROUTE;
    }

    session = find_session(manager, addr);
    if (session == NULL) {
        session = alloc_session(manager);
        if (session == NULL) {
            return PWOS_SESSION_ERR_QUEUE_FULL;
        }
        memset(session, 0, sizeof(*session));
        session->used = 1u;
        session->addr = addr;
        session->boot_id = boot_id;
        session->manager = manager;
        init_client_for_session(session);
        return 0;
    }

    if (session->boot_id != boot_id) {
        session->boot_id = boot_id;
        reset_session_state(session);
        ++manager->stats.resets;
    }
    session->manager = manager;
    return 0;
}

void pwos_session_manager_reset_node(pwos_session_manager_t *manager, uint8_t addr)
{
    pwos_session_entry_t *session;

    session = find_session(manager, addr);
    if (session == NULL) {
        return;
    }

    reset_session_state(session);
    ++manager->stats.resets;
}

struct m9p_client *pwos_session_manager_get_client(
    pwos_session_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint32_t deadline_ms)
{
    pwos_session_entry_t *session;

    if (pwos_session_manager_update_node(manager, addr, boot_id) != 0) {
        return NULL;
    }

    session = find_session(manager, addr);
    if (session == NULL || session->boot_id != boot_id) {
        if (manager != NULL) {
            ++manager->stats.stale_boot_errors;
        }
        return NULL;
    }

    session->deadline_ms = effective_deadline(manager, deadline_ms);
    return &session->client;
}

int pwos_session_manager_attach(
    pwos_session_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint32_t deadline_ms)
{
    pwos_session_entry_t *session;
    struct m9p_client *client;
    int rc;

    client = pwos_session_manager_get_client(manager, addr, boot_id, deadline_ms);
    if (client == NULL) {
        return PWOS_SESSION_ERR_NO_ROUTE;
    }

    session = find_session(manager, addr);
    if (session == NULL) {
        return PWOS_SESSION_ERR_NO_ROUTE;
    }
    if (session->attached != 0u && client->attached) {
        return 0;
    }

    reset_session_state(session);
    session->deadline_ms = effective_deadline(manager, deadline_ms);
    rc = m9p_client_attach(client, M9P_DEFAULT_MSIZE, M9P_DEFAULT_INFLIGHT, 0u);
    if (rc == 0) {
        session->attached = 1u;
        ++session->attach_ok;
        ++manager->stats.attach_ok;
    } else {
        ++session->attach_fail;
        ++manager->stats.attach_fail;
    }
    return rc;
}

void pwos_session_manager_get_stats(
    const pwos_session_manager_t *manager,
    pwos_session_manager_stats_t *out_stats)
{
    if (manager == NULL || out_stats == NULL) {
        return;
    }

    *out_stats = manager->stats;
}

const char *pwos_session_error_name(int rc)
{
    switch (rc) {
    case PWOS_SESSION_ERR_NO_ROUTE:
        return "no_route";
    case PWOS_SESSION_ERR_DEADLINE:
        return "deadline";
    case PWOS_SESSION_ERR_QUEUE_FULL:
        return "queue_full";
    case PWOS_SESSION_ERR_STALE_BOOT:
        return "stale_boot";
    default:
        break;
    }

    if (rc < 0) {
        return m9p_error_name((uint16_t)(-rc));
    }
    return "ok";
}
