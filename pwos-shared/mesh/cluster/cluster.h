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

/* 未知或未指定的本地链路/串口编号。 */
#define CLUSTER_PORT_INVALID 0xFFu

/* 共享约定：最高位端口选择器保留给 Wi-Fi 传输。 */
#define CLUSTER_PORT_WIFI_ID MESH_PORT_SELECTOR_WIFI_ID
#define CLUSTER_PORT_WIFI_MASK MESH_PORT_SELECTOR_WIFI_MASK

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
 * 当前最小元数据包括：
 * - online：控制面观测到的在线状态；
 * - capability_bits：来自最新 REGISTER 的能力位图；
 * - port_bitmap：来自最新 REGISTER 的本地发送选择器位图；
 * - wifi_supported：该节点是否声明支持 Wi-Fi 直连传输。
 */
struct cluster_node {
    uint8_t addr;
    uint16_t capability_bits;
    uint8_t port_bitmap;
    bool online;
    bool wifi_supported;
    bool valid;
};

/*
 * 一条路由项：dst -> selector。
 *
 * 默认情况下，next_hop 表示“下一跳 mesh 地址”；
 * 当 cluster_config.direct_routes_use_port_selectors = true 且工作在
 * DIRECT_TABLE 模式时，next_hop 改为“本地出口串口/端口编号”。
 *
 * 这样一套 cluster API 就能同时覆盖：
 * - 主机/普通路由场景：dst -> 下一跳地址；
 * - 单 runtime 管多 UART 的子机场景：dst -> 本地 UART 端口号。
 *
 * local 字段为 true 时表示 dst 就是本机，不需要外发。
 */
struct cluster_route {
    uint8_t dst;
    uint8_t next_hop;
    uint8_t metric;
    bool local;
    bool selector_is_port;
    bool valid;
};

/*
 * 一条拓扑链路：from <-> to（或单向）。
 *
 * 主机模式下，cluster 可以先维护这些链路，再根据 local_addr
 * 通过最短路/最小代价路径推导出路由表。
 *
 * from_port / to_port 表示：
 * - 若从 from 走向 to，应从 from 的哪个本地串口/端口发出去；
 * - 若链路反向使用，应从 to 的哪个本地串口/端口发向 from。
 *
 * 主机维护全图时，需要这些端口元数据来为子机生成
 * “目的地址 -> 出口串口编号”的 ROUTE_UPDATE。
 */
struct cluster_link {
    uint8_t from;
    uint8_t to;
    uint8_t metric;
    uint8_t from_port;
    uint8_t to_port;
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
    bool direct_routes_use_port_selectors;
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

/* 查询某个节点当前是否被标记为在线。 */
int cluster_get_node_online(struct cluster *cluster, uint8_t addr, bool *out_online);

/*
 * 添加一条静态路由。
 *
 * 适用于：
 * - 子机直接维护 next_hop 表。
 * - 主机先临时塞入静态路由作为兜底。
 *
 * next_hop 语义：
 * - 默认：下一跳 mesh 地址；
 * - 当 direct_routes_use_port_selectors=true 且 cluster 工作在 DIRECT_TABLE：
 *   本地出口串口/端口编号。
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
 * - 未指定端口信息时，from_port / to_port 均记为 CLUSTER_PORT_INVALID。
 */
int cluster_add_link(struct cluster *cluster, uint8_t from, uint8_t to, uint8_t metric, bool bidirectional);

/*
 * 添加带端口元数据的拓扑链路。
 *
 * 适用于主机维护全图时记录：某个节点从哪个本地串口出发能到达某个邻居。
 */
int cluster_add_link_with_ports(
    struct cluster *cluster,
    uint8_t from,
    uint8_t to,
    uint8_t metric,
    bool bidirectional,
    uint8_t from_port,
    uint8_t to_port);

/* 删除一条拓扑链路。 */
int cluster_remove_link(struct cluster *cluster, uint8_t from, uint8_t to, bool bidirectional);

/*
 * 将某个节点标记为离线，并清理与它相关的链路和依赖它的路由。
 *
 * 适用场景：
 * - 主机明确确认某个节点已经退出 mesh。
 * - 需要把该节点从当前连通图中摘掉，再重新评估其可达性。
 */
int cluster_mark_node_offline(struct cluster *cluster, uint8_t addr);

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
 * - out_is_local = false 表示该 dst 需要转发，并由 out_next_hop 给出发送选择器。
 *
 * 发送选择器语义：
 * - 普通/主机场景：返回下一跳 mesh 地址；
 * - 子机多串口 DIRECT_TABLE 场景：返回本地出口端口编号。
 */
int cluster_lookup_next_hop(
    struct cluster *cluster,
    uint8_t dst,
    uint8_t *out_next_hop,
    bool *out_is_local);

/*
 * 查询某个目标地址当前是否仍然可达。
 *
 * 该接口是 cluster_lookup_next_hop() 的布尔封装，便于上层在节点离线、
 * 链路断开等场景中只做“还通不通”的判断，而不关心具体 next_hop。
 */
int cluster_can_reach(struct cluster *cluster, uint8_t dst, bool *out_reachable);

/*
 * 从“远端节点 source 的视角”查询到 dst 的出口端口。
 *
 * 该接口只对 TOPOLOGY 模式有意义：主机维护的是全图，它需要知道某个子机
 * source 为了到达 dst，应当从哪个本地串口把帧发给下一跳。
 */
int cluster_lookup_remote_egress_port(
    struct cluster *cluster,
    uint8_t source,
    uint8_t dst,
    uint8_t *out_egress_port,
    bool *out_is_local);

/*
 * 为“远端节点 source 的子机路由表”构造一条 ROUTE_UPDATE 负载。
 *
 * - action = MESH_ROUTE_SET 时，next_hop 字段写入 source 的出口端口号；
 * - action = MESH_ROUTE_DELETE 时，直接构造删除负载，不依赖当前拓扑。
 */
int cluster_build_remote_route_update(
    struct cluster *cluster,
    uint8_t source,
    uint8_t dst,
    uint16_t route_version,
    uint8_t action,
    struct mesh_route_update_payload *out_payload);

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
