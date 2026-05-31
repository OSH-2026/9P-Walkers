#include "mesh_diag.h"

#ifdef PWOS_ENABLE_MESH_DIAG

#include <stdio.h>
#include <string.h>

#include "usart.h"

static void mesh_diag_write(const char *text)
{
    if (text == NULL) {
        return;
    }

    (void)HAL_UART_Transmit(
        &huart1,
        (const uint8_t *)text,
        (uint16_t)strlen(text),
        20u);
}

void mesh_diag_text(const char *text)
{
    mesh_diag_write(text);
    mesh_diag_write("\r\n");
}

void mesh_diag_kv_u32(const char *key, uint32_t value)
{
    char buffer[64];
    int n = snprintf(buffer, sizeof(buffer), "%s=%lu\r\n", key, (unsigned long)value);

    if (n > 0) {
        mesh_diag_write(buffer);
    }
}

void mesh_diag_send_frame(uint8_t port_id, uint8_t next_hop, size_t tx_len, int rc)
{
    char buffer[96];
    int n = snprintf(
        buffer,
        sizeof(buffer),
        "mesh tx port=%u next=0x%02x len=%lu rc=%d\r\n",
        (unsigned)port_id,
        (unsigned)next_hop,
        (unsigned long)tx_len,
        rc);

    if (n > 0) {
        mesh_diag_write(buffer);
    }
}

void mesh_diag_recv_frame(uint8_t port_id, size_t rx_len, int rc)
{
    char buffer[96];
    int n = snprintf(
        buffer,
        sizeof(buffer),
        "mesh rx port=%u len=%lu rc=%d\r\n",
        (unsigned)port_id,
        (unsigned long)rx_len,
        rc);

    if (n > 0) {
        mesh_diag_write(buffer);
    }
}

#endif
