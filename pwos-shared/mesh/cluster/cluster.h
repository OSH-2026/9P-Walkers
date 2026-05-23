#ifndef MESH_CLUSTER_H
#define MESH_CLUSTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../envelope/mesh_protocal.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cluster：mesh 控制面中的“节点与路由管理层”。
 *
 * 设计取向：
 * - 子机侧：主要保存简化路由表，回答“某个 mesh 地址该走哪个 next_hop”。
 * - 主机侧：可以维护更完整的拓扑信息，并在本模块内派生出路由表。
 *
 * 也就是说，cluster 不直接做文件协议处理，它只给 mesh processor 提供：
 * - 这个地址是不是本机？
 * - 如果不是本机，应该转发给哪个 next_hop？
 * - 收到 ROUTE_UPDATE / 拓扑变化时，如何刷新本地状态？
 */

/*
 * 默认容量：
 * - 这些上限针对 MCU 场景，保持静态内存和线性扫描可接受。
 * - 如果未来节点规模扩大，可以按同样接口调整上限。
 */
#define CLUSTER_MAX_NODES 16u
#define CLUSTER_MAX_ROUTES 16u
#define CLUSTER_MAX_LINKS 32u

/*
 * cluster 的工作模式：
 * - DIRECT_TABLE：更适合子机或简单中继，只维护路由表。
 * - TOPOLOGY：更适合主机，维护链路拓扑并派生路由表。
 */
enum cluster_mode {
    CLUSTER_MODE_DIRECT_TABLE = 0,
    CLUSTER_MODE_TOPOLOGY = 1
};

/*
 * 一个节点的最小运行时信息。
 *
 * 这里先只保存地址和在线状态；
 * 若后续控制面需要节点名、UID、能力位图，可以在不破坏接口的前提下扩展。
 */
struct cluster_node {
    uint8_t addr;
    bool online;
    bool valid;
};

/*
 * 一条路由项：dst -> next_hop。
 *
 * local 字段为 true 时表示 dst 就是本机，不需要外发。
 * 这类项可看作“命中本机地址”的快路径。
 */
struct cluster_route {
    uint8_t dst;
    uint8_t next_hop;
    uint8_t metric;
    bool local;
    bool valid;
};

/*
 * 一条拓扑链路：from <-> to（或单向）。
 *
 * 主机模式下，cluster 可以先维护这些链路，再根据 local_addr
 * 通过最短路/最小代价路径推导出路由表。
 */
struct cluster_link {
    uint8_t from;
    uint8_t to;
    uint8_t metric;
    bool bidirectional;
    bool valid;
};

/*
 * cluster 初始化配置。
 *
 * - local_addr：本机 mesh 地址。
 * - mode：决定是“直接路由表”还是“拓扑派生路由表”。
 */
struct cluster_config {
    uint8_t local_addr;
    enum cluster_mode mode;
};

/*
 * cluster 运行时对象。
 *
 * 该对象适合静态全局实例或外层组件成员，不需要动态分配。
 * 路由表和拓扑表都采用固定容量数组，便于嵌入式环境使用。
 */
struct cluster {
    struct cluster_config config;
    bool initialized;
    bool routes_dirty;
    struct cluster_node nodes[CLUSTER_MAX_NODES];
    struct cluster_route routes[CLUSTER_MAX_ROUTES];
    struct cluster_link links[CLUSTER_MAX_LINKS];
};

/* 返回默认配置：未分配地址 + 直接路由模式。 */
void cluster_get_default_config(struct cluster_config *out_config);

/* 初始化 cluster。 */
int cluster_init(struct cluster *cluster, const struct cluster_config *config);

/* 清理 cluster 状态。 */
void cluster_deinit(struct cluster *cluster);

/* 设置本机地址。 */
int cluster_set_local_addr(struct cluster *cluster, uint8_t local_addr);

/* 设置工作模式。 */
int cluster_set_mode(struct cluster *cluster, enum cluster_mode mode);

/*
 * 标记某个节点在线/离线。
 *
 * 该接口不改变路由本身，只改变节点状态，供未来控制面诊断使用。
 */
int cluster_set_node_online(struct cluster *cluster, uint8_t addr, bool online);

/*
 * 添加一条静态路由。
 *
 * 适用于：
 * - 子机直接维护 next_hop 表。
 * - 主机先临时塞入静态路由作为兜底。
 */
int cluster_add_route(struct cluster *cluster, uint8_t dst, uint8_t next_hop, uint8_t metric);

/* 删除一条静态路由。 */
int cluster_remove_route(struct cluster *cluster, uint8_t dst);

/*
 * 添加一条拓扑链路。
 *
 * 说明：
 * - bidirectional = true 时同时视为双向链路。
 * - metric 为路径代价，数值越小优先级越高。
 * - 添加/删除链路后，路由表会被标记为脏，需要重新派生。
 */
int cluster_add_link(struct cluster *cluster, uint8_t from, uint8_t to, uint8_t metric, bool bidirectional);

/* 删除一条拓扑链路。 */
int cluster_remove_link(struct cluster *cluster, uint8_t from, uint8_t to, bool bidirectional);

/*
 * 根据当前拓扑重新派生路由表。
 *
 * 仅在 TOPOLOGY 模式下有意义；
 * DIRECT_TABLE 模式下调用该接口会直接返回成功但不做任何事。
 */
int cluster_rebuild_routes(struct cluster *cluster);

/*
 * 询问某个 mesh 地址的下一跳。
 *
 * 返回值约定：
 * - 0：成功。
 * - 负值：失败，通常是无路由、参数非法或 cluster 未初始化。
 *
 * 输出约定：
 * - out_is_local = true  表示该 dst 就是本机。
 * - out_is_local = false 表示该 dst 需要转发，并由 out_next_hop 给出下一跳。
 */
int cluster_lookup_next_hop(
    struct cluster *cluster,
    uint8_t dst,
    uint8_t *out_next_hop,
    bool *out_is_local);

/*
 * 处理 ROUTE_UPDATE 控制消息。
 *
 * 这个接口用于让 control_handler 直接把控制帧负载交给 cluster。
 * 现在支持的最小语义：
 * - action = MESH_ROUTE_SET：更新/插入一条静态路由。
 * - action = MESH_ROUTE_DELETE：删除一条静态路由。
 */
int cluster_apply_route_update(struct cluster *cluster, const struct mesh_route_update_payload *payload);

/*
 * 与 mesh processor 对齐的适配接口。
 *
 * 这两个函数可以直接挂到 processor 配置中，无需额外封装：
 * - route_lookup  -> cluster_processor_route_lookup
 * - control_handler -> cluster_processor_control_handler
 */

/*
 * processor 路由查询适配器。
 *
 * 该函数签名与 mesh_processer_route_lookup_fn 完全一致。
 */
int cluster_processor_route_lookup(
    void *cluster_ctx,
    uint8_t dst,
    uint8_t *out_next_hop,
    bool *out_is_local);

/*
 * processor 控制面处理适配器。
 *
 * 该函数签名与 mesh_processer_control_handler_fn 完全一致。
 * 当前默认行为：
 * - REGISTER: 标记源节点在线。
 * - ASSIGN: 若命中本机，更新本机地址。
 * - ROUTE_UPDATE: 直接调用 cluster_apply_route_update。
 * - LINK_STATE: 根据 link_up 更新拓扑边。
 * - PING: 自动回 PONG（可关闭或替换）。
 *
 * 若不需要回包，会返回 *out_reply_len = 0。
 */
int cluster_processor_control_handler(
    void *cluster_ctx,
    const struct mesh_frame_view *frame,
    uint8_t *out_reply_frame,
    size_t reply_cap,
    size_t *out_reply_len);

#ifdef __cplusplus
}
#endif

#endif
