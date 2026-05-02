#include "cluster_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cluster_vfs.h"
#include "mini9p_client.h"
#include "mini9p_protocol.h"

struct cluster_static_node {
    const char *name;             /* 用户可见的节点名，例如 "mcu1" */
    struct m9p_client *client;    /* 通往该节点的 Mini9P client */
    bool auto_attach;             /* 启动注册后是否立即尝试 attach */
};

static struct m9p_client g_mcu1_client;

/* 静态节点注册阶段使用的占位 transport。
 *
 * 它只用于让 m9p_client 完成初始化和路由注册，不模拟真实从机响应，
 * 因此 attach/read/write 等实际通信都会返回 ENOTSUP。等 UART/SPI
 * transport 接好后，应在这里替换为真实的收发回调。
 */
static int cluster_stub_transport(void *transport_ctx,
                                  const uint8_t *tx_data,
                                  size_t tx_len,
                                  uint8_t *rx_data,
                                  size_t rx_cap,
                                  size_t *rx_len)
{
    (void)transport_ctx;
    (void)tx_data;
    (void)tx_len;
    (void)rx_data;
    (void)rx_cap;

    if (rx_len) {
        *rx_len = 0u;
    }
    return -(int)M9P_ERR_ENOTSUP;
}

int cluster_init_static_nodes(void)
{
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

    m9p_client_init(&g_mcu1_client, cluster_stub_transport, NULL);

    for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); ++i) {
        int ret = cluster_vfs_add_direct(nodes[i].name, nodes[i].client);
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
