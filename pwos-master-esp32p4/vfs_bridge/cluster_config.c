#include "cluster_config.h"

#include "cluster_vfs.h"

#define CLUSTER_CONFIG_HOST_ADDR 0x00u
#define CLUSTER_CONFIG_DIRECT_LINK_METRIC 1u

static struct cluster g_mesh_cluster;
static bool g_mesh_cluster_initialized;

static int ensure_mesh_host_initialized(void)
{
    if (g_mesh_cluster_initialized) {
        return 0;
    }

    return cluster_config_init_mesh_host();
}

int cluster_config_init_mesh_host(void)
{
    struct cluster_config cfg;
    int rc;

    cluster_get_default_config(&cfg);
    cfg.local_addr = CLUSTER_CONFIG_HOST_ADDR;
    cfg.mode = CLUSTER_MODE_TOPOLOGY;

    rc = cluster_init(&g_mesh_cluster, &cfg);
    if (rc != 0) {
        return rc;
    }

    rc = cluster_vfs_init();
    if (rc != 0) {
        return rc;
    }

    rc = cluster_vfs_bind_mesh_cluster(&g_mesh_cluster);
    if (rc != 0) {
        return rc;
    }

    g_mesh_cluster_initialized = true;
    return 0;
}

struct cluster *cluster_config_mesh_cluster(void)
{
    if (!g_mesh_cluster_initialized) {
        return NULL;
    }

    return &g_mesh_cluster;
}

int cluster_config_on_node_discovered(
    uint8_t mesh_addr,
    const uint8_t hw_uid[MESH_UID_LEN],
    struct m9p_client *client,
    const char **out_name,
    bool *out_reused_mapping)
{
    int rc;

    if (hw_uid == NULL || client == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = ensure_mesh_host_initialized();
    if (rc != 0) {
        return rc;
    }

    /*
     * 当前主机实现把“新节点加入链路”视为 host 与该节点之间出现一条直连边。
     * 若未来 discovery 来自更复杂的多跳控制面，可以把这里替换成更精细的
     * 链路注入逻辑，而 VFS 接口无需变化。
     */
    rc = cluster_add_link(
        &g_mesh_cluster,
        g_mesh_cluster.config.local_addr,
        mesh_addr,
        CLUSTER_CONFIG_DIRECT_LINK_METRIC,
        true);
    if (rc != 0) {
        return rc;
    }

    rc = cluster_set_node_online(&g_mesh_cluster, mesh_addr, true);
    if (rc != 0) {
        return rc;
    }

    return cluster_vfs_discover_node(mesh_addr, hw_uid, client, out_name, out_reused_mapping);
}

int cluster_config_refresh_node_connectivity(uint8_t mesh_addr, bool *out_reachable)
{
    int rc;

    if (out_reachable == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = ensure_mesh_host_initialized();
    if (rc != 0) {
        return rc;
    }

    return cluster_vfs_refresh_node_from_cluster(mesh_addr, out_reachable);
}

int cluster_config_on_node_departed(uint8_t mesh_addr, bool *out_reachable)
{
    bool reachable = false;
    int rc;

    rc = ensure_mesh_host_initialized();
    if (rc != 0) {
        return rc;
    }

    rc = cluster_mark_node_offline(&g_mesh_cluster, mesh_addr);
    if (rc != 0) {
        return rc;
    }

    rc = cluster_config_refresh_node_connectivity(mesh_addr, &reachable);
    if (rc != 0) {
        return rc;
    }

    if (out_reachable != NULL) {
        *out_reachable = reachable;
    }

    return 0;
}

int cluster_init_static_nodes(void)
{
    return cluster_config_init_mesh_host();
}
