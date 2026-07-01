#include "service_runtime.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "compute_worker.h"
#include "fault_control.h"
#include "frame_pool.h"
#include "local_vfs.h"
#include "job_service.h"
#include "mini9p_protocol.h"
#include "mini9p_server.h"
#include "node_control.h"
#include "port_manager.h"
#include "pwos_queues.h"
#include "pwos_link_frame.h"
#include "pwos_tasks.h"
#include "rpc_service.h"
#include "render_display.h"
#include "stm32f4xx_hal.h"
#include "uart_dma_port.h"

typedef struct {
    uint8_t initialized;
    struct local_vfs vfs;
    struct m9p_server server;
    pwos_rpc_service_t rpc;
    pwos_compute_worker_t compute;
    pwos_job_service_t jobs;
    StaticSemaphore_t compute_mutex_cb;
    SemaphoreHandle_t compute_mutex;
    uint8_t response_payload[PWOS_LINK_MAX_PAYLOAD_LEN];
    pwos_service_runtime_stats_t stats;
} pwos_service_runtime_t;

static pwos_service_runtime_t g_service;

typedef struct {
    uint32_t request_offset;
    uint32_t stream_offset;
    uint8_t *data;
    uint16_t cap;
    uint16_t count;
} diag_writer_t;

static void diag_write(diag_writer_t *writer, const char *text, size_t len)
{
    size_t start = 0u;
    size_t available;

    if (writer == NULL || text == NULL || len == 0u) {
        return;
    }
    if (writer->stream_offset + len <= writer->request_offset) {
        writer->stream_offset += (uint32_t)len;
        return;
    }
    if (writer->request_offset > writer->stream_offset) {
        start = (size_t)(writer->request_offset - writer->stream_offset);
    }
    available = len - start;
    if (available > (size_t)(writer->cap - writer->count)) {
        available = (size_t)(writer->cap - writer->count);
    }
    if (available > 0u) {
        memcpy(writer->data + writer->count, text + start, available);
        writer->count = (uint16_t)(writer->count + available);
    }
    writer->stream_offset += (uint32_t)len;
}

static void diag_printf(diag_writer_t *writer, const char *format, ...)
{
    char line[192];
    va_list args;
    int written;
    size_t len;

    if (writer == NULL || writer->count == writer->cap) {
        return;
    }
    va_start(args, format);
    written = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (written <= 0) {
        return;
    }
    len = (size_t)written;
    if (len >= sizeof(line)) {
        len = sizeof(line) - 1u;
    }
    diag_write(writer, line, len);
}

static void render_health(diag_writer_t *writer)
{
    pwos_node_control_snapshot_t node;

    pwos_node_control_get_snapshot(&node);
    /* 首行保持 ok，兼容 coordinator 的稳定性探测日志。 */
    diag_printf(writer, "ok\n");
    diag_printf(writer,
        "uptime_ms=%lu free_heap=%lu frame_pool=%u/%u alloc_fail=%lu\n",
        (unsigned long)HAL_GetTick(),
        (unsigned long)xPortGetFreeHeapSize(),
        (unsigned)pwos_frame_pool_free_count(),
        (unsigned)pwos_frame_pool_capacity(),
        (unsigned long)pwos_frame_pool_alloc_fail_count());
    diag_printf(writer,
        "role=node state=%s addr=%u upstream=%u boot=%lu uid=%08lx-%08lx-%08lx lease_epoch=%lu lease_ms=%lu\n",
        pwos_node_state_name(node.state),
        node.local_addr,
        node.upstream_port,
        (unsigned long)node.local_boot_id,
        (unsigned long)node.local_uid[0],
        (unsigned long)node.local_uid[1],
        (unsigned long)node.local_uid[2],
        (unsigned long)node.lease_epoch,
        (unsigned long)node.lease_ms);
    diag_printf(writer,
        "authority valid=%u port=%u ttl=%u uid=%08lx-%08lx-%08lx epoch=%lu priority=%u advertise_rx=%lu forward=%lu duplicate=%lu rejected_ctrl=%lu\n",
        node.authority_valid,
        node.authority_port,
        node.authority_ttl,
        (unsigned long)node.authority_uid[0],
        (unsigned long)node.authority_uid[1],
        (unsigned long)node.authority_uid[2],
        (unsigned long)node.authority_epoch,
        node.authority_priority,
        (unsigned long)node.host_advertise_rx,
        (unsigned long)node.host_advertise_forward_tx,
        (unsigned long)node.host_advertise_duplicate_rx,
        (unsigned long)node.nonleader_ctrl_drop);
}

static void render_tasks(diag_writer_t *writer)
{
    pwos_task_status_t tasks[PWOS_TASK_COUNT];
    size_t count;
    size_t i;

    count = pwos_tasks_get_status(tasks, PWOS_TASK_COUNT);
    for (i = 0u; i < count; ++i) {
        diag_printf(writer,
            "name=%s priority=%lu stack_free_words=%lu heartbeat=%lu last_tick=%lu\n",
            tasks[i].name,
            (unsigned long)tasks[i].priority,
            (unsigned long)tasks[i].stack_high_water_words,
            (unsigned long)tasks[i].heartbeat,
            (unsigned long)tasks[i].last_tick);
    }
}

static void render_ports(diag_writer_t *writer)
{
    pwos_port_snapshot_t ports[PWOS_UART_DMA_MAX_PORTS];
    size_t count;
    size_t i;

    count = pwos_port_manager_get_snapshot(ports, PWOS_UART_DMA_MAX_PORTS);
    for (i = 0u; i < count; ++i) {
        pwos_uart_dma_port_stats_t uart;

        memset(&uart, 0, sizeof(uart));
        (void)pwos_uart_dma_get_stats(i, &uart);
        diag_printf(writer,
            "id=%u name=%s state=%s peer_role=%u peer_port=%u caps=0x%02x last_rx=%lu last_tx=%lu hello=%lu/%lu ack=%lu/%lu bad=%lu\n",
            ports[i].id,
            ports[i].name,
            pwos_port_state_name(ports[i].state),
            ports[i].peer_role,
            ports[i].peer_port_id,
            ports[i].peer_caps,
            (unsigned long)ports[i].last_rx_tick,
            (unsigned long)ports[i].last_tx_tick,
            (unsigned long)ports[i].hello_tx,
            (unsigned long)ports[i].hello_rx,
            (unsigned long)ports[i].hello_ack_tx,
            (unsigned long)ports[i].hello_ack_rx,
            (unsigned long)ports[i].bad_link_frames);
        diag_printf(writer,
            "id=%u dma=%u rx_bytes=%lu rx_frames=%lu parse=%lu rx_drop=%lu tx_bytes=%lu tx_frames=%lu tx_err=%lu uart_err=%lu rearm_fail=%lu restarts=%lu\n",
            uart.id,
            uart.dma_running,
            (unsigned long)uart.rx_bytes,
            (unsigned long)uart.rx_frames,
            (unsigned long)uart.rx_parse_errors,
            (unsigned long)(uart.rx_drop_no_block + uart.rx_drop_queue_full),
            (unsigned long)uart.tx_bytes,
            (unsigned long)uart.tx_frames,
            (unsigned long)uart.tx_errors,
            (unsigned long)uart.uart_errors,
            (unsigned long)uart.rx_rearm_failures,
            (unsigned long)uart.dma_restarts);
    }
}

static void render_links(diag_writer_t *writer)
{
    pwos_port_snapshot_t ports[PWOS_UART_DMA_MAX_PORTS];
    size_t count;
    size_t i;

    count = pwos_port_manager_get_snapshot(ports, PWOS_UART_DMA_MAX_PORTS);
    for (i = 0u; i < count; ++i) {
        uint8_t bidirectional =
            ports[i].hello_rx > 0u && ports[i].hello_ack_rx > 0u ? 1u : 0u;

        diag_printf(writer,
            "port=%u state=%s bidirectional=%u metric=%u last_seen=%lu peer_boot=%lu\n",
            ports[i].id,
            pwos_port_state_name(ports[i].state),
            bidirectional,
            bidirectional != 0u ? 1u : 0xFFFFu,
            (unsigned long)ports[i].last_rx_tick,
            (unsigned long)ports[i].peer_boot_id);
    }
}

static void render_neighbors(diag_writer_t *writer)
{
    pwos_port_snapshot_t ports[PWOS_UART_DMA_MAX_PORTS];
    size_t count;
    size_t i;
    size_t produced = 0u;

    count = pwos_port_manager_get_snapshot(ports, PWOS_UART_DMA_MAX_PORTS);
    for (i = 0u; i < count; ++i) {
        if (ports[i].peer_role == PWOS_PORT_PEER_UNKNOWN) {
            continue;
        }
        diag_printf(writer,
            "port=%u role=%u uid=%08lx-%08lx-%08lx boot=%lu caps=0x%02x\n",
            ports[i].id,
            ports[i].peer_role,
            (unsigned long)ports[i].peer_uid[0],
            (unsigned long)ports[i].peer_uid[1],
            (unsigned long)ports[i].peer_uid[2],
            (unsigned long)ports[i].peer_boot_id,
            ports[i].peer_caps);
        ++produced;
    }
    if (produced == 0u) {
        diag_printf(writer, "(empty)\n");
    }
}

static void render_routes(diag_writer_t *writer)
{
    pwos_node_route_snapshot_t routes[PWOS_NODE_CONTROL_MAX_ROUTES];
    pwos_node_control_snapshot_t node;
    size_t count;
    size_t i;

    pwos_node_control_get_snapshot(&node);
    diag_printf(writer, "host next=host port=%u metric=1 owner=host\n", node.upstream_port);
    count = pwos_node_control_get_routes(routes, PWOS_NODE_CONTROL_MAX_ROUTES);
    for (i = 0u; i < count; ++i) {
        diag_printf(writer,
            "dst=%u next=%u port=%u metric=%u version=%lu owner=host\n",
            routes[i].dst,
            routes[i].next_hop,
            routes[i].port_id,
            routes[i].metric,
            (unsigned long)routes[i].route_version);
    }
}

static void render_sessions(pwos_service_runtime_t *runtime, diag_writer_t *writer)
{
    pwos_job_service_stats_t jobs;
    pwos_compute_worker_stats_t compute;
    pwos_rpc_service_stats_t rpc;
    size_t i;
    size_t active = 0u;

    for (i = 0u; i < runtime->server.max_fids; ++i) {
        if (runtime->server.fids[i].in_use) {
            ++active;
        }
    }
    diag_printf(writer,
        "attached=%u msize=%u max_fids=%u active_fids=%u rx=%lu tx=%lu errors=%lu tx_fail=%lu last_peer=%u last_tag=%u last_type=0x%02x\n",
        runtime->server.attached ? 1u : 0u,
        runtime->server.negotiated_msize,
        runtime->server.max_fids,
        (unsigned)active,
        (unsigned long)runtime->stats.mini9p_rx,
        (unsigned long)runtime->stats.mini9p_tx,
        (unsigned long)runtime->stats.server_errors,
        (unsigned long)runtime->stats.tx_failures,
        runtime->stats.last_src,
        runtime->stats.last_m9p_tag,
        runtime->stats.last_m9p_type);
    pwos_rpc_service_get_stats(&runtime->rpc, &rpc);
    diag_printf(writer,
        "rpc request=%lu response=%lu oneway=%lu stream=%lu/%lu/%lu cancel=%lu deadline=%lu completed=%lu pending_peak=%lu bad=%lu not_found=%lu busy=%lu send_fail=%lu last_call=%u last_status=%s(%u)\n",
        (unsigned long)rpc.request_rx,
        (unsigned long)rpc.response_tx,
        (unsigned long)rpc.oneway_rx,
        (unsigned long)rpc.stream_request_rx,
        (unsigned long)rpc.stream_chunk_tx,
        (unsigned long)rpc.stream_end_tx,
        (unsigned long)rpc.cancel_rx,
        (unsigned long)rpc.deadline_tx,
        (unsigned long)rpc.completed,
        (unsigned long)rpc.pending_peak,
        (unsigned long)rpc.bad_frames,
        (unsigned long)rpc.not_found,
        (unsigned long)rpc.busy,
        (unsigned long)rpc.send_failures,
        rpc.last_call_id,
        pwos_rpc_status_name(rpc.last_status),
        rpc.last_status);
    pwos_job_service_get_stats(&runtime->jobs, &jobs);
    pwos_compute_worker_get_stats(&runtime->compute, &compute);
    diag_printf(writer,
        "job request=%lu response=%lu caps=%lu submit=%lu status=%lu result=%lu cancel=%lu bad=%lu send_fail=%lu active=%u queued=%u done=%lu failed=%lu cancelled=%lu steps=%lu last_job=%lu last_status=%s(%u)\n",
        (unsigned long)jobs.request_rx,
        (unsigned long)jobs.response_tx,
        (unsigned long)jobs.caps_rx,
        (unsigned long)jobs.submit_rx,
        (unsigned long)jobs.status_rx,
        (unsigned long)jobs.result_rx,
        (unsigned long)jobs.cancel_rx,
        (unsigned long)jobs.bad_frames,
        (unsigned long)jobs.send_failures,
        compute.active_jobs,
        compute.queued_jobs,
        (unsigned long)compute.completed,
        (unsigned long)compute.failed,
        (unsigned long)compute.cancelled,
        (unsigned long)compute.steps,
        (unsigned long)jobs.last_job_id,
        pwos_job_status_name(jobs.last_status),
        jobs.last_status);
    for (i = 0u; i < runtime->server.max_fids; ++i) {
        const struct m9p_server_fid *fid = &runtime->server.fids[i];

        if (!fid->in_use) {
            continue;
        }
        diag_printf(writer,
            "fid=%u open=%u mode=0x%02x iounit=%u path=%.96s\n",
            fid->fid,
            fid->open ? 1u : 0u,
            fid->mode,
            fid->iounit,
            fid->path);
    }
}

static void render_compute_caps(
    pwos_service_runtime_t *runtime,
    diag_writer_t *writer)
{
    pwos_compute_worker_stats_t stats;

    pwos_compute_worker_get_stats(&runtime->compute, &stats);
    diag_printf(writer,
        "cpu=stm32f429 slots=%u input=%u result=%u active=%u queued=%u "
        "kernels=hash,vector_add,matmul,mandelbrot,raytrace_tile\n",
        PWOS_COMPUTE_MAX_JOBS,
        PWOS_COMPUTE_INPUT_CAP,
        PWOS_COMPUTE_RESULT_CAP,
        stats.active_jobs,
        stats.queued_jobs);
}

static void render_compute_load(
    pwos_service_runtime_t *runtime,
    diag_writer_t *writer)
{
    pwos_compute_worker_stats_t stats;

    pwos_compute_worker_get_stats(&runtime->compute, &stats);
    diag_printf(writer,
        "active=%u queued=%u slots=%u submitted=%lu started=%lu completed=%lu "
        "failed=%lu cancelled=%lu rejected=%lu steps=%lu reused=%lu\n",
        stats.active_jobs,
        stats.queued_jobs,
        PWOS_COMPUTE_MAX_JOBS,
        (unsigned long)stats.submitted,
        (unsigned long)stats.started,
        (unsigned long)stats.completed,
        (unsigned long)stats.failed,
        (unsigned long)stats.cancelled,
        (unsigned long)stats.rejected,
        (unsigned long)stats.steps,
        (unsigned long)stats.slots_reused);
}

static void render_compute_jobs(
    pwos_service_runtime_t *runtime,
    diag_writer_t *writer)
{
    size_t i;
    size_t count = 0u;

    for (i = 0u; i < PWOS_COMPUTE_MAX_JOBS; ++i) {
        pwos_compute_job_snapshot_t job;

        if (pwos_compute_worker_get_slot_snapshot(
                &runtime->compute, i, &job) != 0) {
            continue;
        }
        diag_printf(writer,
            "id=%lu owner=%u kernel=%s state=%s progress=%u.%u%% result=%u log=%s\n",
            (unsigned long)job.job_id,
            job.owner_addr,
            pwos_job_kernel_name(job.kernel),
            pwos_job_state_name(job.state),
            job.progress_permille / 10u,
            job.progress_permille % 10u,
            job.result_len,
            job.log);
        ++count;
    }
    if (count == 0u) {
        diag_printf(writer, "(empty)\n");
    }
}

static void render_queues(diag_writer_t *writer)
{
    diag_printf(writer, "name=link_rx depth=%lu capacity=%u drops=%lu\n",
        (unsigned long)pwos_link_rx_depth(), PWOS_LINK_RX_QUEUE_LEN,
        (unsigned long)pwos_link_rx_drop_count());
    diag_printf(writer, "name=mesh_rx depth=%lu capacity=%u drops=%lu\n",
        (unsigned long)pwos_mesh_rx_depth(), PWOS_MESH_RX_QUEUE_LEN,
        (unsigned long)pwos_mesh_rx_drop_count());
    diag_printf(writer, "name=service_rx depth=%lu capacity=%u drops=%lu\n",
        (unsigned long)pwos_service_rx_depth(), PWOS_SERVICE_RX_QUEUE_LEN,
        (unsigned long)pwos_service_rx_drop_count());
    diag_printf(writer, "name=ctrl_tx depth=%lu capacity=%u drops=%lu\n",
        (unsigned long)pwos_ctrl_tx_depth(), PWOS_CTRL_TX_QUEUE_LEN,
        (unsigned long)pwos_ctrl_tx_drop_count());
    diag_printf(writer, "name=link_tx depth=%lu capacity=%u drops=%lu\n",
        (unsigned long)pwos_link_tx_depth(), PWOS_LINK_TX_QUEUE_LEN,
        (unsigned long)pwos_link_tx_drop_count());
}

static void render_log(pwos_service_runtime_t *runtime, diag_writer_t *writer)
{
    pwos_node_control_snapshot_t node;
    pwos_rpc_service_stats_t rpc;
    pwos_compute_worker_stats_t compute;

    pwos_node_control_get_snapshot(&node);
    diag_printf(writer,
        "control register_tx=%lu assign_rx=%lu renew_tx=%lu lease_ack_rx=%lu link_state_tx=%lu route_rx=%lu host_adv=%lu host_adv_forward=%lu host_adv_duplicate=%lu rejected_ctrl=%lu forward_fail=%lu no_route=%lu bad=%lu\n",
        (unsigned long)node.register_tx,
        (unsigned long)node.assign_rx,
        (unsigned long)node.renew_tx,
        (unsigned long)node.lease_ack_rx,
        (unsigned long)node.link_state_tx,
        (unsigned long)node.route_update_rx,
        (unsigned long)node.host_advertise_rx,
        (unsigned long)node.host_advertise_forward_tx,
        (unsigned long)node.host_advertise_duplicate_rx,
        (unsigned long)node.nonleader_ctrl_drop,
        (unsigned long)node.forward_fail,
        (unsigned long)node.drop_no_route,
        (unsigned long)node.bad_ctrl_frames);
    diag_printf(writer,
        "service m9p_rx=%lu m9p_tx=%lu rpc_rx=%lu rpc_tx=%lu rpc_error=%lu bad=%lu unsupported=%lu server_error=%lu tx_fail=%lu\n",
        (unsigned long)runtime->stats.mini9p_rx,
        (unsigned long)runtime->stats.mini9p_tx,
        (unsigned long)runtime->stats.rpc_rx,
        (unsigned long)runtime->stats.rpc_tx,
        (unsigned long)runtime->stats.rpc_errors,
        (unsigned long)runtime->stats.bad_frames,
        (unsigned long)runtime->stats.unsupported_frames,
        (unsigned long)runtime->stats.server_errors,
        (unsigned long)runtime->stats.tx_failures);
    pwos_rpc_service_get_stats(&runtime->rpc, &rpc);
    diag_printf(writer,
        "rpc request=%lu response=%lu oneway=%lu stream=%lu/%lu/%lu cancel=%lu deadline=%lu completed=%lu bad=%lu not_found=%lu busy=%lu send_fail=%lu\n",
        (unsigned long)rpc.request_rx,
        (unsigned long)rpc.response_tx,
        (unsigned long)rpc.oneway_rx,
        (unsigned long)rpc.stream_request_rx,
        (unsigned long)rpc.stream_chunk_tx,
        (unsigned long)rpc.stream_end_tx,
        (unsigned long)rpc.cancel_rx,
        (unsigned long)rpc.deadline_tx,
        (unsigned long)rpc.completed,
        (unsigned long)rpc.bad_frames,
        (unsigned long)rpc.not_found,
        (unsigned long)rpc.busy,
        (unsigned long)rpc.send_failures);
    pwos_compute_worker_get_stats(&runtime->compute, &compute);
    diag_printf(writer,
        "compute submitted=%lu started=%lu completed=%lu failed=%lu cancelled=%lu rejected=%lu steps=%lu reused=%lu active=%u queued=%u\n",
        (unsigned long)compute.submitted,
        (unsigned long)compute.started,
        (unsigned long)compute.completed,
        (unsigned long)compute.failed,
        (unsigned long)compute.cancelled,
        (unsigned long)compute.rejected,
        (unsigned long)compute.steps,
        (unsigned long)compute.slots_reused,
        compute.active_jobs,
        compute.queued_jobs);
}

static void render_build(diag_writer_t *writer)
{
#if defined(STM32F407xx)
    const char *target = "STM32F407";
#elif defined(STM32F429xx)
    const char *target = "STM32F429";
#else
    const char *target = "STM32F4";
#endif
#ifdef PWOS_ENABLE_FAULT_INJECTION
    const char *profile = "debug";
#else
    const char *profile = "release";
#endif
    diag_printf(writer,
        "target=%s profile=%s built=%s %s protocol=mini9p,rpc-v1,job-v1 mesh=mesh2\n",
        target, profile, __DATE__, __TIME__);
}

static void render_fault(diag_writer_t *writer)
{
    pwos_fault_port_snapshot_t faults[PWOS_FAULT_MAX_PORTS];
    size_t count;
    size_t i;

    diag_printf(writer, "reboot_pending=%u\n",
        pwos_fault_control_reboot_pending());
    count = pwos_fault_control_get_snapshot(faults, PWOS_FAULT_MAX_PORTS);
    for (i = 0u; i < count; ++i) {
        diag_printf(writer,
            "enabled=%u port=%u down=%u drop=%u corrupt=%u delay_ms=%lu seen=%lu tx_drop=%lu rx_drop=%lu corrupted=%lu delayed=%lu\n",
            faults[i].enabled,
            faults[i].port_id,
            faults[i].forced_down,
            faults[i].drop_percent,
            faults[i].corrupt_percent,
            (unsigned long)faults[i].delay_ms,
            (unsigned long)faults[i].frames_seen,
            (unsigned long)faults[i].dropped,
            (unsigned long)faults[i].rx_dropped,
            (unsigned long)faults[i].corrupted,
            (unsigned long)faults[i].delayed);
    }
}

static int service_vfs_read(
    void *ctx,
    const char *path,
    uint32_t offset,
    uint8_t *out_data,
    uint16_t out_cap,
    uint16_t *out_count)
{
    pwos_service_runtime_t *runtime = (pwos_service_runtime_t *)ctx;
    diag_writer_t writer;

    if (runtime == NULL || path == NULL || out_count == NULL ||
        (out_cap > 0u && out_data == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }
    memset(&writer, 0, sizeof(writer));
    writer.request_offset = offset;
    writer.data = out_data;
    writer.cap = out_cap;

    if (strcmp(path, "/sys/health") == 0) {
        render_health(&writer);
    } else if (strcmp(path, "/sys/tasks") == 0) {
        render_tasks(&writer);
    } else if (strcmp(path, "/sys/ports") == 0 ||
               strcmp(path, "/sys/uart") == 0) {
        render_ports(&writer);
    } else if (strcmp(path, "/sys/links") == 0) {
        render_links(&writer);
    } else if (strcmp(path, "/sys/neighbors") == 0) {
        render_neighbors(&writer);
    } else if (strcmp(path, "/sys/routes") == 0) {
        render_routes(&writer);
    } else if (strcmp(path, "/sys/sessions") == 0) {
        render_sessions(runtime, &writer);
    } else if (strcmp(path, "/sys/queues") == 0) {
        render_queues(&writer);
    } else if (strcmp(path, "/sys/log") == 0) {
        render_log(runtime, &writer);
    } else if (strcmp(path, "/sys/build") == 0 ||
               strcmp(path, "/sys/info") == 0) {
        render_build(&writer);
    } else if (strcmp(path, "/sys/fault") == 0) {
        render_fault(&writer);
    } else if (strcmp(path, "/compute/caps") == 0) {
        render_compute_caps(runtime, &writer);
    } else if (strcmp(path, "/compute/load") == 0) {
        render_compute_load(runtime, &writer);
    } else if (strcmp(path, "/compute/jobs") == 0) {
        render_compute_jobs(runtime, &writer);
    } else if (strcmp(path, "/display/status") == 0) {
        pwos_render_display_status_t status;

        pwos_render_display_get_status(&status);
        diag_printf(&writer,
            "ready=%u frame=%u dirty=%u tiles=%lu presents=%lu rejected=%lu canvas=%ux%u\n",
            status.initialized,
            status.frame_id,
            status.dirty,
            (unsigned long)status.tiles_received,
            (unsigned long)status.frames_presented,
            (unsigned long)status.rejected_tiles,
            PWOS_RENDER_CANVAS_WIDTH,
            PWOS_RENDER_CANVAS_HEIGHT);
    } else {
        return -(int)M9P_ERR_ENOENT;
    }

    *out_count = writer.count;
    return 0;
}

static int service_vfs_write(
    void *ctx,
    const char *path,
    uint32_t offset,
    const uint8_t *data,
    uint16_t count,
    uint16_t *out_count)
{
    int rc;

    (void)ctx;
    if (path == NULL || out_count == NULL || offset != 0u) {
        return -(int)M9P_ERR_EINVAL;
    }
    if (strcmp(path, "/display/tile") == 0) {
        rc = pwos_render_display_apply_tile(data, count);
        if (rc == -2) return -(int)M9P_ERR_EAGAIN;
        if (rc != 0) return -(int)M9P_ERR_EINVAL;
        *out_count = count;
        return 0;
    }
    if (strcmp(path, "/sys/fault") != 0) return -(int)M9P_ERR_EINVAL;
    rc = pwos_fault_control_command(data, count);
    if (rc == -2) {
        return -(int)M9P_ERR_ENOTSUP;
    }
    if (rc != 0) {
        return -(int)M9P_ERR_EINVAL;
    }
    *out_count = count;
    return 0;
}

static int decode_link_view(
    const pwos_frame_block_t *block,
    pwos_link_frame_view_t *out_view)
{
    if (block == NULL || out_view == NULL || block->len == 0u) {
        return -1;
    }

    return pwos_link_decode(block->data, block->len, out_view) == PWOS_OK ? 0 : -1;
}

static int rpc_send(
    void *ctx,
    uint8_t dst_addr,
    const uint8_t *payload,
    uint16_t payload_len)
{
    pwos_service_runtime_t *runtime = (pwos_service_runtime_t *)ctx;
    int rc;

    rc = pwos_node_control_send_data(
        (uint8_t)PWOS_LINK_TYPE_DATA_RPC,
        dst_addr,
        payload,
        payload_len);
    if (rc != 0) {
        ++runtime->stats.tx_failures;
        return rc;
    }
    ++runtime->stats.rpc_tx;
    return 0;
}

static int job_send(
    void *ctx,
    uint8_t dst_addr,
    const uint8_t *payload,
    uint16_t payload_len)
{
    pwos_service_runtime_t *runtime = (pwos_service_runtime_t *)ctx;
    int rc = pwos_node_control_send_data(
        (uint8_t)PWOS_LINK_TYPE_DATA_JOB, dst_addr, payload, payload_len);

    if (rc != 0) {
        ++runtime->stats.tx_failures;
        return rc;
    }
    ++runtime->stats.job_tx;
    return 0;
}

static void compute_lock(void *ctx)
{
    pwos_service_runtime_t *runtime = (pwos_service_runtime_t *)ctx;

    (void)xSemaphoreTake(runtime->compute_mutex, portMAX_DELAY);
}

static void compute_unlock(void *ctx)
{
    pwos_service_runtime_t *runtime = (pwos_service_runtime_t *)ctx;

    (void)xSemaphoreGive(runtime->compute_mutex);
}

static uint32_t rpc_now_ms(void *ctx)
{
    (void)ctx;
    return HAL_GetTick();
}

int pwos_service_runtime_init(void)
{
    struct local_vfs_config vfs_config;
    struct m9p_server_config server_config;
    pwos_compute_worker_config_t compute_config;

    memset(&g_service, 0, sizeof(g_service));
    g_service.compute_mutex = xSemaphoreCreateMutexStatic(
        &g_service.compute_mutex_cb);
    if (g_service.compute_mutex == NULL) {
        return -1;
    }
    memset(&compute_config, 0, sizeof(compute_config));
    compute_config.lock_ctx = &g_service;
    compute_config.lock = compute_lock;
    compute_config.unlock = compute_unlock;
    if (pwos_compute_worker_init(&g_service.compute, &compute_config) != 0 ||
        pwos_job_service_init(
            &g_service.jobs, &g_service, job_send, &g_service.compute) != 0) {
        return -1;
    }

    local_vfs_get_default_config(&vfs_config);
    vfs_config.io_ctx = &g_service;
    vfs_config.read = service_vfs_read;
    vfs_config.write = service_vfs_write;
    if (local_vfs_init(&g_service.vfs, &vfs_config) != 0) {
        return -1;
    }

    m9p_server_get_default_config(&server_config);
    server_config.ops = local_vfs_ops();
    server_config.ops_ctx = &g_service.vfs;
    server_config.max_msize = PWOS_LINK_MAX_PAYLOAD_LEN;
    server_config.default_iounit = LOCAL_VFS_DEFAULT_IOUNIT;
    if (m9p_server_init(&g_service.server, &server_config) != 0) {
        return -1;
    }
    if (pwos_rpc_service_init(
            &g_service.rpc, &g_service, rpc_send, rpc_now_ms) != 0 ||
        pwos_rpc_service_register_builtins(&g_service.rpc) != 0) {
        return -1;
    }

    g_service.initialized = 1u;
    g_service.stats.initialized = 1u;
    return 0;
}

int pwos_service_runtime_accepts(const pwos_frame_block_t *block)
{
    pwos_link_frame_view_t view;
    pwos_node_control_snapshot_t node;

    if (g_service.initialized == 0u || decode_link_view(block, &view) != 0) {
        return 0;
    }
    if (view.type != (uint8_t)PWOS_LINK_TYPE_DATA_MINI9P &&
        view.type != (uint8_t)PWOS_LINK_TYPE_DATA_RPC &&
        view.type != (uint8_t)PWOS_LINK_TYPE_DATA_JOB) {
        return 0;
    }

    pwos_node_control_get_snapshot(&node);
    return node.state == PWOS_NODE_ASSIGNED && view.dst == node.local_addr ? 1 : 0;
}

void pwos_service_runtime_process(pwos_frame_block_t *block)
{
    pwos_link_frame_view_t link_view;
    struct m9p_frame_view m9p_view;
    size_t response_len = 0u;
    int rc;

    if (g_service.initialized == 0u || decode_link_view(block, &link_view) != 0) {
        ++g_service.stats.bad_frames;
        return;
    }

    g_service.stats.last_src = link_view.src;
    g_service.stats.last_dst = link_view.dst;
    if (link_view.type == (uint8_t)PWOS_LINK_TYPE_DATA_JOB) {
        ++g_service.stats.job_rx;
        if (pwos_job_service_process(
                &g_service.jobs,
                link_view.src,
                link_view.payload,
                link_view.payload_len) != 0) {
            ++g_service.stats.job_errors;
        }
        return;
    }
    if (link_view.type == (uint8_t)PWOS_LINK_TYPE_DATA_RPC) {
        pwos_rpc_frame_view_t rpc_view;

        ++g_service.stats.rpc_rx;
        if (pwos_rpc_decode(
                link_view.payload, link_view.payload_len, &rpc_view) == 0) {
            g_service.stats.last_rpc_call_id = rpc_view.call_id;
            g_service.stats.last_rpc_status = rpc_view.status;
        }
        if (pwos_rpc_service_process(
                &g_service.rpc,
                link_view.src,
                link_view.payload,
                link_view.payload_len) != 0) {
            ++g_service.stats.rpc_errors;
        }
        return;
    }

    if (link_view.type != (uint8_t)PWOS_LINK_TYPE_DATA_MINI9P) {
        ++g_service.stats.unsupported_frames;
        return;
    }

    ++g_service.stats.mini9p_rx;
    if (m9p_decode_frame(link_view.payload, link_view.payload_len, &m9p_view)) {
        g_service.stats.last_m9p_type = m9p_view.type;
        g_service.stats.last_m9p_tag = m9p_view.tag;
    }

    /*
     * mini9P server 串行运行在 service_task 内。TWRITE 的 data 指针只在本调用
     * 期间有效，响应写入独立缓冲，避免覆盖请求 payload。
     */
    rc = m9p_server_handle_frame(
        &g_service.server,
        link_view.payload,
        link_view.payload_len,
        g_service.response_payload,
        sizeof(g_service.response_payload),
        &response_len);
    if (rc != 0) {
        ++g_service.stats.server_errors;
    }
    if (response_len == 0u || response_len > PWOS_LINK_MAX_PAYLOAD_LEN) {
        ++g_service.stats.server_errors;
        return;
    }

    if (pwos_node_control_send_data(
            (uint8_t)PWOS_LINK_TYPE_DATA_MINI9P,
            link_view.src,
            g_service.response_payload,
            (uint16_t)response_len) != 0) {
        ++g_service.stats.tx_failures;
        return;
    }

    ++g_service.stats.mini9p_tx;
}

void pwos_service_runtime_poll(void)
{
    if (g_service.initialized != 0u) {
        pwos_rpc_service_poll(&g_service.rpc);
    }
}

int pwos_service_runtime_compute_step(void)
{
    return g_service.initialized == 0u ? 0 :
        pwos_compute_worker_step(&g_service.compute);
}

void pwos_service_runtime_get_stats(pwos_service_runtime_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return;
    }
    *out_stats = g_service.stats;
}
