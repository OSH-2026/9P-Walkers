#ifndef PWOS_FAULT_CONTROL_H
#define PWOS_FAULT_CONTROL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_FAULT_MAX_PORTS 5u

typedef struct {
    uint8_t enabled;
    uint8_t port_id;
    uint8_t forced_down;
    uint8_t drop_percent;
    uint8_t corrupt_percent;
    uint32_t delay_ms;
    uint32_t frames_seen;
    uint32_t dropped;
    uint32_t rx_dropped;
    uint32_t corrupted;
    uint32_t delayed;
} pwos_fault_port_snapshot_t;

void pwos_fault_control_init(void);

/*
 * 在 link_tx_task 中调用。返回 1 表示本帧被注入丢弃，0 表示继续发送。
 * data 可能被故意破坏，delay 在任务上下文执行，禁止从 ISR 调用。
 */
int pwos_fault_control_before_tx(
    uint8_t port_id,
    uint8_t *data,
    size_t len);

/* forced_down 时在唯一 link_rx_task 丢弃对应端口的入站帧。 */
int pwos_fault_control_drop_rx(uint8_t port_id);

int pwos_fault_control_command(const uint8_t *data, uint16_t len);

/* port_mgr_task 周期调用，用于在命令响应发出后执行延迟自重启。 */
void pwos_fault_control_poll(void);

uint8_t pwos_fault_control_reboot_pending(void);

size_t pwos_fault_control_get_snapshot(
    pwos_fault_port_snapshot_t *out,
    size_t out_capacity);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_FAULT_CONTROL_H */
