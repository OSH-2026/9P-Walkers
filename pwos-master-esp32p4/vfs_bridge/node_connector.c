#include "node_connector.h"

#include <stdio.h>

#include "cluster_vfs.h"
#include "mini9p_client.h"
#include "uart_transport.h"

/*
 * 物理接线约定（ESP32-P4 侧）：
 *   UART1  TX → GPIO 17  →  STM32 USART1 RX (PA10)
 *   UART1  RX → GPIO 18  →  STM32 USART1 TX (PA9)
 *
 * 波特率与从机 main.c 中的 huart1.Init.BaudRate = 115200 保持一致。
 * 若从机改为 1000000，这里同步修改即可。
 */
#define SLAVE_UART_PORT   1
#define SLAVE_UART_TX_PIN 17
#define SLAVE_UART_RX_PIN 18
#define SLAVE_BAUD_RATE   115200
#define SLAVE_NODE_NAME   "mcu1"

static struct m9p_uart_transport g_slave_transport;
static struct m9p_client         g_slave_client;

int node_connector_init_static_slave(void)
{
    struct m9p_uart_transport_config cfg;
    int rc;

    m9p_uart_transport_get_default_config(&cfg);
    cfg.uart_port  = SLAVE_UART_PORT;
    cfg.tx_pin     = SLAVE_UART_TX_PIN;
    cfg.rx_pin     = SLAVE_UART_RX_PIN;
    cfg.baud_rate  = SLAVE_BAUD_RATE;

    rc = m9p_uart_transport_init(&g_slave_transport, &cfg);
    if (rc != 0) {
        printf("node_connector: uart init failed (rc=%d)\n", rc);
        return rc;
    }

    m9p_client_init(&g_slave_client, m9p_uart_transport_request, &g_slave_transport);

    rc = cluster_vfs_add_direct(SLAVE_NODE_NAME, &g_slave_client);
    if (rc != 0) {
        printf("node_connector: cluster_vfs_add_direct failed (rc=%d)\n", rc);
        return rc;
    }

    rc = cluster_vfs_attach(SLAVE_NODE_NAME);
    if (rc != 0) {
        /*
         * 从机可能还没上电或 UART 未接好；attach 失败不致命。
         * 路由已注册为 READY 状态，板子接好后在 Lua shell 里执行
         * vfs.attach("mcu1") 即可完成会话建立。
         */
        printf("node_connector: attach failed (rc=%d) — "
               "slave may not be connected; retry via vfs.attach(\"mcu1\")\n", rc);
        return 0;
    }

    printf("node_connector: mcu1 attached OK\n");
    return 0;
}
