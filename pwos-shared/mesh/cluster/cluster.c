#include "cluster.h"

#include <string.h>

/*
 * cluster 实现说明：
 *
 * 1. DIRECT_TABLE 模式：
 *    - 直接维护 dst -> next_hop 的路由表。
 *    - 适合子机、简单中继、或已经由主机算好下一跳的场景。
 *
 * 2. TOPOLOGY 模式：
 *    - 维护链路拓扑。
 *    - 在需要时根据本机地址派生路由表。
 *    - 适合主机：主机可以先收集全图，再把路由结果下发到子机。
 *
 * 当前实现优先保证“可用、静态内存友好、便于 processor 直接调用”。
 */

#define CLUSTER_INVALID_ADDR 0xFFu
#define CLUSTER_INF_COST 0xFFu
#define CLUSTER_CONTROL_REPLY_HOP 8u

static void clear_nodes(struct cluster *cluster)
{
    size_t i;

    for (i = 0u; i < CLUSTER_MAX_NODES; ++i) {
        cluster->nodes[i].addr = CLUSTER_INVALID_ADDR;
        cluster->nodes[i].online = false;
        cluster->nodes[i].valid = false;
    }
}

static void clear_routes(struct cluster *cluster)
{
    size_t i;

    for (i = 0u; i < CLUSTER_MAX_ROUTES; ++i) {
        cluster->routes[i].dst = CLUSTER_INVALID_ADDR;
        cluster->routes[i].next_hop = CLUSTER_INVALID_ADDR;
        cluster->routes[i].metric = 0u;
        cluster->routes[i].local = false;
        cluster->routes[i].valid = false;
    }
}

static void clear_links(struct cluster *cluster)
{
    size_t i;

    for (i = 0u; i < CLUSTER_MAX_LINKS; ++i) {
        cluster->links[i].from = CLUSTER_INVALID_ADDR;
        cluster->links[i].to = CLUSTER_INVALID_ADDR;
        cluster->links[i].metric = 0u;
        cluster->links[i].bidirectional = false;
        cluster->links[i].valid = false;
    }
}

static struct cluster_node *find_node(struct cluster *cluster, uint8_t addr)
{
    size_t i;

    if (cluster == NULL) {
        return NULL;
    }

    for (i = 0u; i < CLUSTER_MAX_NODES; ++i) {
        if (cluster->nodes[i].valid && cluster->nodes[i].addr == addr) {
            return &cluster->nodes[i];
        }
    }
    return NULL;
}

static struct cluster_node *ensure_node(struct cluster *cluster, uint8_t addr)
{
    size_t i;
    struct cluster_node *node;

    node = find_node(cluster, addr);
    if (node != NULL) {
        return node;
    }

    for (i = 0u; i < CLUSTER_MAX_NODES; ++i) {
        if (!cluster->nodes[i].valid) {
            cluster->nodes[i].valid = true;
            cluster->nodes[i].addr = addr;
            cluster->nodes[i].online = false;
            return &cluster->nodes[i];
        }
    }

    return NULL;
}

static struct cluster_route *find_route(struct cluster *cluster, uint8_t dst)
{
    size_t i;

    if (cluster == NULL) {
        return NULL;
    }

    for (i = 0u; i < CLUSTER_MAX_ROUTES; ++i) {
        if (cluster->routes[i].valid && cluster->routes[i].dst == dst) {
            return &cluster->routes[i];
        }
    }
    return NULL;
}

static struct cluster_route *ensure_route(struct cluster *cluster, uint8_t dst)
{
    size_t i;
    struct cluster_route *route;

    route = find_route(cluster, dst);
    if (route != NULL) {
        return route;
    }

    for (i = 0u; i < CLUSTER_MAX_ROUTES; ++i) {
        if (!cluster->routes[i].valid) {
            cluster->routes[i].valid = true;
            cluster->routes[i].dst = dst;
            cluster->routes[i].next_hop = dst;
            cluster->routes[i].metric = 1u;
            cluster->routes[i].local = false;
            return &cluster->routes[i];
        }
    }

    return NULL;
}

static struct cluster_link *find_link(struct cluster *cluster, uint8_t from, uint8_t to, bool bidirectional)
{
    size_t i;

    if (cluster == NULL) {
        return NULL;
    }

    for (i = 0u; i < CLUSTER_MAX_LINKS; ++i) {
        if (!cluster->links[i].valid) {
            continue;
        }
        if (cluster->links[i].from == from && cluster->links[i].to == to &&
            cluster->links[i].bidirectional == bidirectional) {
            return &cluster->links[i];
        }
    }
    return NULL;
}

static struct cluster_link *ensure_link(struct cluster *cluster, uint8_t from, uint8_t to, uint8_t metric, bool bidirectional)
{
    size_t i;
    struct cluster_link *link;

    link = find_link(cluster, from, to, bidirectional);
    if (link != NULL) {
        link->metric = metric;
        return link;
    }

    for (i = 0u; i < CLUSTER_MAX_LINKS; ++i) {
        if (!cluster->links[i].valid) {
            cluster->links[i].valid = true;
            cluster->links[i].from = from;
            cluster->links[i].to = to;
            cluster->links[i].metric = (metric == 0u) ? 1u : metric;
            cluster->links[i].bidirectional = bidirectional;
            return &cluster->links[i];
        }
    }

    return NULL;
}

static int remove_route_by_dst(struct cluster *cluster, uint8_t dst)
{
    struct cluster_route *route;

    route = find_route(cluster, dst);
    if (route == NULL) {
        return -(int)MESH_ERR_NO_ROUTE;
    }

    memset(route, 0, sizeof(*route));
    route->dst = CLUSTER_INVALID_ADDR;
    return 0;
}

static int remove_link_by_key(struct cluster *cluster, uint8_t from, uint8_t to, bool bidirectional)
{
    struct cluster_link *link;

    link = find_link(cluster, from, to, bidirectional);
    if (link == NULL) {
        return -(int)MESH_ERR_NO_ROUTE;
    }

    memset(link, 0, sizeof(*link));
    link->from = CLUSTER_INVALID_ADDR;
    link->to = CLUSTER_INVALID_ADDR;
    return 0;
}

static int cluster_rebuild_routes_from_topology(struct cluster *cluster)
{
    uint8_t node_addrs[CLUSTER_MAX_NODES];
    uint8_t dist[CLUSTER_MAX_NODES];
    uint8_t first_hop[CLUSTER_MAX_NODES];
    bool visited[CLUSTER_MAX_NODES];
    size_t node_count = 0u;
    size_t i;
    size_t j;

    if (cluster == NULL || !cluster->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    if (cluster->config.mode != CLUSTER_MODE_TOPOLOGY) {
        cluster->routes_dirty = false;
        return 0;
    }

    clear_routes(cluster);

    /* 收集所有出现过的节点地址。 */
    if (ensure_node(cluster, cluster->config.local_addr) == NULL) {
        return -(int)MESH_ERR_BUSY;
    }
    for (i = 0u; i < CLUSTER_MAX_NODES; ++i) {
        if (cluster->nodes[i].valid) {
            node_addrs[node_count++] = cluster->nodes[i].addr;
        }
    }

    for (i = 0u; i < node_count; ++i) {
        dist[i] = CLUSTER_INF_COST;
        first_hop[i] = CLUSTER_INVALID_ADDR;
        visited[i] = false;
    }

    /* 本机到本机的距离为 0。 */
    for (i = 0u; i < node_count; ++i) {
        if (node_addrs[i] == cluster->config.local_addr) {
            dist[i] = 0u;
            first_hop[i] = cluster->config.local_addr;
            break;
        }
    }

    /*
     * 简化版 Dijkstra：
     * - 节点数量很小（<= 16），使用数组扫描就足够。
     * - metric 越小越优先。
     */
    for (i = 0u; i < node_count; ++i) {
        size_t u_index = node_count;
        uint8_t best_dist = CLUSTER_INF_COST;

        for (j = 0u; j < node_count; ++j) {
            if (!visited[j] && dist[j] < best_dist) {
                best_dist = dist[j];
                u_index = j;
            }
        }

        if (u_index == node_count) {
            break;
        }

        visited[u_index] = true;

        for (j = 0u; j < CLUSTER_MAX_LINKS; ++j) {
            struct cluster_link *link;
            size_t v_index = node_count;
            uint8_t u_addr;
            uint8_t v_addr;
            uint8_t cost;
            uint8_t candidate;

            if (!cluster->links[j].valid) {
                continue;
            }

            link = &cluster->links[j];
            u_addr = node_addrs[u_index];
            v_addr = CLUSTER_INVALID_ADDR;
            cost = (link->metric == 0u) ? 1u : link->metric;

            if (link->from == u_addr) {
                v_addr = link->to;
            } else if (link->bidirectional && link->to == u_addr) {
                v_addr = link->from;
            }

            if (v_addr == CLUSTER_INVALID_ADDR) {
                continue;
            }

            for (v_index = 0u; v_index < node_count; ++v_index) {
                if (node_addrs[v_index] == v_addr) {
                    break;
                }
            }
            if (v_index == node_count) {
                continue;
            }
            if (visited[v_index]) {
                continue;
            }

            candidate = (uint8_t)(dist[u_index] + cost);
            if (candidate < dist[v_index]) {
                dist[v_index] = candidate;
                if (u_addr == cluster->config.local_addr) {
                    first_hop[v_index] = v_addr;
                } else {
                    first_hop[v_index] = first_hop[u_index];
                }
            }
        }
    }

    /* 将最短路结果写回 routes 表。 */
    for (i = 0u; i < node_count; ++i) {
        struct cluster_route *route;

        if (node_addrs[i] == cluster->config.local_addr) {
            route = ensure_route(cluster, node_addrs[i]);
            if (route == NULL) {
                return -(int)MESH_ERR_BUSY;
            }
            route->next_hop = cluster->config.local_addr;
            route->metric = 0u;
            route->local = true;
            continue;
        }

        if (dist[i] == CLUSTER_INF_COST || first_hop[i] == CLUSTER_INVALID_ADDR) {
            continue;
        }

        route = ensure_route(cluster, node_addrs[i]);
        if (route == NULL) {
            return -(int)MESH_ERR_BUSY;
        }
        route->next_hop = first_hop[i];
        route->metric = dist[i];
        route->local = false;
    }

    cluster->routes_dirty = false;
    return 0;
}

/*
 * 获取默认配置：
 * - 地址设为未分配，避免误判“本机命中”。
 * - 模式设为 DIRECT_TABLE，便于先跑通最小链路。
 */
void cluster_get_default_config(struct cluster_config *out_config)
{
    if (out_config == NULL) {
        return;
    }

    out_config->local_addr = CLUSTER_INVALID_ADDR;
    out_config->mode = CLUSTER_MODE_DIRECT_TABLE;
}

/*
 * 初始化 cluster 对象。
 *
 * 该函数只初始化内存和模式，不主动创建任何路由项。
 */
int cluster_init(struct cluster *cluster, const struct cluster_config *config)
{
    struct cluster_config default_config;

    if (cluster == NULL) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    memset(cluster, 0, sizeof(*cluster));
    cluster_get_default_config(&default_config);
    if (config != NULL) {
        default_config = *config;
    }

    cluster->config = default_config;
    clear_nodes(cluster);
    clear_routes(cluster);
    clear_links(cluster);
    cluster->initialized = true;
    cluster->routes_dirty = (cluster->config.mode == CLUSTER_MODE_TOPOLOGY);
    return 0;
}

/* 清理 cluster 状态。 */
void cluster_deinit(struct cluster *cluster)
{
    if (cluster == NULL) {
        return;
    }

    memset(cluster, 0, sizeof(*cluster));
}

/*
 * 更新本机地址。
 *
 * 在 TOPOLOGY 模式下，本机地址变化会让历史路由失效，因此标记 dirty。
 */
int cluster_set_local_addr(struct cluster *cluster, uint8_t local_addr)
{
    if (cluster == NULL || !cluster->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    cluster->config.local_addr = local_addr;
    cluster->routes_dirty = (cluster->config.mode == CLUSTER_MODE_TOPOLOGY);
    return 0;
}

/* 切换模式后，必要时触发路由重算。 */
int cluster_set_mode(struct cluster *cluster, enum cluster_mode mode)
{
    if (cluster == NULL || !cluster->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    cluster->config.mode = mode;
    cluster->routes_dirty = (mode == CLUSTER_MODE_TOPOLOGY);
    return 0;
}

/* 更新节点在线状态。 */
int cluster_set_node_online(struct cluster *cluster, uint8_t addr, bool online)
{
    struct cluster_node *node;

    if (cluster == NULL || !cluster->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    node = ensure_node(cluster, addr);
    if (node == NULL) {
        return -(int)MESH_ERR_BUSY;
    }

    node->online = online;
    return 0;
}

/*
 * 插入或更新静态路由。
 *
 * 这是 DIRECT_TABLE 模式的核心写接口。
 */
int cluster_add_route(struct cluster *cluster, uint8_t dst, uint8_t next_hop, uint8_t metric)
{
    struct cluster_route *route;

    if (cluster == NULL || !cluster->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    route = ensure_route(cluster, dst);
    if (route == NULL) {
        return -(int)MESH_ERR_BUSY;
    }

    route->dst = dst;
    route->next_hop = next_hop;
    route->metric = (metric == 0u) ? 1u : metric;
    route->local = (dst == cluster->config.local_addr);
    route->valid = true;
    ensure_node(cluster, dst);
    ensure_node(cluster, next_hop);
    return 0;
}

/* 删除静态路由。 */
int cluster_remove_route(struct cluster *cluster, uint8_t dst)
{
    if (cluster == NULL || !cluster->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    return remove_route_by_dst(cluster, dst);
}

/*
 * 添加拓扑链路。
 *
 * 链路更新后，拓扑派生路由需要重新计算。
 */
int cluster_add_link(struct cluster *cluster, uint8_t from, uint8_t to, uint8_t metric, bool bidirectional)
{
    struct cluster_link *link;

    if (cluster == NULL || !cluster->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    link = ensure_link(cluster, from, to, metric, bidirectional);
    if (link == NULL) {
        return -(int)MESH_ERR_BUSY;
    }

    ensure_node(cluster, from);
    ensure_node(cluster, to);
    cluster->routes_dirty = true;
    return 0;
}

/* 删除拓扑链路。 */
int cluster_remove_link(struct cluster *cluster, uint8_t from, uint8_t to, bool bidirectional)
{
    if (cluster == NULL || !cluster->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    cluster->routes_dirty = true;
    return remove_link_by_key(cluster, from, to, bidirectional);
}

/* 对外暴露的重算接口。 */
int cluster_rebuild_routes(struct cluster *cluster)
{
    if (cluster == NULL || !cluster->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    return cluster_rebuild_routes_from_topology(cluster);
}

/*
 * 核心查询接口：给定 dst，返回 next_hop 和是否本机。
 *
 * mesh processor 会直接依赖这个函数的语义来决定“转发还是本地处理”。
 */
int cluster_lookup_next_hop(
    struct cluster *cluster,
    uint8_t dst,
    uint8_t *out_next_hop,
    bool *out_is_local)
{
    struct cluster_route *route;

    if (cluster == NULL || out_next_hop == NULL || out_is_local == NULL || !cluster->initialized) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    if (dst == cluster->config.local_addr) {
        *out_next_hop = cluster->config.local_addr;
        *out_is_local = true;
        return 0;
    }

    if (cluster->config.mode == CLUSTER_MODE_TOPOLOGY && cluster->routes_dirty) {
        int rc = cluster_rebuild_routes_from_topology(cluster);
        if (rc != 0) {
            return rc;
        }
    }

    route = find_route(cluster, dst);
    if (route == NULL || !route->valid) {
        return -(int)MESH_ERR_NO_ROUTE;
    }

    *out_next_hop = route->next_hop;
    *out_is_local = route->local;
    return 0;
}

/*
 * ROUTE_UPDATE 落地入口。
 *
 * 目前只处理 SET / DELETE 两种动作，保持最小闭环。
 */
int cluster_apply_route_update(struct cluster *cluster, const struct mesh_route_update_payload *payload)
{
    if (cluster == NULL || payload == NULL) {
        return -(int)MESH_ERR_BAD_FRAME;
    }
    if (!cluster->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    if (payload->action == MESH_ROUTE_SET) {
        return cluster_add_route(cluster, payload->dst, payload->next_hop, payload->metric);
    }
    if (payload->action == MESH_ROUTE_DELETE) {
        return cluster_remove_route(cluster, payload->dst);
    }

    return -(int)MESH_ERR_UNSUPPORTED_TYPE;
}

/*
 * ===== 与 processor 对齐的适配接口 =====
 */

int cluster_processor_route_lookup(
    void *cluster_ctx,
    uint8_t dst,
    uint8_t *out_next_hop,
    bool *out_is_local)
{
    return cluster_lookup_next_hop(
        (struct cluster *)cluster_ctx,
        dst,
        out_next_hop,
        out_is_local);
}

int cluster_processor_control_handler(
    void *cluster_ctx,
    const struct mesh_frame_view *frame,
    uint8_t *out_reply_frame,
    size_t reply_cap,
    size_t *out_reply_len)
{
    struct cluster *cluster = (struct cluster *)cluster_ctx;

    if (out_reply_len != NULL) {
        *out_reply_len = 0u;
    }
    if (cluster == NULL || frame == NULL || out_reply_len == NULL) {
        return -(int)MESH_ERR_BAD_FRAME;
    }
    if (!cluster->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    switch (frame->type) {
        case MESH_TYPE_REGISTER: {
            struct mesh_register_payload payload;

            if (!mesh_parse_register(frame, &payload)) {
                return -(int)MESH_ERR_BAD_FRAME;
            }
            return cluster_set_node_online(cluster, frame->src, true);
        }

        case MESH_TYPE_ASSIGN: {
            struct mesh_assign_payload payload;

            if (!mesh_parse_assign(frame, &payload)) {
                return -(int)MESH_ERR_BAD_FRAME;
            }

            /*
             * 当 ASSIGN 明确发给本机，或者本机仍处于未分配地址状态时，
             * 接受该地址更新。
             */
            if (frame->dst == cluster->config.local_addr ||
                cluster->config.local_addr == CLUSTER_INVALID_ADDR) {
                int rc = cluster_set_local_addr(cluster, payload.node_addr);
                if (rc != 0) {
                    return rc;
                }
            }

            return cluster_set_node_online(cluster, frame->src, true);
        }

        case MESH_TYPE_ROUTE_UPDATE: {
            struct mesh_route_update_payload payload;

            if (!mesh_parse_route_update(frame, &payload)) {
                return -(int)MESH_ERR_BAD_FRAME;
            }
            return cluster_apply_route_update(cluster, &payload);
        }

        case MESH_TYPE_LINK_STATE: {
            struct mesh_link_state_payload payload;

            if (!mesh_parse_link_state(frame, &payload)) {
                return -(int)MESH_ERR_BAD_FRAME;
            }

            if (payload.link_up != 0u) {
                int rc = cluster_set_node_online(cluster, frame->src, true);
                if (rc != 0) {
                    return rc;
                }
                return cluster_add_link(cluster, frame->src, payload.neighbor, payload.quality, false);
            }

            return cluster_remove_link(cluster, frame->src, payload.neighbor, false);
        }

        case MESH_TYPE_PING: {
            struct mesh_ping_payload payload;
            int rc;

            if (!mesh_parse_ping(frame, &payload)) {
                return -(int)MESH_ERR_BAD_FRAME;
            }

            rc = cluster_set_node_online(cluster, frame->src, true);
            if (rc != 0) {
                return rc;
            }

            /* 自动回 PONG，便于最小探活闭环。 */
            if (out_reply_frame == NULL) {
                return -(int)MESH_ERR_BAD_FRAME;
            }
            if (!mesh_build_ping(
                    cluster->config.local_addr,
                    frame->src,
                    frame->seq,
                    CLUSTER_CONTROL_REPLY_HOP,
                    MESH_TYPE_PONG,
                    &payload,
                    out_reply_frame,
                    reply_cap,
                    out_reply_len)) {
                return -(int)MESH_ERR_BAD_FRAME;
            }
            return 0;
        }

        case MESH_TYPE_PONG:
        case MESH_TYPE_TIME_SYNC:
            return cluster_set_node_online(cluster, frame->src, true);

        case MESH_TYPE_ERROR:
            /* 当前版本只保留错误帧，不对 cluster 状态做更改。 */
            return 0;

        default:
            return -(int)MESH_ERR_UNSUPPORTED_TYPE;
    }
}
