#ifndef PWOS_COORDINATOR_RUNTIME_H
#define PWOS_COORDINATOR_RUNTIME_H

#include <stdint.h>

#include "host_coordinator.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_COORDINATOR_UART_PORT 1
#define PWOS_COORDINATOR_UART_TX_PIN 37
#define PWOS_COORDINATOR_UART_RX_PIN 38
#define PWOS_COORDINATOR_UART_BAUD_RATE 1000000

typedef struct {
    uint8_t initialized;
    uint8_t task_started;
    uint8_t node_count;
    uint8_t uart_port;
    uint32_t rx_bytes;
    uint32_t rx_frames;
    uint32_t rx_parse_errors;
    uint32_t tx_bytes;
    uint32_t tx_frames;
    uint32_t tx_errors;
    uint32_t hello_tx;
    uint32_t hello_rx;
    uint32_t hello_ack_tx;
    uint32_t hello_ack_rx;
    uint32_t register_rx;
    uint32_t assign_tx;
    uint32_t lease_renew_rx;
    uint32_t lease_ack_tx;
    uint32_t link_state_rx;
    uint32_t route_update_tx;
    uint32_t data_rx;
    uint32_t mini9p_tx;
    uint32_t mini9p_rx;
    uint32_t mini9p_probe_ok;
    uint32_t mini9p_probe_fail;
    uint8_t mini9p_last_addr;
    int32_t mini9p_last_error;
    uint32_t last_rx_tick;
    uint32_t last_tx_tick;
} pwos_coordinator_runtime_stats_t;

int pwos_coordinator_runtime_start_default(void);

void pwos_coordinator_runtime_get_stats(pwos_coordinator_runtime_stats_t *out_stats);

const pwos_host_coordinator_t *pwos_coordinator_runtime_get_coordinator(void);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_COORDINATOR_RUNTIME_H */
