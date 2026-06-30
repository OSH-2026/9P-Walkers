#include "compute_worker.h"

#include <stdio.h>
#include <string.h>

#include "pwos_smallpt.h"

static void worker_lock(pwos_compute_worker_t *worker)
{
    if (worker != NULL && worker->config.lock != NULL) {
        worker->config.lock(worker->config.lock_ctx);
    }
}

static void worker_unlock(pwos_compute_worker_t *worker)
{
    if (worker != NULL && worker->config.unlock != NULL) {
        worker->config.unlock(worker->config.lock_ctx);
    }
}

static int terminal_state(uint8_t state)
{
    return state == PWOS_JOB_STATE_DONE || state == PWOS_JOB_STATE_FAILED ||
        state == PWOS_JOB_STATE_CANCELLED;
}

static pwos_compute_job_t *find_job_locked(
    pwos_compute_worker_t *worker,
    uint8_t owner_addr,
    uint32_t job_id)
{
    size_t i;

    for (i = 0u; i < PWOS_COMPUTE_MAX_JOBS; ++i) {
        if (worker->jobs[i].used != 0u &&
            worker->jobs[i].owner_addr == owner_addr &&
            worker->jobs[i].job_id == job_id) {
            return &worker->jobs[i];
        }
    }
    return NULL;
}

static pwos_compute_job_t *alloc_job_locked(pwos_compute_worker_t *worker)
{
    pwos_compute_job_t *oldest = NULL;
    size_t i;

    for (i = 0u; i < PWOS_COMPUTE_MAX_JOBS; ++i) {
        if (worker->jobs[i].used == 0u) {
            return &worker->jobs[i];
        }
        if (terminal_state(worker->jobs[i].state) &&
            (oldest == NULL || worker->jobs[i].sequence < oldest->sequence)) {
            oldest = &worker->jobs[i];
        }
    }
    if (oldest != NULL) {
        ++worker->stats.slots_reused;
    }
    return oldest;
}

static uint16_t validate_input(
    uint8_t kernel,
    const uint8_t *input,
    uint16_t input_len,
    uint16_t *out_result_len,
    uint32_t *out_work_total)
{
    uint32_t expected;
    uint32_t result_len;

    if (input == NULL || input_len == 0u || out_result_len == NULL ||
        out_work_total == NULL || input_len > PWOS_COMPUTE_INPUT_CAP) {
        return PWOS_JOB_STATUS_BAD_REQUEST;
    }
    switch (kernel) {
    case PWOS_JOB_KERNEL_HASH:
        *out_result_len = 4u;
        *out_work_total = input_len;
        return PWOS_JOB_STATUS_OK;
    case PWOS_JOB_KERNEL_VECTOR_ADD:
    {
        uint16_t count;

        if (input_len < 2u) {
            return PWOS_JOB_STATUS_BAD_REQUEST;
        }
        count = pwos_job_get_le16(input);
        expected = 2u + (uint32_t)count * 4u;
        result_len = 2u + (uint32_t)count * 4u;
        if (count == 0u || count > 32u || expected != input_len ||
            result_len > PWOS_COMPUTE_RESULT_CAP) {
            return PWOS_JOB_STATUS_BAD_REQUEST;
        }
        *out_result_len = (uint16_t)result_len;
        *out_work_total = count;
        return PWOS_JOB_STATUS_OK;
    }
    case PWOS_JOB_KERNEL_MATMUL:
    {
        uint8_t rows;
        uint8_t inner;
        uint8_t cols;

        if (input_len < 4u) {
            return PWOS_JOB_STATUS_BAD_REQUEST;
        }
        rows = input[0];
        inner = input[1];
        cols = input[2];
        expected = 4u + 2u * ((uint32_t)rows * inner + (uint32_t)inner * cols);
        result_len = 4u + 4u * (uint32_t)rows * cols;
        if (rows == 0u || inner == 0u || cols == 0u ||
            rows > 8u || inner > 8u || cols > 8u || expected != input_len ||
            result_len > PWOS_COMPUTE_RESULT_CAP) {
            return PWOS_JOB_STATUS_BAD_REQUEST;
        }
        *out_result_len = (uint16_t)result_len;
        *out_work_total = (uint32_t)rows * cols;
        return PWOS_JOB_STATUS_OK;
    }
    case PWOS_JOB_KERNEL_MANDELBROT:
    {
        uint8_t width;
        uint8_t height;
        uint16_t max_iter;

        if (input_len != 20u) {
            return PWOS_JOB_STATUS_BAD_REQUEST;
        }
        width = input[0];
        height = input[1];
        max_iter = pwos_job_get_le16(input + 2u);
        result_len = 4u + (uint32_t)width * height;
        if (width == 0u || height == 0u || width > 16u || height > 16u ||
            max_iter == 0u || max_iter > 255u ||
            pwos_job_get_i32(input + 12u) == 0 ||
            pwos_job_get_i32(input + 16u) == 0 ||
            result_len > PWOS_COMPUTE_RESULT_CAP) {
            return PWOS_JOB_STATUS_BAD_REQUEST;
        }
        *out_result_len = (uint16_t)result_len;
        *out_work_total = (uint32_t)width * height;
        return PWOS_JOB_STATUS_OK;
    }
    case PWOS_JOB_KERNEL_RAYTRACE_TILE:
    {
        pwos_render_request_t request;

        if (pwos_render_decode_request(input, input_len, &request) != 0 ||
            pwos_render_result_len(&request) > PWOS_COMPUTE_RESULT_CAP) {
            return PWOS_JOB_STATUS_BAD_REQUEST;
        }
        *out_result_len = pwos_render_result_len(&request);
        *out_work_total = (uint32_t)request.tile_w * request.tile_h;
        return PWOS_JOB_STATUS_OK;
    }
    default:
        return PWOS_JOB_STATUS_UNSUPPORTED;
    }
}

static void prepare_result(pwos_compute_job_t *job)
{
    memset(job->result, 0, sizeof(job->result));
    if (job->kernel == PWOS_JOB_KERNEL_HASH) {
        job->accumulator = 2166136261u;
    } else if (job->kernel == PWOS_JOB_KERNEL_VECTOR_ADD) {
        pwos_job_put_le16(job->result, pwos_job_get_le16(job->input));
    } else if (job->kernel == PWOS_JOB_KERNEL_MATMUL) {
        job->result[0] = job->input[0];
        job->result[1] = job->input[2];
    } else if (job->kernel == PWOS_JOB_KERNEL_MANDELBROT) {
        job->result[0] = job->input[0];
        job->result[1] = job->input[1];
        pwos_job_put_le16(job->result + 2u, pwos_job_get_le16(job->input + 2u));
    } else if (job->kernel == PWOS_JOB_KERNEL_RAYTRACE_TILE) {
        pwos_render_request_t request;

        if (pwos_render_decode_request(job->input, job->input_len, &request) == 0) {
            pwos_render_write_result_header(&request, job->result);
        }
    }
}

static void snapshot_job(
    const pwos_compute_job_t *job,
    pwos_compute_job_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->state = job->state;
    out_snapshot->kernel = job->kernel;
    out_snapshot->owner_addr = job->owner_addr;
    out_snapshot->job_id = job->job_id;
    out_snapshot->progress_permille = job->progress_permille;
    out_snapshot->result_len = job->result_len;
    memcpy(out_snapshot->log, job->log, sizeof(out_snapshot->log));
}

int pwos_compute_worker_init(
    pwos_compute_worker_t *worker,
    const pwos_compute_worker_config_t *config)
{
    if (worker == NULL ||
        (config != NULL && (config->lock == NULL) != (config->unlock == NULL))) {
        return -1;
    }
    memset(worker, 0, sizeof(*worker));
    if (config != NULL) {
        worker->config = *config;
    }
    return 0;
}

uint16_t pwos_compute_worker_submit(
    pwos_compute_worker_t *worker,
    uint8_t owner_addr,
    uint8_t kernel,
    const uint8_t *input,
    uint16_t input_len,
    uint32_t *out_job_id)
{
    pwos_compute_job_t *job;
    uint16_t result_len;
    uint32_t work_total;
    uint16_t status;

    if (worker == NULL || owner_addr == 0xFFu || out_job_id == NULL) {
        return PWOS_JOB_STATUS_BAD_REQUEST;
    }
    status = validate_input(kernel, input, input_len, &result_len, &work_total);
    if (status != PWOS_JOB_STATUS_OK) {
        worker_lock(worker);
        ++worker->stats.rejected;
        worker_unlock(worker);
        return status;
    }

    worker_lock(worker);
    job = alloc_job_locked(worker);
    if (job == NULL) {
        ++worker->stats.rejected;
        worker_unlock(worker);
        return PWOS_JOB_STATUS_BUSY;
    }
    memset(job, 0, sizeof(*job));
    ++worker->next_job_id;
    if (worker->next_job_id == 0u) {
        ++worker->next_job_id;
    }
    job->used = 1u;
    job->owner_addr = owner_addr;
    job->kernel = kernel;
    job->state = PWOS_JOB_STATE_QUEUED;
    job->job_id = worker->next_job_id;
    job->sequence = ++worker->next_sequence;
    job->input_len = input_len;
    job->result_len = result_len;
    job->work_total = work_total;
    memcpy(job->input, input, input_len);
    (void)snprintf(job->log, sizeof(job->log), "queued");
    prepare_result(job);
    *out_job_id = job->job_id;
    ++worker->stats.submitted;
    ++worker->stats.queued_jobs;
    worker_unlock(worker);
    return PWOS_JOB_STATUS_OK;
}

int pwos_compute_worker_get_slot_snapshot(
    pwos_compute_worker_t *worker,
    size_t index,
    pwos_compute_job_snapshot_t *out_snapshot)
{
    if (worker == NULL || out_snapshot == NULL || index >= PWOS_COMPUTE_MAX_JOBS) {
        return -1;
    }
    worker_lock(worker);
    if (worker->jobs[index].used == 0u) {
        worker_unlock(worker);
        return -1;
    }
    snapshot_job(&worker->jobs[index], out_snapshot);
    worker_unlock(worker);
    return 0;
}

uint16_t pwos_compute_worker_get_snapshot(
    pwos_compute_worker_t *worker,
    uint8_t owner_addr,
    uint32_t job_id,
    pwos_compute_job_snapshot_t *out_snapshot)
{
    pwos_compute_job_t *job;

    if (worker == NULL || out_snapshot == NULL || job_id == 0u) {
        return PWOS_JOB_STATUS_BAD_REQUEST;
    }
    worker_lock(worker);
    job = find_job_locked(worker, owner_addr, job_id);
    if (job == NULL) {
        worker_unlock(worker);
        return PWOS_JOB_STATUS_NOT_FOUND;
    }
    snapshot_job(job, out_snapshot);
    worker_unlock(worker);
    return PWOS_JOB_STATUS_OK;
}

uint16_t pwos_compute_worker_get_result(
    pwos_compute_worker_t *worker,
    uint8_t owner_addr,
    uint32_t job_id,
    uint8_t *out_result,
    uint16_t out_cap,
    uint16_t *out_len,
    pwos_compute_job_snapshot_t *out_snapshot)
{
    pwos_compute_job_t *job;
    uint16_t status = PWOS_JOB_STATUS_OK;

    if (worker == NULL || out_len == NULL || job_id == 0u ||
        (out_cap > 0u && out_result == NULL)) {
        return PWOS_JOB_STATUS_BAD_REQUEST;
    }
    *out_len = 0u;
    worker_lock(worker);
    job = find_job_locked(worker, owner_addr, job_id);
    if (job == NULL) {
        worker_unlock(worker);
        return PWOS_JOB_STATUS_NOT_FOUND;
    }
    snapshot_job(job, out_snapshot);
    if (job->state == PWOS_JOB_STATE_CANCELLED) {
        status = PWOS_JOB_STATUS_CANCELLED;
    } else if (job->state != PWOS_JOB_STATE_DONE) {
        status = PWOS_JOB_STATUS_NOT_READY;
    } else if (job->result_len > out_cap) {
        status = PWOS_JOB_STATUS_INTERNAL;
    } else {
        memcpy(out_result, job->result, job->result_len);
        *out_len = job->result_len;
    }
    worker_unlock(worker);
    return status;
}

uint16_t pwos_compute_worker_cancel(
    pwos_compute_worker_t *worker,
    uint8_t owner_addr,
    uint32_t job_id,
    pwos_compute_job_snapshot_t *out_snapshot)
{
    pwos_compute_job_t *job;

    if (worker == NULL || job_id == 0u) {
        return PWOS_JOB_STATUS_BAD_REQUEST;
    }
    worker_lock(worker);
    job = find_job_locked(worker, owner_addr, job_id);
    if (job == NULL) {
        worker_unlock(worker);
        return PWOS_JOB_STATUS_NOT_FOUND;
    }
    if (job->state == PWOS_JOB_STATE_QUEUED ||
        job->state == PWOS_JOB_STATE_RUNNING) {
        if (job->state == PWOS_JOB_STATE_QUEUED && worker->stats.queued_jobs > 0u) {
            --worker->stats.queued_jobs;
        }
        if (job->state == PWOS_JOB_STATE_RUNNING && worker->stats.active_jobs > 0u) {
            --worker->stats.active_jobs;
        }
        job->state = PWOS_JOB_STATE_CANCELLED;
        (void)snprintf(job->log, sizeof(job->log), "cancelled");
        ++worker->stats.cancelled;
    }
    snapshot_job(job, out_snapshot);
    worker_unlock(worker);
    return PWOS_JOB_STATUS_OK;
}

static void step_hash(pwos_compute_job_t *job)
{
    job->accumulator ^= job->input[job->work_index];
    job->accumulator *= 16777619u;
    ++job->work_index;
    if (job->work_index == job->work_total) {
        pwos_job_put_le32(job->result, job->accumulator);
    }
}

static void step_vector_add(pwos_compute_job_t *job)
{
    uint16_t count = pwos_job_get_le16(job->input);
    uint32_t index = job->work_index;
    int32_t a = pwos_job_get_i16(job->input + 2u + index * 2u);
    int32_t b = pwos_job_get_i16(job->input + 2u + (uint32_t)count * 2u + index * 2u);

    pwos_job_put_i32(job->result + 2u + index * 4u, a + b);
    ++job->work_index;
}

static void step_matmul(pwos_compute_job_t *job)
{
    uint8_t inner = job->input[1];
    uint8_t cols = job->input[2];
    uint32_t row = job->work_index / cols;
    uint32_t col = job->work_index % cols;
    uint32_t a_offset = 4u;
    uint32_t b_offset = a_offset + 2u * (uint32_t)job->input[0] * inner;
    int32_t sum = 0;
    uint32_t k;

    for (k = 0u; k < inner; ++k) {
        int32_t a = pwos_job_get_i16(
            job->input + a_offset + 2u * (row * inner + k));
        int32_t b = pwos_job_get_i16(
            job->input + b_offset + 2u * (k * cols + col));
        sum += a * b;
    }
    pwos_job_put_i32(job->result + 4u + job->work_index * 4u, sum);
    ++job->work_index;
}

static uint8_t mandelbrot_pixel(const pwos_compute_job_t *job, uint32_t index)
{
    uint8_t width = job->input[0];
    uint16_t max_iter = pwos_job_get_le16(job->input + 2u);
    int32_t cx = pwos_job_get_i32(job->input + 4u) +
        (int32_t)(index % width) * pwos_job_get_i32(job->input + 12u);
    int32_t cy = pwos_job_get_i32(job->input + 8u) +
        (int32_t)(index / width) * pwos_job_get_i32(job->input + 16u);
    int32_t zx = 0;
    int32_t zy = 0;
    uint16_t iter = 0u;

    while (iter < max_iter) {
        int64_t zx2 = (int64_t)zx * zx;
        int64_t zy2 = (int64_t)zy * zy;
        int32_t next_x;
        int32_t next_y;

        if (zx2 + zy2 > ((int64_t)4 << 32)) {
            break;
        }
        next_x = (int32_t)((zx2 - zy2) >> 16) + cx;
        next_y = (int32_t)(((int64_t)2 * zx * zy) >> 16) + cy;
        zx = next_x;
        zy = next_y;
        ++iter;
    }
    return (uint8_t)iter;
}

static void step_mandelbrot(pwos_compute_job_t *job)
{
    job->result[4u + job->work_index] = mandelbrot_pixel(job, job->work_index);
    ++job->work_index;
}

static void step_raytrace_tile(pwos_compute_job_t *job)
{
    pwos_render_request_t request;
    uint16_t pixel;
    uint32_t offset;

    if (pwos_render_decode_request(job->input, job->input_len, &request) != 0) {
        job->state = PWOS_JOB_STATE_FAILED;
        return;
    }
    pixel = pwos_smallpt_render_pixel(&request, job->work_index);
    offset = PWOS_RENDER_RESULT_HEADER_LEN + job->work_index * 2u;
    job->result[offset] = (uint8_t)(pixel & 0xFFu);
    job->result[offset + 1u] = (uint8_t)(pixel >> 8);
    ++job->work_index;
}

int pwos_compute_worker_step(pwos_compute_worker_t *worker)
{
    pwos_compute_job_t *job = NULL;
    size_t i;

    if (worker == NULL) {
        return 0;
    }
    worker_lock(worker);
    for (i = 0u; i < PWOS_COMPUTE_MAX_JOBS; ++i) {
        if (worker->jobs[i].used != 0u &&
            worker->jobs[i].state == PWOS_JOB_STATE_RUNNING) {
            job = &worker->jobs[i];
            break;
        }
    }
    if (job == NULL) {
        for (i = 0u; i < PWOS_COMPUTE_MAX_JOBS; ++i) {
            if (worker->jobs[i].used != 0u &&
                worker->jobs[i].state == PWOS_JOB_STATE_QUEUED &&
                (job == NULL || worker->jobs[i].sequence < job->sequence)) {
                job = &worker->jobs[i];
            }
        }
        if (job != NULL) {
            job->state = PWOS_JOB_STATE_RUNNING;
            if (worker->stats.queued_jobs > 0u) {
                --worker->stats.queued_jobs;
            }
            ++worker->stats.active_jobs;
            ++worker->stats.started;
            (void)snprintf(job->log, sizeof(job->log), "running");
        }
    }
    if (job == NULL) {
        worker_unlock(worker);
        return 0;
    }

    switch (job->kernel) {
    case PWOS_JOB_KERNEL_HASH: step_hash(job); break;
    case PWOS_JOB_KERNEL_VECTOR_ADD: step_vector_add(job); break;
    case PWOS_JOB_KERNEL_MATMUL: step_matmul(job); break;
    case PWOS_JOB_KERNEL_MANDELBROT: step_mandelbrot(job); break;
    case PWOS_JOB_KERNEL_RAYTRACE_TILE: step_raytrace_tile(job); break;
    default:
        job->state = PWOS_JOB_STATE_FAILED;
        ++worker->stats.failed;
        break;
    }
    ++worker->stats.steps;
    if (job->state == PWOS_JOB_STATE_RUNNING) {
        job->progress_permille = (uint16_t)(
            (job->work_index * PWOS_JOB_PROGRESS_MAX) / job->work_total);
        if (job->work_index >= job->work_total) {
            job->state = PWOS_JOB_STATE_DONE;
            job->progress_permille = PWOS_JOB_PROGRESS_MAX;
            (void)snprintf(job->log, sizeof(job->log), "done");
            ++worker->stats.completed;
            if (worker->stats.active_jobs > 0u) {
                --worker->stats.active_jobs;
            }
        }
    }
    worker_unlock(worker);
    return 1;
}

void pwos_compute_worker_get_stats(
    pwos_compute_worker_t *worker,
    pwos_compute_worker_stats_t *out_stats)
{
    if (worker == NULL || out_stats == NULL) {
        return;
    }
    worker_lock(worker);
    *out_stats = worker->stats;
    worker_unlock(worker);
}
