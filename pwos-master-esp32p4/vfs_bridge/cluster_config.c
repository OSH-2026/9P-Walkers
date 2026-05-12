#include "cluster_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cluster_vfs.h"
#include "mini9p_client.h"
#include "mini9p_peer_link.h"
#include "uart_transport.h"

struct cluster_static_node {
    const char *name;             /* 用户可见的节点名，例如 "mcu1" */
    struct m9p_client *client;    /* 通往该节点的 Mini9P client */
    bool auto_attach;             /* 启动注册后是否立即尝试 attach */
};

static struct m9p_client g_mcu1_client;
static struct m9p_peer_link g_mcu1_peer_link;
static uint8_t g_mcu1_peer_link_rx[M9P_DEFAULT_MSIZE];
static uint8_t g_mcu1_peer_link_tx[M9P_DEFAULT_MSIZE];

int cluster_init_static_nodes(void)
{
    struct m9p_peer_link_config peer_link_config;
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

    /*
     * Master 当前还没有本地 mini9p_server backend，因此这里只把 raw UART transport
     * 包成 peer_link，用来确保“等待本端响应”和“接收对端主动请求”不会混帧。
     * 若对端此时主动发来 T*，peer_link 会回 Rerror ENOTSUP，而不是把它误认为本端响应。
     */
    m9p_peer_link_get_default_config(&peer_link_config);
    peer_link_config.send_frame = (m9p_peer_link_send_frame_fn)m9p_uart_transport_send_frame;
    peer_link_config.receive_frame = (m9p_peer_link_receive_frame_fn)m9p_uart_transport_receive_frame;
    peer_link_config.transport_ctx = m9p_uart_transport_default();
    peer_link_config.dispatch_rx_buffer = g_mcu1_peer_link_rx;
    peer_link_config.dispatch_rx_cap = sizeof(g_mcu1_peer_link_rx);
    peer_link_config.dispatch_tx_buffer = g_mcu1_peer_link_tx;
    peer_link_config.dispatch_tx_cap = sizeof(g_mcu1_peer_link_tx);
    ret = m9p_peer_link_init(&g_mcu1_peer_link, &peer_link_config);
    if (ret < 0) {
        return ret;
    }

    m9p_client_init(&g_mcu1_client,
                    m9p_peer_link_request,
                    &g_mcu1_peer_link);

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
