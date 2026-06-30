#include "job_manager.h"

#include <string.h>

#include "mini9p_protocol.h"
#include "pwos_link_frame.h"

static void manager_lock(pwos_job_manager_t *manager)
{
    if (manager != NULL && manager->config.lock != NULL) {
        manager->config.lock(manager->config.lock_ctx);
    }
}

static void manager_unlock(pwos_job_manager_t *manager)
{
    if (manager != NULL && manager->config.unlock != NULL) {
        manager->config.unlock(manager->config.lock_ctx);
    }
}

static int job_retag_adapter(
    uint8_t *frame,
    size_t frame_len,
    uint16_t wire_tag)
{
    return pwos_job_retag(frame, frame_len, wire_tag);
}

static uint32_t effective_deadline(
    uint32_t deadline_ms)
{
    return deadline_ms == 0u ? PWOS_JOB_MANAGER_DEFAULT_DEADLINE_MS : deadline_ms;
}

static int terminal_state(uint8_t state)
{
    return state == PWOS_JOB_STATE_DONE || state == PWOS_JOB_STATE_FAILED ||
        state == PWOS_JOB_STATE_CANCELLED || state == PWOS_JOB_STATE_LOST;
}

static pwos_job_entry_t *find_job_locked(
    pwos_job_manager_t *manager,
    uint32_t host_job_id)
{
    size_t i;

    for (i = 0u; i < PWOS_JOB_MANAGER_MAX_JOBS; ++i) {
        if (manager->jobs[i].used != 0u &&
            manager->jobs[i].host_job_id == host_job_id) {
            return &manager->jobs[i];
        }
    }
    return NULL;
}

static pwos_job_entry_t *alloc_job_locked(pwos_job_manager_t *manager)
{
    pwos_job_entry_t *oldest = NULL;
    size_t i;

    for (i = 0u; i < PWOS_JOB_MANAGER_MAX_JOBS; ++i) {
        if (manager->jobs[i].used == 0u) {
            return &manager->jobs[i];
        }
        if (terminal_state(manager->jobs[i].state) &&
            (oldest == NULL || manager->jobs[i].sequence < oldest->sequence)) {
            oldest = &manager->jobs[i];
        }
    }
    if (oldest != NULL) {
        ++manager->stats.slots_reused;
    }
    return oldest;
}

static int request(
    pwos_job_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint8_t kind,
    uint8_t kernel,
    uint32_t remote_job_id,
    const uint8_t *payload,
    uint16_t payload_len,
    uint32_t deadline_ms,
    uint8_t expected_kind,
    uint8_t *response_frame,
    size_t response_cap,
    size_t *out_response_len,
    pwos_job_frame_view_t *out_response)
{
    uint8_t request_frame[PWOS_JOB_MAX_FRAME_LEN];
    size_t request_len = 0u;
    uint16_t wire_tag = 0u;
    int rc;

    if (pwos_job_encode(
            kind,
            PWOS_JOB_STATE_EMPTY,
            kernel,
            0u,
            PWOS_JOB_STATUS_OK,
            remote_job_id,
            0u,
            0u,
            payload,
            payload_len,
            request_frame,
            sizeof(request_frame),
            &request_len) != 0) {
        return -(int)M9P_ERR_EINVAL;
    }
    rc = pwos_session_manager_request_data(
        manager->sessions,
        addr,
        boot_id,
        (uint8_t)PWOS_LINK_TYPE_DATA_JOB,
        request_frame,
        request_len,
        job_retag_adapter,
        effective_deadline(deadline_ms),
        response_frame,
        response_cap,
        out_response_len,
        &wire_tag);
    if (rc != 0) {
        return rc;
    }
    if (pwos_job_decode(response_frame, *out_response_len, out_response) != 0 ||
        out_response->request_id != wire_tag ||
        out_response->kind != expected_kind) {
        return -(int)M9P_ERR_EIO;
    }
    return 0;
}

static void mark_transport_result_locked(
    pwos_job_manager_t *manager,
    pwos_job_entry_t *job,
    int rc)
{
    job->last_error = rc;
    manager->stats.last_error = rc;
    if (rc != 0) {
        ++manager->stats.transport_errors;
        if (job->state != PWOS_JOB_STATE_LOST) {
            job->state = PWOS_JOB_STATE_LOST;
            ++manager->stats.lost;
        }
    }
}

static void update_from_response_locked(
    pwos_job_manager_t *manager,
    pwos_job_entry_t *job,
    const pwos_job_frame_view_t *response)
{
    job->remote_status = response->status;
    job->remote_job_id = response->job_id;
    job->progress_permille = response->progress_permille;
    job->result_len = response->result_len;
    job->last_error = 0;
    manager->stats.last_error = 0;
    if (response->status == PWOS_JOB_STATUS_OK) {
        job->state = response->state;
    } else if (response->status == PWOS_JOB_STATUS_NOT_READY) {
        /* NOT_READY 是正常轮询结果，仍要采纳远端的 RUNNING 状态与进度。 */
        if (response->job_id != 0u) {
            job->state = response->state;
        }
    } else {
        ++manager->stats.remote_errors;
        if (response->status == PWOS_JOB_STATUS_CANCELLED) {
            job->state = PWOS_JOB_STATE_CANCELLED;
        } else if (response->status == PWOS_JOB_STATUS_NOT_FOUND) {
            if (job->state != PWOS_JOB_STATE_LOST) {
                job->state = PWOS_JOB_STATE_LOST;
                ++manager->stats.lost;
            }
        } else {
            job->state = PWOS_JOB_STATE_FAILED;
        }
    }
}

int pwos_job_manager_init(
    pwos_job_manager_t *manager,
    pwos_session_manager_t *sessions,
    const pwos_job_manager_config_t *config)
{
    if (manager == NULL || sessions == NULL ||
        (config != NULL && (config->lock == NULL) != (config->unlock == NULL))) {
        return -(int)M9P_ERR_EINVAL;
    }
    memset(manager, 0, sizeof(*manager));
    manager->sessions = sessions;
    if (config != NULL) {
        manager->config = *config;
    }
    return 0;
}

int pwos_job_manager_caps(
    pwos_job_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint8_t *out_caps,
    uint16_t *in_out_len,
    uint32_t deadline_ms,
    uint16_t *out_remote_status)
{
    uint8_t response_frame[PWOS_JOB_MAX_FRAME_LEN];
    pwos_job_frame_view_t response;
    size_t response_len = 0u;
    int rc;

    if (manager == NULL || in_out_len == NULL || out_remote_status == NULL ||
        (*in_out_len > 0u && out_caps == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }
    rc = request(
        manager, addr, boot_id, PWOS_JOB_KIND_CAPS_REQUEST,
        PWOS_JOB_KERNEL_NONE, 0u, NULL, 0u, deadline_ms,
        PWOS_JOB_KIND_CAPS_RESPONSE, response_frame, sizeof(response_frame),
        &response_len, &response);
    manager_lock(manager);
    ++manager->stats.caps_requests;
    manager->stats.last_error = rc;
    if (rc != 0) {
        ++manager->stats.transport_errors;
        manager_unlock(manager);
        return rc;
    }
    manager_unlock(manager);
    if (response.payload_len > *in_out_len) {
        return -(int)M9P_ERR_EMSIZE;
    }
    if (response.payload_len > 0u) {
        memcpy(out_caps, response.payload, response.payload_len);
    }
    *in_out_len = response.payload_len;
    *out_remote_status = response.status;
    return 0;
}

int pwos_job_manager_submit(
    pwos_job_manager_t *manager,
    const char *target,
    uint8_t addr,
    uint32_t boot_id,
    uint8_t kernel,
    const uint8_t *input,
    uint16_t input_len,
    uint32_t deadline_ms,
    uint32_t *out_host_job_id,
    uint16_t *out_remote_status)
{
    uint8_t response_frame[PWOS_JOB_MAX_FRAME_LEN];
    pwos_job_frame_view_t response;
    pwos_job_entry_t *job;
    size_t response_len = 0u;
    uint32_t host_job_id;
    int rc;

    if (manager == NULL || target == NULL || target[0] == '\0' ||
        strlen(target) >= PWOS_JOB_MANAGER_TARGET_CAP || input == NULL ||
        input_len == 0u || input_len > PWOS_JOB_MAX_PAYLOAD_LEN ||
        out_host_job_id == NULL || out_remote_status == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    manager_lock(manager);
    job = alloc_job_locked(manager);
    if (job == NULL) {
        manager_unlock(manager);
        return PWOS_SESSION_ERR_QUEUE_FULL;
    }
    memset(job, 0, sizeof(*job));
    ++manager->next_host_job_id;
    if (manager->next_host_job_id == 0u) {
        ++manager->next_host_job_id;
    }
    host_job_id = manager->next_host_job_id;
    job->used = 1u;
    job->addr = addr;
    job->boot_id = boot_id;
    job->kernel = kernel;
    job->state = PWOS_JOB_STATE_ASSIGNED;
    job->host_job_id = host_job_id;
    job->sequence = ++manager->next_sequence;
    job->input_len = input_len;
    memcpy(job->target, target, strlen(target) + 1u);
    memcpy(job->input, input, input_len);
    ++manager->stats.submitted;
    manager->stats.last_host_job_id = host_job_id;
    manager_unlock(manager);

    rc = request(
        manager, addr, boot_id, PWOS_JOB_KIND_SUBMIT, kernel, 0u,
        input, input_len, deadline_ms, PWOS_JOB_KIND_SUBMIT_ACK,
        response_frame, sizeof(response_frame), &response_len, &response);
    manager_lock(manager);
    job = find_job_locked(manager, host_job_id);
    if (job != NULL) {
        if (rc != 0) {
            mark_transport_result_locked(manager, job, rc);
        } else {
            update_from_response_locked(manager, job, &response);
        }
        *out_remote_status = job->remote_status;
    }
    manager_unlock(manager);
    *out_host_job_id = host_job_id;
    return rc;
}

static int request_existing(
    pwos_job_manager_t *manager,
    uint32_t host_job_id,
    uint8_t request_kind,
    uint8_t response_kind,
    uint32_t deadline_ms,
    uint8_t *out_payload,
    uint16_t *in_out_payload_len,
    pwos_job_entry_t *out_entry,
    uint16_t *out_remote_status)
{
    uint8_t response_frame[PWOS_JOB_MAX_FRAME_LEN];
    pwos_job_frame_view_t response;
    pwos_job_entry_t copy;
    pwos_job_entry_t *job;
    size_t response_len = 0u;
    int rc;

    if (manager == NULL || out_remote_status == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    manager_lock(manager);
    job = find_job_locked(manager, host_job_id);
    if (job == NULL || job->remote_job_id == 0u) {
        manager_unlock(manager);
        return PWOS_SESSION_ERR_NO_ROUTE;
    }
    /* LOST 绑定的是旧执行实例，只允许显式 retry，禁止被新 boot 的同号 job 复活。 */
    if (job->state == PWOS_JOB_STATE_LOST) {
        manager_unlock(manager);
        return PWOS_SESSION_ERR_STALE_BOOT;
    }
    copy = *job;
    manager_unlock(manager);

    rc = request(
        manager, copy.addr, copy.boot_id, request_kind,
        PWOS_JOB_KERNEL_NONE, copy.remote_job_id, NULL, 0u, deadline_ms,
        response_kind, response_frame, sizeof(response_frame),
        &response_len, &response);
    manager_lock(manager);
    job = find_job_locked(manager, host_job_id);
    if (job == NULL) {
        manager_unlock(manager);
        return PWOS_SESSION_ERR_STALE_BOOT;
    }
    if (rc != 0) {
        mark_transport_result_locked(manager, job, rc);
    } else {
        update_from_response_locked(manager, job, &response);
    }
    *out_remote_status = job->remote_status;
    copy = *job;
    manager_unlock(manager);

    if (rc == 0 && out_payload != NULL && in_out_payload_len != NULL) {
        if (response.payload_len > *in_out_payload_len) {
            return -(int)M9P_ERR_EMSIZE;
        }
        if (response.payload_len > 0u) {
            memcpy(out_payload, response.payload, response.payload_len);
        }
        *in_out_payload_len = response.payload_len;
    }
    if (out_entry != NULL) {
        *out_entry = copy;
    }
    return rc;
}

int pwos_job_manager_status(
    pwos_job_manager_t *manager,
    uint32_t host_job_id,
    uint32_t deadline_ms,
    pwos_job_entry_t *out_entry,
    uint16_t *out_remote_status)
{
    int rc;

    if (manager == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    manager_lock(manager);
    ++manager->stats.status_requests;
    manager_unlock(manager);
    rc = request_existing(
        manager, host_job_id, PWOS_JOB_KIND_STATUS_REQUEST,
        PWOS_JOB_KIND_STATUS_RESPONSE, deadline_ms,
        NULL, NULL, out_entry, out_remote_status);
    return rc;
}

int pwos_job_manager_result(
    pwos_job_manager_t *manager,
    uint32_t host_job_id,
    uint32_t deadline_ms,
    uint8_t *out_result,
    uint16_t *in_out_len,
    pwos_job_entry_t *out_entry,
    uint16_t *out_remote_status)
{
    if (manager == NULL || in_out_len == NULL ||
        (*in_out_len > 0u && out_result == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }
    manager_lock(manager);
    ++manager->stats.result_requests;
    manager_unlock(manager);
    return request_existing(
        manager, host_job_id, PWOS_JOB_KIND_RESULT_REQUEST,
        PWOS_JOB_KIND_RESULT_RESPONSE, deadline_ms,
        out_result, in_out_len, out_entry, out_remote_status);
}

int pwos_job_manager_cancel(
    pwos_job_manager_t *manager,
    uint32_t host_job_id,
    uint32_t deadline_ms,
    pwos_job_entry_t *out_entry,
    uint16_t *out_remote_status)
{
    pwos_job_entry_t local_entry;
    pwos_job_entry_t *entry = out_entry == NULL ? &local_entry : out_entry;
    pwos_job_entry_t before;
    int rc;

    if (manager == NULL || out_remote_status == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    memset(&before, 0, sizeof(before));
    (void)pwos_job_manager_get(manager, host_job_id, &before);
    rc = request_existing(
        manager, host_job_id, PWOS_JOB_KIND_CANCEL_REQUEST,
        PWOS_JOB_KIND_CANCEL_ACK, deadline_ms,
        NULL, NULL, entry, out_remote_status);

    if (rc == 0 && *out_remote_status == PWOS_JOB_STATUS_OK &&
        entry->state == PWOS_JOB_STATE_CANCELLED &&
        before.state != PWOS_JOB_STATE_CANCELLED) {
        manager_lock(manager);
        ++manager->stats.cancelled;
        manager_unlock(manager);
    }
    return rc;
}

int pwos_job_manager_retry(
    pwos_job_manager_t *manager,
    uint32_t host_job_id,
    uint8_t addr,
    uint32_t boot_id,
    uint32_t deadline_ms,
    uint32_t *out_new_host_job_id,
    uint16_t *out_remote_status)
{
    pwos_job_entry_t old;
    int rc;

    if (pwos_job_manager_get(manager, host_job_id, &old) != 0 ||
        !terminal_state(old.state)) {
        return -(int)M9P_ERR_EBUSY;
    }
    rc = pwos_job_manager_submit(
        manager, old.target, addr, boot_id, old.kernel,
        old.input, old.input_len, deadline_ms,
        out_new_host_job_id, out_remote_status);
    if (rc == 0) {
        manager_lock(manager);
        ++manager->stats.retried;
        manager_unlock(manager);
    }
    return rc;
}

int pwos_job_manager_get(
    pwos_job_manager_t *manager,
    uint32_t host_job_id,
    pwos_job_entry_t *out_entry)
{
    pwos_job_entry_t *job;

    if (manager == NULL || out_entry == NULL || host_job_id == 0u) {
        return -(int)M9P_ERR_EINVAL;
    }
    manager_lock(manager);
    job = find_job_locked(manager, host_job_id);
    if (job == NULL) {
        manager_unlock(manager);
        return -(int)M9P_ERR_ENOENT;
    }
    *out_entry = *job;
    manager_unlock(manager);
    return 0;
}

int pwos_job_manager_get_at(
    pwos_job_manager_t *manager,
    size_t index,
    pwos_job_entry_t *out_entry)
{
    if (manager == NULL || out_entry == NULL || index >= PWOS_JOB_MANAGER_MAX_JOBS) {
        return -(int)M9P_ERR_EINVAL;
    }
    manager_lock(manager);
    if (manager->jobs[index].used == 0u) {
        manager_unlock(manager);
        return -(int)M9P_ERR_ENOENT;
    }
    *out_entry = manager->jobs[index];
    manager_unlock(manager);
    return 0;
}

void pwos_job_manager_get_stats(
    pwos_job_manager_t *manager,
    pwos_job_manager_stats_t *out_stats)
{
    if (manager == NULL || out_stats == NULL) {
        return;
    }
    manager_lock(manager);
    *out_stats = manager->stats;
    manager_unlock(manager);
}

size_t pwos_job_manager_mark_node_lost(
    pwos_job_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    int32_t error)
{
    size_t i;
    size_t marked = 0u;

    if (manager == NULL) {
        return 0u;
    }
    manager_lock(manager);
    for (i = 0u; i < PWOS_JOB_MANAGER_MAX_JOBS; ++i) {
        pwos_job_entry_t *job = &manager->jobs[i];

        if (job->used == 0u || job->addr != addr || job->boot_id != boot_id ||
            terminal_state(job->state)) {
            continue;
        }
        job->state = PWOS_JOB_STATE_LOST;
        job->last_error = error;
        ++manager->stats.lost;
        ++marked;
    }
    if (marked > 0u) {
        manager->stats.last_error = error;
    }
    manager_unlock(manager);
    return marked;
}
