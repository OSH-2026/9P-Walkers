/* ========================================================================
 * Host RPC 服务端 —— 实现
 *
 * 接收远端请求帧，按 service.method 路由到对应 dispatch 函数，
 * 调用 runtime 层回调完成实际业务，编码响应帧返回。
 *
 * 支持的方法:
 *   cluster.read_node   → dispatch_read
 *   cluster.write_node  → dispatch_write
 *   host.advertise      → dispatch_advertise
 *   topology.whoowns    → dispatch_whoowns
 *   topology.sync       → dispatch_topology_sync
 * ======================================================================== */

#include "host_rpc_service.h"

#include <stdio.h>
#include <string.h>

#include "mini9p_protocol.h"
#include "session_manager.h"

/* ========================================================================
 * 第一节: 工具函数
 * ======================================================================== */

/**
 * 比较二进制字段与 C 字符串是否相等。
 * 因为 RPC 协议中 service/method 字段是 (ptr, len) 而非 null-terminated。
 */
static int field_equals(const char *field, uint8_t field_len, const char *expected)
{
    size_t expected_len = strlen(expected);
    return field_len == expected_len && memcmp(field, expected, expected_len) == 0;
}

/** 将本地错误码映射为 Host RPC 状态码 */
static uint16_t map_local_error(int rc)
{
    if (rc == 0) return PWOS_HOST_RPC_STATUS_OK;
    if (rc == PWOS_SESSION_ERR_DEADLINE)       return PWOS_HOST_RPC_STATUS_DEADLINE;
    if (rc == PWOS_SESSION_ERR_NO_ROUTE ||
        rc == PWOS_SESSION_ERR_STALE_BOOT)     return PWOS_HOST_RPC_STATUS_NO_ROUTE;
    if (rc == PWOS_SESSION_ERR_QUEUE_FULL ||
        rc == -(int)M9P_ERR_EBUSY ||
        rc == -(int)M9P_ERR_EAGAIN)            return PWOS_HOST_RPC_STATUS_BUSY;
    if (rc == -(int)M9P_ERR_ENOENT)            return PWOS_HOST_RPC_STATUS_NOT_FOUND;
    if (rc == -(int)M9P_ERR_EINVAL ||
        rc == -(int)M9P_ERR_EOFFS ||
        rc == -(int)M9P_ERR_EMSIZE)            return PWOS_HOST_RPC_STATUS_BAD_REQUEST;
    return PWOS_HOST_RPC_STATUS_INTERNAL;
}

/**
 * 将二进制字段 (ptr, len) 安全拷贝到 null-terminated 字符串缓冲区。
 * @return 0=成功, -1=字段过长
 */
static int copy_field(const char *source, uint8_t source_len,
                      char *out, size_t out_cap)
{
    if (!source || source_len == 0u || source_len >= out_cap)
        return -1;
    memcpy(out, source, source_len);
    out[source_len] = '\0';
    return 0;
}

/* ========================================================================
 * 第二节: 方法分发 (dispatch)
 * ======================================================================== */

/**
 * cluster.read_node: 读取远端节点上的虚拟文件。
 * 响应: blob 编码的文件内容。
 */
static uint16_t dispatch_read(pwos_host_rpc_service_t *service,
                              const pwos_host_rpc_frame_view_t *request,
                              uint16_t *out_payload_len)
{
    service->stats.read_calls++;

    /* 解码参数 */
    pwos_host_rpc_read_node_view_t args;
    char target[PWOS_HOST_RPC_TARGET_CAP];
    char path[PWOS_HOST_RPC_PATH_CAP];

    if (!service->config.read_node ||
        pwos_host_rpc_decode_read_node(request->payload, request->payload_len, &args) != 0 ||
        copy_field(args.target, args.target_len, target, sizeof(target)) != 0 ||
        copy_field(args.path, args.path_len, path, sizeof(path)) != 0) {
        return PWOS_HOST_RPC_STATUS_BAD_REQUEST;
    }

    /* 执行读取 */
    uint16_t data_len = args.max_bytes;
    if (data_len > sizeof(service->data)) data_len = sizeof(service->data);

    int rc = service->config.read_node(service->config.ctx,
                                       target, path,
                                       service->data, &data_len,
                                       request->deadline_ms);
    if (rc != 0) return map_local_error(rc);

    /* 编码 blob 响应 */
    if (pwos_host_rpc_encode_blob(service->data, data_len,
                                  service->method_payload,
                                  sizeof(service->method_payload),
                                  out_payload_len) != 0) {
        return PWOS_HOST_RPC_STATUS_INTERNAL;
    }
    return PWOS_HOST_RPC_STATUS_OK;
}

/**
 * cluster.write_node: 写入远端节点上的虚拟文件。
 * 响应: blob 编码的写入字节数 (2 字节大端)。
 */
static uint16_t dispatch_write(pwos_host_rpc_service_t *service,
                               const pwos_host_rpc_frame_view_t *request,
                               uint16_t *out_payload_len)
{
    service->stats.write_calls++;

    pwos_host_rpc_write_node_view_t args;
    char target[PWOS_HOST_RPC_TARGET_CAP];
    char path[PWOS_HOST_RPC_PATH_CAP];

    if (!service->config.write_node ||
        pwos_host_rpc_decode_write_node(request->payload, request->payload_len, &args) != 0 ||
        copy_field(args.target, args.target_len, target, sizeof(target)) != 0 ||
        copy_field(args.path, args.path_len, path, sizeof(path)) != 0) {
        return PWOS_HOST_RPC_STATUS_BAD_REQUEST;
    }

    uint16_t written = 0u;
    int rc = service->config.write_node(service->config.ctx,
                                        target, path,
                                        args.data, args.data_len, &written,
                                        request->deadline_ms);
    if (rc != 0) return map_local_error(rc);

    /* 编码写入字节数 */
    uint8_t written_bytes[2];
    written_bytes[0] = (uint8_t)(written >> 8);
    written_bytes[1] = (uint8_t)written;

    if (pwos_host_rpc_encode_blob(written_bytes, sizeof(written_bytes),
                                  service->method_payload,
                                  sizeof(service->method_payload),
                                  out_payload_len) != 0) {
        return PWOS_HOST_RPC_STATUS_INTERNAL;
    }
    return PWOS_HOST_RPC_STATUS_OK;
}

/**
 * host.advertise: 接收远端主机的公告 → 回复本地公告。
 */
static uint16_t dispatch_advertise(pwos_host_rpc_service_t *service,
                                   const pwos_host_rpc_frame_view_t *request,
                                   uint16_t *out_payload_len)
{
    service->stats.advertise_calls++;

    pwos_host_rpc_advertise_t advertise, local;

    if (!service->config.advertise || !service->config.local_advertise ||
        pwos_host_rpc_decode_advertise(request->payload, request->payload_len,
                                       &advertise) != 0) {
        return PWOS_HOST_RPC_STATUS_BAD_REQUEST;
    }

    int rc = service->config.advertise(service->config.ctx, &advertise);
    if (rc != 0) return map_local_error(rc);

    memset(&local, 0, sizeof(local));
    rc = service->config.local_advertise(service->config.ctx, &local);
    if (rc != 0) return map_local_error(rc);

    if (pwos_host_rpc_encode_advertise(&local,
                                       service->method_payload,
                                       sizeof(service->method_payload),
                                       out_payload_len) != 0) {
        return PWOS_HOST_RPC_STATUS_INTERNAL;
    }
    return PWOS_HOST_RPC_STATUS_OK;
}

/**
 * topology.whoowns: 查询全局名称所属主机的公告信息。
 */
static uint16_t dispatch_whoowns(pwos_host_rpc_service_t *service,
                                 const pwos_host_rpc_frame_view_t *request,
                                 uint16_t *out_payload_len)
{
    service->stats.whoowns_calls++;

    const char *target_view;
    char target[PWOS_HOST_RPC_TARGET_CAP];
    uint8_t target_len;

    if (!service->config.whoowns ||
        pwos_host_rpc_decode_text(request->payload, request->payload_len,
                                  &target_view, &target_len) != 0 ||
        copy_field(target_view, target_len, target, sizeof(target)) != 0) {
        return PWOS_HOST_RPC_STATUS_BAD_REQUEST;
    }

    pwos_host_rpc_advertise_t owner;
    memset(&owner, 0, sizeof(owner));

    int rc = service->config.whoowns(service->config.ctx, target, &owner);
    if (rc != 0) return map_local_error(rc);

    if (pwos_host_rpc_encode_advertise(&owner,
                                       service->method_payload,
                                       sizeof(service->method_payload),
                                       out_payload_len) != 0) {
        return PWOS_HOST_RPC_STATUS_INTERNAL;
    }
    return PWOS_HOST_RPC_STATUS_OK;
}

/**
 * topology.sync: 接收远端拓扑 → 合并 → 返回本地拓扑。
 */
static uint16_t dispatch_topology_sync(pwos_host_rpc_service_t *service,
                                       const pwos_host_rpc_frame_view_t *request,
                                       uint16_t *out_payload_len)
{
    service->stats.topology_sync_calls++;

    if (!service->config.topology_sync ||
        pwos_host_rpc_decode_topology(request->payload, request->payload_len,
                                      &service->incoming_topology) != 0) {
        return PWOS_HOST_RPC_STATUS_BAD_REQUEST;
    }

    memset(&service->outgoing_topology, 0, sizeof(service->outgoing_topology));
    int rc = service->config.topology_sync(service->config.ctx,
                                           &service->incoming_topology,
                                           &service->outgoing_topology);
    if (rc != 0) return map_local_error(rc);

    if (pwos_host_rpc_encode_topology(&service->outgoing_topology,
                                      service->method_payload,
                                      sizeof(service->method_payload),
                                      out_payload_len) != 0) {
        return PWOS_HOST_RPC_STATUS_INTERNAL;
    }
    return PWOS_HOST_RPC_STATUS_OK;
}

/* ========================================================================
 * 第三节: 服务初始化 & 请求处理入口
 * ======================================================================== */

int pwos_host_rpc_service_init(pwos_host_rpc_service_t *service,
                               const pwos_host_rpc_service_config_t *config)
{
    if (!service || !config) return -1;
    memset(service, 0, sizeof(*service));
    service->config = *config;
    return 0;
}

/**
 * 处理一个 RPC 请求帧。
 *
 * 流程:
 *   1. 解码请求帧 → 提取 service / method (二进制字段)
 *   2. field_equals 匹配路由表 → 调用对应 dispatch 函数
 *   3. 编码响应帧 (KIND_RESPONSE, 原始 call_id, 状态码, 方法 payload)
 */
int pwos_host_rpc_service_handle(pwos_host_rpc_service_t *service,
                                 const uint8_t *request_frame, size_t request_len,
                                 uint8_t *response_frame, size_t response_cap,
                                 size_t *out_response_len)
{
    /* ---- 解码请求帧 ---- */
    pwos_host_rpc_frame_view_t request;
    if (!service || !response_frame || !out_response_len ||
        pwos_host_rpc_decode(request_frame, request_len, &request) != 0 ||
        request.kind != PWOS_HOST_RPC_KIND_REQUEST) {
        if (service) service->stats.bad_frames++;
        return -1;
    }

    service->stats.requests++;
    service->stats.last_call_id = request.call_id;

    /* ---- 路由表 (service.method 匹配) ---- */
    const char *resp_service = "host";
    const char *resp_method  = "error";
    uint16_t    payload_len  = 0u;
    uint16_t    status       = PWOS_HOST_RPC_STATUS_NOT_FOUND;

    if (field_equals(request.service, request.service_len, "cluster") &&
        field_equals(request.method, request.method_len, "read_node")) {
        resp_service = "cluster"; resp_method = "read_node";
        status = dispatch_read(service, &request, &payload_len);

    } else if (field_equals(request.service, request.service_len, "cluster") &&
               field_equals(request.method, request.method_len, "write_node")) {
        resp_service = "cluster"; resp_method = "write_node";
        status = dispatch_write(service, &request, &payload_len);

    } else if (field_equals(request.service, request.service_len, "host") &&
               field_equals(request.method, request.method_len, "advertise")) {
        resp_service = "host"; resp_method = "advertise";
        status = dispatch_advertise(service, &request, &payload_len);

    } else if (field_equals(request.service, request.service_len, "topology") &&
               field_equals(request.method, request.method_len, "whoowns")) {
        resp_service = "topology"; resp_method = "whoowns";
        status = dispatch_whoowns(service, &request, &payload_len);

    } else if (field_equals(request.service, request.service_len, "topology") &&
               field_equals(request.method, request.method_len, "sync")) {
        resp_service = "topology"; resp_method = "sync";
        status = dispatch_topology_sync(service, &request, &payload_len);

    } else {
        service->stats.not_found++;
    }

    /* ---- 统计 & 编码响应 ---- */
    if (status != PWOS_HOST_RPC_STATUS_OK) {
        payload_len = 0u;
        service->stats.remote_errors++;
    }

    if (pwos_host_rpc_encode(PWOS_HOST_RPC_KIND_RESPONSE,
                             request.call_id, 0u, status,
                             resp_service, resp_method,
                             payload_len ? service->method_payload : NULL,
                             payload_len,
                             response_frame, response_cap,
                             out_response_len) != 0) {
        return -1;
    }

    service->stats.responses++;
    service->stats.last_status = status;
    return 0;
}

/* ========================================================================
 * 第四节: 统计查询
 * ======================================================================== */

void pwos_host_rpc_service_get_stats(const pwos_host_rpc_service_t *service,
                                     pwos_host_rpc_service_stats_t *out_stats)
{
    if (service && out_stats) *out_stats = service->stats;
}
