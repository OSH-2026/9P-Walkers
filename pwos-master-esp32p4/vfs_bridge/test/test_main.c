#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "cluster_vfs.h"

static int mock_transport(void *ctx, const uint8_t *tx, size_t tx_len,
                          uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
    (void)ctx;
    (void)tx;
    (void)rx;
    (void)rx_cap;
    (void)tx_len;
    *rx_len = 0;
    return 0;
}

int main(void)
{
    printf("cluster_vfs test runner start\n");

    cluster_vfs_init();
    printf("init ok\n");

    struct m9p_client client;
    m9p_client_init(&client, mock_transport, NULL);

    int rc = cluster_vfs_add_direct("mcu1", &client);
    printf("add_direct returned %d\n", rc);

    printf("test runner finish\n");
    return 0;
}
