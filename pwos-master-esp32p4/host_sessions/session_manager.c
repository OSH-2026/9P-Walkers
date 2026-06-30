#include "session_manager.h"

#include <string.h>

#include "mini9p_protocol.h"

static void manager_lock(pwos_session_manager_t *manager)
{
    if (manager != NULL && manager->config.lock != NULL) {
        manager->config.lock(manager->config.sync_ctx);
    }
}

static void manager_unlock(pwos_session_manager_t *manager)
{
    if (manager != NULL && manager->config.unlock != NULL) {
        manager->config.unlock(manager->config.sync_ctx);
    }
}

static void client_lock(pwos_session_manager_t *manager, uint8_t index)
{
    if (manager != NULL && manager->config.client_lock != NULL) {
        manager->config.client_lock(manager->config.sync_ctx, index);
    }
}

static void client_unlock(pwos_session_manager_t *manager, uint8_t index)
{
    if (manager != NULL && manager->config.client_unlock != NULL) {
        manager->config.client_unlock(manager->config.sync_ctx, index);
    }
}

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

static pwos_session_entry_t *find_session_locked(
    pwos_session_manager_t *manager,
    uint8_t addr)
{
    size_t i;

    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_SESSIONS; ++i) {
        if (manager->sessions[i].used != 0u && manager->sessions[i].addr == addr) {
            return &manager->sessions[i];
        }
    }
    return NULL;
}

static pwos_session_entry_t *alloc_session_locked(pwos_session_manager_t *manager)
{
    size_t i;

    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_SESSIONS; ++i) {
        if (manager->sessions[i].used == 0u) {
            return &manager->sessions[i];
        }
    }
    return NULL;
}

static uint32_t pending_count_locked(const pwos_session_manager_t *manager)
{
    size_t i;
    uint32_t count = 0u;

    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_PENDING; ++i) {
        if (manager->pending[i].used != 0u) {
            ++count;
        }
    }
    return count;
}

static uint8_t tag_is_active_locked(
    const pwos_session_manager_t *manager,
    uint16_t tag)
{
    size_t i;

    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_PENDING; ++i) {
        if (manager->pending[i].used != 0u && manager->pending[i].wire_tag == tag) {
            return 1u;
        }
    }
    return 0u;
}

static uint16_t alloc_wire_tag_locked(pwos_session_manager_t *manager)
{
    uint32_t attempts;

    for (attempts = 0u; attempts < UINT16_MAX; ++attempts) {
        ++manager->next_wire_tag;
        if (manager->next_wire_tag == 0u) {
            ++manager->next_wire_tag;
        }
        if (tag_is_active_locked(manager, manager->next_wire_tag) == 0u) {
            return manager->next_wire_tag;
        }
    }
    return 0u;
}

static int reserve_pending_locked(
    pwos_session_manager_t *manager,
    const pwos_session_entry_t *session,
    uint8_t data_type,
    uint8_t streaming,
    uint8_t *out_index,
    uint16_t *out_wire_tag)
{
    size_t i;
    uint16_t wire_tag;
    uint32_t count;

    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_PENDING; ++i) {
        if (manager->pending[i].used == 0u) {
            break;
        }
    }
    if (i == PWOS_SESSION_MANAGER_MAX_PENDING) {
        return PWOS_SESSION_ERR_QUEUE_FULL;
    }

    wire_tag = alloc_wire_tag_locked(manager);
    if (wire_tag == 0u) {
        return PWOS_SESSION_ERR_QUEUE_FULL;
    }

    if (manager->config.pending_reset != NULL) {
        manager->config.pending_reset(manager->config.sync_ctx, (uint8_t)i);
    }
    memset(&manager->pending[i], 0, sizeof(manager->pending[i]));
    manager->pending[i].used = 1u;
    manager->pending[i].streaming = streaming;
    manager->pending[i].data_type = data_type;
    manager->pending[i].src_addr = session->addr;
    manager->pending[i].session_index = session->index;
    manager->pending[i].wire_tag = wire_tag;
    manager->pending[i].boot_id = session->boot_id;

    count = pending_count_locked(manager);
    if (count > manager->stats.pending_peak) {
        manager->stats.pending_peak = count;
    }
    *out_index = (uint8_t)i;
    *out_wire_tag = wire_tag;
    return 0;
}

static void release_pending_locked(
    pwos_session_manager_t *manager,
    uint8_t pending_index)
{
    if (pending_index < PWOS_SESSION_MANAGER_MAX_PENDING) {
        memset(&manager->pending[pending_index], 0, sizeof(manager->pending[pending_index]));
    }
}

static uint32_t now_ms(pwos_session_manager_t *manager)
{
    if (manager != NULL && manager->config.now_ms != NULL) {
        return manager->config.now_ms(manager->config.sync_ctx);
    }
    return 0u;
}

static int wait_for_pending(
    pwos_session_manager_t *manager,
    uint8_t pending_index,
    uint32_t deadline_ms,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len,
    uint16_t *out_remote_status,
    uint16_t *out_part_count)
{
    const uint32_t start = now_ms(manager);
    uint32_t remaining = deadline_ms;

    for (;;) {
        pwos_session_pending_t *pending;
        int rc;

        manager_lock(manager);
        pending = &manager->pending[pending_index];
        if (pending->used == 0u) {
            manager_unlock(manager);
            return PWOS_SESSION_ERR_STALE_BOOT;
        }
        if (pending->completed != 0u) {
            rc = (int)pending->status;
            if (rc == 0) {
                if ((size_t)pending->response_len > rx_cap) {
                    rc = -(int)M9P_ERR_EMSIZE;
                } else {
                    if (pending->response_len > 0u) {
                        memcpy(rx_data, pending->response, pending->response_len);
                    }
                    *rx_len = pending->response_len;
                }
                if (out_remote_status != NULL) {
                    *out_remote_status = pending->remote_status;
                }
                if (out_part_count != NULL) {
                    *out_part_count = pending->stream_parts;
                }
            }
            release_pending_locked(manager, pending_index);
            manager_unlock(manager);
            return rc;
        }
        manager_unlock(manager);

        if (manager->config.pending_wait == NULL) {
            rc = PWOS_SESSION_ERR_DEADLINE;
        } else {
            rc = manager->config.pending_wait(
                manager->config.sync_ctx,
                pending_index,
                remaining);
        }

        manager_lock(manager);
        if (manager->pending[pending_index].used != 0u &&
            manager->pending[pending_index].completed != 0u) {
            manager_unlock(manager);
            continue;
        }
        manager_unlock(manager);

        if (rc != 0 || manager->config.now_ms == NULL) {
            manager_lock(manager);
            release_pending_locked(manager, pending_index);
            manager_unlock(manager);
            return rc == PWOS_SESSION_ERR_NO_ROUTE ? rc : PWOS_SESSION_ERR_DEADLINE;
        }

        {
            uint32_t elapsed = now_ms(manager) - start;

            if (elapsed >= deadline_ms) {
                manager_lock(manager);
                release_pending_locked(manager, pending_index);
                manager_unlock(manager);
                return PWOS_SESSION_ERR_DEADLINE;
            }
            remaining = deadline_ms - elapsed;
        }
    }
}

static void record_transport_error(
    pwos_session_manager_t *manager,
    pwos_session_entry_t *session,
    int rc)
{
    manager_lock(manager);
    if (rc == PWOS_SESSION_ERR_DEADLINE) {
        ++session->deadline_errors;
        ++manager->stats.deadline_errors;
    } else if (rc == PWOS_SESSION_ERR_NO_ROUTE) {
        ++session->no_route_errors;
        ++manager->stats.no_route_errors;
    } else if (rc == PWOS_SESSION_ERR_QUEUE_FULL) {
        ++session->queue_full_errors;
        ++manager->stats.queue_full_errors;
    } else if (rc == PWOS_SESSION_ERR_STALE_BOOT) {
        ++manager->stats.stale_boot_errors;
    } else {
        ++session->io_errors;
        ++manager->stats.io_errors;
    }
    manager_unlock(manager);
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
    struct m9p_frame_view tx_view;
    struct m9p_frame_view rx_view;
    uint8_t wire_data[M9P_CLIENT_BUFFER_CAP];
    uint8_t pending_index = 0u;
    uint16_t wire_tag = 0u;
    int rc;

    if (session == NULL || session->manager == NULL || tx_data == NULL ||
        tx_len == 0u || tx_len > sizeof(wire_data) || rx_len == NULL ||
        (rx_cap > 0u && rx_data == NULL) ||
        !m9p_decode_frame(tx_data, tx_len, &tx_view)) {
        return -(int)M9P_ERR_EINVAL;
    }

    manager = session->manager;
    if (manager->config.send == NULL) {
        return -(int)M9P_ERR_ENOTSUP;
    }

    memcpy(wire_data, tx_data, tx_len);
    manager_lock(manager);
    if (session->resetting != 0u) {
        manager_unlock(manager);
        return PWOS_SESSION_ERR_STALE_BOOT;
    }
    rc = reserve_pending_locked(
        manager,
        session,
        0x80u,
        0u,
        &pending_index,
        &wire_tag);
    if (rc == 0) {
        session->last_tx_tag = wire_tag;
        session->last_tx_type = tx_view.type;
    }
    manager_unlock(manager);
    if (rc != 0) {
        record_transport_error(manager, session, rc);
        return rc;
    }

    if (!m9p_retag_frame(wire_data, tx_len, wire_tag)) {
        manager_lock(manager);
        release_pending_locked(manager, pending_index);
        manager_unlock(manager);
        return -(int)M9P_ERR_EIO;
    }

    rc = manager->config.send(
        manager->config.io_ctx,
        0x80u,
        session->addr,
        wire_data,
        (uint16_t)tx_len);
    if (rc != 0) {
        manager_lock(manager);
        release_pending_locked(manager, pending_index);
        manager_unlock(manager);
        record_transport_error(manager, session, rc);
        return rc;
    }

    manager_lock(manager);
    ++session->tx_requests;
    ++manager->stats.tx_requests;
    manager_unlock(manager);
    *rx_len = 0u;

    rc = wait_for_pending(
        manager,
        pending_index,
        session->deadline_ms,
        rx_data,
        rx_cap,
        rx_len,
        NULL,
        NULL);
    if (rc != 0) {
        record_transport_error(manager, session, rc);
        return rc;
    }

    if (!m9p_decode_frame(rx_data, *rx_len, &rx_view)) {
        record_transport_error(manager, session, -(int)M9P_ERR_EIO);
        return -(int)M9P_ERR_EIO;
    }

    manager_lock(manager);
    session->last_rx_tag = rx_view.tag;
    session->last_rx_type = rx_view.type;
    ++session->rx_responses;
    ++manager->stats.rx_responses;
    manager_unlock(manager);

    /* mini9P client 仍校验自己的本地 tag，因此返回前恢复原 tag。 */
    if (!m9p_retag_frame(rx_data, *rx_len, tx_view.tag)) {
        return -(int)M9P_ERR_EIO;
    }
    return 0;
}

static void init_client_for_session(pwos_session_entry_t *session)
{
    m9p_client_init(&session->client, session_transport, session);
}

static size_t cancel_pending_for_session_locked(
    pwos_session_manager_t *manager,
    uint8_t session_index,
    uint8_t signal_indices[PWOS_SESSION_MANAGER_MAX_PENDING])
{
    size_t i;
    size_t count = 0u;

    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_PENDING; ++i) {
        if (manager->pending[i].used != 0u &&
            manager->pending[i].session_index == session_index &&
            manager->pending[i].completed == 0u) {
            manager->pending[i].status = PWOS_SESSION_ERR_STALE_BOOT;
            manager->pending[i].completed = 1u;
            signal_indices[count++] = (uint8_t)i;
            ++manager->stats.pending_cancelled;
        }
    }
    return count;
}

static void signal_pending_indices(
    pwos_session_manager_t *manager,
    const uint8_t *indices,
    size_t count)
{
    size_t i;

    if (manager->config.pending_signal == NULL) {
        return;
    }
    for (i = 0u; i < count; ++i) {
        manager->config.pending_signal(manager->config.sync_ctx, indices[i]);
    }
}

int pwos_session_manager_init(
    pwos_session_manager_t *manager,
    const pwos_session_manager_config_t *config)
{
    if (manager == NULL || config == NULL || config->send == NULL ||
        (config->lock == NULL) != (config->unlock == NULL) ||
        (config->client_lock == NULL) != (config->client_unlock == NULL)) {
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
    uint8_t signal_indices[PWOS_SESSION_MANAGER_MAX_PENDING];
    uint8_t index;
    size_t signal_count = 0u;

    if (manager == NULL || addr == 0u || addr == 0xFFu) {
        return PWOS_SESSION_ERR_NO_ROUTE;
    }

    manager_lock(manager);
    session = find_session_locked(manager, addr);
    if (session == NULL) {
        session = alloc_session_locked(manager);
        if (session == NULL) {
            manager_unlock(manager);
            return PWOS_SESSION_ERR_QUEUE_FULL;
        }
        index = (uint8_t)(session - manager->sessions);
        memset(session, 0, sizeof(*session));
        session->used = 1u;
        session->addr = addr;
        session->index = index;
        session->boot_id = boot_id;
        session->manager = manager;
        init_client_for_session(session);
        manager_unlock(manager);
        return 0;
    }

    if (session->boot_id == boot_id && session->resetting == 0u) {
        manager_unlock(manager);
        return 0;
    }

    index = session->index;
    session->resetting = 1u;
    signal_count = cancel_pending_for_session_locked(manager, index, signal_indices);
    manager_unlock(manager);
    signal_pending_indices(manager, signal_indices, signal_count);

    client_lock(manager, index);
    manager_lock(manager);
    session = &manager->sessions[index];
    session->boot_id = boot_id;
    m9p_client_reset_session(&session->client);
    session->attached = 0u;
    session->resetting = 0u;
    ++manager->stats.resets;
    manager_unlock(manager);
    client_unlock(manager, index);
    return 0;
}

void pwos_session_manager_reset_node(pwos_session_manager_t *manager, uint8_t addr)
{
    pwos_session_entry_t *session;
    uint8_t signal_indices[PWOS_SESSION_MANAGER_MAX_PENDING];
    uint8_t index;
    size_t signal_count;

    if (manager == NULL) {
        return;
    }

    manager_lock(manager);
    session = find_session_locked(manager, addr);
    if (session == NULL) {
        manager_unlock(manager);
        return;
    }
    index = session->index;
    session->resetting = 1u;
    signal_count = cancel_pending_for_session_locked(manager, index, signal_indices);
    manager_unlock(manager);
    signal_pending_indices(manager, signal_indices, signal_count);

    client_lock(manager, index);
    manager_lock(manager);
    m9p_client_reset_session(&manager->sessions[index].client);
    manager->sessions[index].attached = 0u;
    manager->sessions[index].resetting = 0u;
    ++manager->stats.resets;
    manager_unlock(manager);
    client_unlock(manager, index);
}

int pwos_session_manager_acquire_client(
    pwos_session_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint32_t deadline_ms,
    struct m9p_client **out_client,
    uint8_t *out_session_index)
{
    pwos_session_entry_t *session;
    uint8_t index;

    if (manager == NULL || out_client == NULL || out_session_index == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    *out_client = NULL;
    *out_session_index = 0xFFu;

    manager_lock(manager);
    session = find_session_locked(manager, addr);
    if (session == NULL) {
        ++manager->stats.no_route_errors;
        manager_unlock(manager);
        return PWOS_SESSION_ERR_NO_ROUTE;
    }
    if (session->boot_id != boot_id || session->resetting != 0u) {
        ++manager->stats.stale_boot_errors;
        manager_unlock(manager);
        return PWOS_SESSION_ERR_STALE_BOOT;
    }
    index = session->index;
    manager_unlock(manager);

    client_lock(manager, index);
    manager_lock(manager);
    session = &manager->sessions[index];
    if (session->used == 0u || session->addr != addr ||
        session->boot_id != boot_id || session->resetting != 0u) {
        ++manager->stats.stale_boot_errors;
        manager_unlock(manager);
        client_unlock(manager, index);
        return PWOS_SESSION_ERR_STALE_BOOT;
    }
    session->deadline_ms = effective_deadline(manager, deadline_ms);
    *out_client = &session->client;
    *out_session_index = index;
    manager_unlock(manager);
    return 0;
}

void pwos_session_manager_release_client(
    pwos_session_manager_t *manager,
    uint8_t session_index)
{
    if (manager == NULL || session_index >= PWOS_SESSION_MANAGER_MAX_SESSIONS) {
        return;
    }
    client_unlock(manager, session_index);
}

int pwos_session_manager_attach(
    pwos_session_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint32_t deadline_ms)
{
    pwos_session_entry_t *session;
    struct m9p_client *client;
    uint8_t index;
    int rc;

    rc = pwos_session_manager_acquire_client(
        manager,
        addr,
        boot_id,
        deadline_ms,
        &client,
        &index);
    if (rc != 0) {
        return rc;
    }

    session = &manager->sessions[index];
    if (session->attached != 0u && client->attached) {
        pwos_session_manager_release_client(manager, index);
        return 0;
    }

    rc = m9p_client_attach(
        client,
        M9P_DEFAULT_MSIZE,
        M9P_DEFAULT_INFLIGHT,
        0u);
    manager_lock(manager);
    if (rc == 0) {
        session->attached = 1u;
        ++session->attach_ok;
        ++manager->stats.attach_ok;
    } else {
        session->attached = 0u;
        ++session->attach_fail;
        ++manager->stats.attach_fail;
    }
    manager_unlock(manager);
    pwos_session_manager_release_client(manager, index);
    return rc;
}

int pwos_session_manager_deliver_data(
    pwos_session_manager_t *manager,
    uint8_t data_type,
    uint8_t src_addr,
    uint16_t wire_tag,
    const uint8_t *payload,
    size_t payload_len)
{
    uint8_t signal_index = 0xFFu;
    size_t i;

    if (manager == NULL || payload == NULL || payload_len == 0u ||
        payload_len > M9P_CLIENT_BUFFER_CAP || data_type == 0u || wire_tag == 0u) {
        if (manager != NULL) {
            manager_lock(manager);
            ++manager->stats.pending_malformed;
            manager_unlock(manager);
        }
        return -(int)M9P_ERR_EINVAL;
    }

    manager_lock(manager);
    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_PENDING; ++i) {
        pwos_session_pending_t *pending = &manager->pending[i];

        if (pending->used == 0u || pending->completed != 0u ||
            pending->streaming != 0u ||
            pending->data_type != data_type || pending->src_addr != src_addr ||
            pending->wire_tag != wire_tag) {
            continue;
        }
        if (pending->session_index >= PWOS_SESSION_MANAGER_MAX_SESSIONS ||
            manager->sessions[pending->session_index].boot_id != pending->boot_id ||
            manager->sessions[pending->session_index].resetting != 0u) {
            pending->status = PWOS_SESSION_ERR_STALE_BOOT;
        } else {
            memcpy(pending->response, payload, payload_len);
            pending->response_len = (uint16_t)payload_len;
            pending->status = 0;
        }
        pending->completed = 1u;
        signal_index = (uint8_t)i;
        ++manager->stats.pending_delivered;
        break;
    }
    if (signal_index == 0xFFu) {
        ++manager->stats.pending_unmatched;
    }
    manager_unlock(manager);

    if (signal_index != 0xFFu && manager->config.pending_signal != NULL) {
        manager->config.pending_signal(manager->config.sync_ctx, signal_index);
        return 1;
    }
    return signal_index != 0xFFu ? 1 : 0;
}

int pwos_session_manager_deliver_data_part(
    pwos_session_manager_t *manager,
    uint8_t data_type,
    uint8_t src_addr,
    uint16_t wire_tag,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t final,
    uint16_t status_or_part_index)
{
    uint8_t signal_index = 0xFFu;
    uint8_t matched = 0u;
    size_t i;

    if (manager == NULL || data_type == 0u || wire_tag == 0u ||
        (payload_len > 0u && payload == NULL) ||
        payload_len > M9P_CLIENT_BUFFER_CAP) {
        if (manager != NULL) {
            manager_lock(manager);
            ++manager->stats.pending_malformed;
            manager_unlock(manager);
        }
        return -(int)M9P_ERR_EINVAL;
    }

    manager_lock(manager);
    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_PENDING; ++i) {
        pwos_session_pending_t *pending = &manager->pending[i];

        if (pending->used == 0u || pending->completed != 0u ||
            pending->streaming == 0u || pending->data_type != data_type ||
            pending->src_addr != src_addr || pending->wire_tag != wire_tag) {
            continue;
        }
        matched = 1u;
        if (pending->session_index >= PWOS_SESSION_MANAGER_MAX_SESSIONS ||
            manager->sessions[pending->session_index].boot_id != pending->boot_id ||
            manager->sessions[pending->session_index].resetting != 0u) {
            pending->status = PWOS_SESSION_ERR_STALE_BOOT;
            pending->completed = 1u;
        } else if (final == 0u &&
                   status_or_part_index != pending->stream_parts) {
            /* 序号不连续说明发生丢帧、重复或乱序，不能返回残缺聚合结果。 */
            pending->status = -(int)M9P_ERR_EIO;
            pending->completed = 1u;
            ++manager->stats.pending_malformed;
        } else if ((size_t)pending->response_len + payload_len >
                   sizeof(pending->response)) {
            pending->status = -(int)M9P_ERR_EMSIZE;
            pending->completed = 1u;
        } else {
            if (payload_len > 0u) {
                memcpy(
                    pending->response + pending->response_len,
                    payload,
                    payload_len);
                pending->response_len =
                    (uint16_t)(pending->response_len + payload_len);
            }
            if (final == 0u) {
                ++pending->stream_parts;
                ++manager->stats.stream_parts_delivered;
            } else {
                pending->remote_status = status_or_part_index;
                pending->status = 0;
                pending->completed = 1u;
                ++manager->stats.stream_completed;
            }
        }
        if (pending->completed != 0u) {
            signal_index = (uint8_t)i;
            ++manager->stats.pending_delivered;
        }
        break;
    }
    if (matched == 0u) {
        ++manager->stats.pending_unmatched;
    }
    manager_unlock(manager);

    if (signal_index != 0xFFu && manager->config.pending_signal != NULL) {
        manager->config.pending_signal(manager->config.sync_ctx, signal_index);
    }
    return matched != 0u ? 1 : 0;
}

int pwos_session_manager_deliver_mini9p(
    pwos_session_manager_t *manager,
    uint8_t src_addr,
    const uint8_t *payload,
    size_t payload_len)
{
    struct m9p_frame_view view;

    if (payload == NULL || !m9p_decode_frame(payload, payload_len, &view)) {
        if (manager != NULL) {
            manager_lock(manager);
            ++manager->stats.pending_malformed;
            manager_unlock(manager);
        }
        return 0;
    }
    return pwos_session_manager_deliver_data(
        manager, 0x80u, src_addr, view.tag, payload, payload_len);
}

static int request_data_internal(
    pwos_session_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint8_t data_type,
    const uint8_t *request,
    size_t request_len,
    pwos_session_retag_fn retag,
    uint32_t deadline_ms,
    uint8_t *response,
    size_t response_cap,
    size_t *response_len,
    uint16_t *out_wire_tag,
    uint8_t streaming,
    uint16_t *out_remote_status,
    uint16_t *out_part_count)
{
    pwos_session_entry_t *session;
    uint8_t wire_data[M9P_CLIENT_BUFFER_CAP];
    uint8_t pending_index = 0u;
    uint16_t wire_tag = 0u;
    int rc;

    if (manager == NULL || data_type == 0u || request == NULL || request_len == 0u ||
        request_len > sizeof(wire_data) || retag == NULL || response_len == NULL ||
        out_wire_tag == NULL || (response_cap > 0u && response == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }
    *response_len = 0u;
    *out_wire_tag = 0u;
    if (out_remote_status != NULL) {
        *out_remote_status = 0u;
    }
    if (out_part_count != NULL) {
        *out_part_count = 0u;
    }
    memcpy(wire_data, request, request_len);

    manager_lock(manager);
    session = find_session_locked(manager, addr);
    if (session == NULL) {
        ++manager->stats.no_route_errors;
        manager_unlock(manager);
        return PWOS_SESSION_ERR_NO_ROUTE;
    }
    if (session->boot_id != boot_id || session->resetting != 0u) {
        ++manager->stats.stale_boot_errors;
        manager_unlock(manager);
        return PWOS_SESSION_ERR_STALE_BOOT;
    }
    rc = reserve_pending_locked(
        manager, session, data_type, streaming, &pending_index, &wire_tag);
    manager_unlock(manager);
    if (rc != 0) {
        record_transport_error(manager, session, rc);
        return rc;
    }
    *out_wire_tag = wire_tag;
    if (retag(wire_data, request_len, wire_tag) != 0) {
        manager_lock(manager);
        release_pending_locked(manager, pending_index);
        manager_unlock(manager);
        return -(int)M9P_ERR_EIO;
    }

    rc = manager->config.send(
        manager->config.io_ctx,
        data_type,
        addr,
        wire_data,
        (uint16_t)request_len);
    if (rc != 0) {
        manager_lock(manager);
        release_pending_locked(manager, pending_index);
        manager_unlock(manager);
        record_transport_error(manager, session, rc);
        return rc;
    }

    manager_lock(manager);
    ++session->tx_requests;
    ++manager->stats.tx_requests;
    manager_unlock(manager);
    rc = wait_for_pending(
        manager,
        pending_index,
        effective_deadline(manager, deadline_ms),
        response,
        response_cap,
        response_len,
        out_remote_status,
        out_part_count);
    if (rc != 0) {
        record_transport_error(manager, session, rc);
        return rc;
    }

    manager_lock(manager);
    session->last_tx_tag = wire_tag;
    session->last_rx_tag = wire_tag;
    ++session->rx_responses;
    ++manager->stats.rx_responses;
    manager_unlock(manager);
    return 0;
}

int pwos_session_manager_request_data(
    pwos_session_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint8_t data_type,
    const uint8_t *request,
    size_t request_len,
    pwos_session_retag_fn retag,
    uint32_t deadline_ms,
    uint8_t *response,
    size_t response_cap,
    size_t *response_len,
    uint16_t *out_wire_tag)
{
    return request_data_internal(
        manager, addr, boot_id, data_type, request, request_len, retag,
        deadline_ms, response, response_cap, response_len, out_wire_tag,
        0u, NULL, NULL);
}

int pwos_session_manager_request_stream_data(
    pwos_session_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint8_t data_type,
    const uint8_t *request,
    size_t request_len,
    pwos_session_retag_fn retag,
    uint32_t deadline_ms,
    uint8_t *response,
    size_t response_cap,
    size_t *response_len,
    uint16_t *out_wire_tag,
    uint16_t *out_remote_status,
    uint16_t *out_part_count)
{
    if (out_remote_status == NULL || out_part_count == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    return request_data_internal(
        manager, addr, boot_id, data_type, request, request_len, retag,
        deadline_ms, response, response_cap, response_len, out_wire_tag,
        1u, out_remote_status, out_part_count);
}

int pwos_session_manager_send_data(
    pwos_session_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint8_t data_type,
    const uint8_t *payload,
    uint16_t payload_len)
{
    pwos_session_entry_t *session;
    int rc;

    if (manager == NULL || data_type == 0u || payload == NULL || payload_len == 0u) {
        return -(int)M9P_ERR_EINVAL;
    }
    manager_lock(manager);
    session = find_session_locked(manager, addr);
    if (session == NULL) {
        ++manager->stats.no_route_errors;
        manager_unlock(manager);
        return PWOS_SESSION_ERR_NO_ROUTE;
    }
    if (session->boot_id != boot_id || session->resetting != 0u) {
        ++manager->stats.stale_boot_errors;
        manager_unlock(manager);
        return PWOS_SESSION_ERR_STALE_BOOT;
    }
    manager_unlock(manager);

    rc = manager->config.send(
        manager->config.io_ctx, data_type, addr, payload, payload_len);
    if (rc != 0) {
        record_transport_error(manager, session, rc);
        return rc;
    }
    manager_lock(manager);
    ++session->tx_requests;
    ++manager->stats.tx_requests;
    manager_unlock(manager);
    return 0;
}

int pwos_session_manager_send_oneway_data(
    pwos_session_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint8_t data_type,
    const uint8_t *payload,
    size_t payload_len,
    pwos_session_retag_fn retag,
    uint16_t *out_wire_tag)
{
    pwos_session_entry_t *session;
    uint8_t wire_data[M9P_CLIENT_BUFFER_CAP];
    uint16_t wire_tag;

    if (manager == NULL || data_type == 0u || payload == NULL || payload_len == 0u ||
        payload_len > sizeof(wire_data) || retag == NULL || out_wire_tag == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    *out_wire_tag = 0u;
    memcpy(wire_data, payload, payload_len);

    manager_lock(manager);
    session = find_session_locked(manager, addr);
    if (session == NULL) {
        ++manager->stats.no_route_errors;
        manager_unlock(manager);
        return PWOS_SESSION_ERR_NO_ROUTE;
    }
    if (session->boot_id != boot_id || session->resetting != 0u) {
        ++manager->stats.stale_boot_errors;
        manager_unlock(manager);
        return PWOS_SESSION_ERR_STALE_BOOT;
    }
    wire_tag = alloc_wire_tag_locked(manager);
    manager_unlock(manager);
    if (wire_tag == 0u) {
        record_transport_error(manager, session, PWOS_SESSION_ERR_QUEUE_FULL);
        return PWOS_SESSION_ERR_QUEUE_FULL;
    }
    if (retag(wire_data, payload_len, wire_tag) != 0) {
        return -(int)M9P_ERR_EIO;
    }
    *out_wire_tag = wire_tag;
    return pwos_session_manager_send_data(
        manager,
        addr,
        boot_id,
        data_type,
        wire_data,
        (uint16_t)payload_len);
}

void pwos_session_manager_get_stats(
    const pwos_session_manager_t *manager,
    pwos_session_manager_stats_t *out_stats)
{
    pwos_session_manager_t *mutable_manager = (pwos_session_manager_t *)manager;

    if (manager == NULL || out_stats == NULL) {
        return;
    }
    manager_lock(mutable_manager);
    *out_stats = manager->stats;
    manager_unlock(mutable_manager);
}

int pwos_session_manager_get_snapshot(
    pwos_session_manager_t *manager,
    size_t index,
    pwos_session_snapshot_t *out_snapshot)
{
    const pwos_session_entry_t *session;

    if (manager == NULL || out_snapshot == NULL ||
        index >= PWOS_SESSION_MANAGER_MAX_SESSIONS) {
        return -(int)M9P_ERR_EINVAL;
    }

    manager_lock(manager);
    session = &manager->sessions[index];
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->used = session->used;
    out_snapshot->resetting = session->resetting;
    out_snapshot->addr = session->addr;
    out_snapshot->attached = session->attached;
    out_snapshot->boot_id = session->boot_id;
    out_snapshot->deadline_ms = session->deadline_ms;
    out_snapshot->last_tx_tag = session->last_tx_tag;
    out_snapshot->last_rx_tag = session->last_rx_tag;
    out_snapshot->last_tx_type = session->last_tx_type;
    out_snapshot->last_rx_type = session->last_rx_type;
    out_snapshot->tx_requests = session->tx_requests;
    out_snapshot->rx_responses = session->rx_responses;
    out_snapshot->deadline_errors = session->deadline_errors;
    out_snapshot->no_route_errors = session->no_route_errors;
    out_snapshot->queue_full_errors = session->queue_full_errors;
    out_snapshot->io_errors = session->io_errors;
    manager_unlock(manager);
    return out_snapshot->used != 0u ? 0 : -(int)M9P_ERR_ENOENT;
}

const char *pwos_session_error_name(int rc)
{
    switch (rc) {
    case 0:
        return "ok";
    case PWOS_SESSION_ERR_NO_ROUTE:
        return "no_route";
    case PWOS_SESSION_ERR_DEADLINE:
        return "deadline";
    case PWOS_SESSION_ERR_QUEUE_FULL:
        return "queue_full";
    case PWOS_SESSION_ERR_STALE_BOOT:
        return "stale_boot";
    default:
        return "mini9p_or_io_error";
    }
}
