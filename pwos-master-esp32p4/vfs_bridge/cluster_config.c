#include "cluster_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cluster_vfs.h"
#include "mini9p_client.h"
#include "uart_transport.h"

struct cluster_static_node {
    const char *name;             /* 用户可见的节点名，例如 "mcu1" */
    struct m9p_client *client;    /* 通往该节点的 Mini9P client */
    bool auto_attach;             /* 启动注册后是否立即尝试 attach */
};

static struct m9p_client g_mcu1_client;

int cluster_init_static_nodes(void)
{
    int ret;

    /* 第一版只注册一个静态节点。真实 UART/SPI transport 接好后，
     * 只需要替换 g_mcu1_client 的 transport 初始化，不需要改 cluster_vfs。
     */
    static const struct cluster_static_node nodes[] = {
        {
            .name = "mcu1",
            .client = &g_mcu1_client,
            .auto_attach = true,
        },
    };

    ret = m9p_uart_transport_init_default();
    if (ret < 0) {
        return ret;
    }

    m9p_client_init(&g_mcu1_client,
                    m9p_uart_transport_request,
                    m9p_uart_transport_default());

    for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); ++i) {
        ret = cluster_vfs_add_direct(nodes[i].name, nodes[i].client);
        if (ret < 0) {
            return ret;
        }

        if (nodes[i].auto_attach) {
            ret = cluster_vfs_attach(nodes[i].name);
            if (ret < 0) {
                /* 当前策略允许从机未接入时 Master 继续启动。 */
                printf("cluster_config: %s attach deferred, rc=%d\n", nodes[i].name, ret);
            }
        }
    }

    return 0;
}
