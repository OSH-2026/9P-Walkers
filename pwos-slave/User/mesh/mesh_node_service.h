/**
 * @file mesh_node_service.h
 * @brief STM32 slave 侧 Mesh 节点服务装配层。
 *
 * 本模块只负责把 raw mesh UART transport 与 mesh_node_runtime 串成一个
 * 可轮询的从机 mesh service。具体本地 backend 由板级代码初始化，并通过
 * Mini9P server handler/context 注入到 mesh 数据面。
 */

#ifndef MESH_NODE_SERVICE_H
#define MESH_NODE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mesh_node_runtime.h"
#include "mesh_uart_transport.h"
#include "mini9p_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_NODE_SERVICE_FRAME_CAP M9P_SERVER_DEFAULT_MSIZE
#define MESH_NODE_SERVICE_MAX_PORTS 4u
#define MESH_NODE_SERVICE_MAX_BOOTSTRAP_PENDING 8u
#define MESH_NODE_SERVICE_NEIGHBOR_ANY 0xffu
#define MESH_NODE_SERVICE_REGISTER_CAPABILITY_BITS 0x0001u

struct mesh_node_service_port_config {
    bool enabled;
    struct mesh_uart_transport_config uart_config;
};

struct mesh_node_service_config {
    mesh_processer_mini9p_server_handler_fn mini9p_server_handler;
    void *mini9p_server_ctx;
    size_t port_count;
    struct mesh_node_service_port_config ports[MESH_NODE_SERVICE_MAX_PORTS];
};

void mesh_node_service_get_default_config(struct mesh_node_service_config *out_config);

int mesh_node_service_init(const struct mesh_node_service_config *config);

void mesh_node_service_deinit(void);

int mesh_node_service_notify_link_up(void);

int mesh_node_service_poll_once(void);

struct mesh_node_runtime *mesh_node_service_runtime(void);

#ifdef __cplusplus
}
#endif

#endif
