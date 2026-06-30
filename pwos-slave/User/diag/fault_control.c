#include "fault_control.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "stm32f4xx_hal.h"
#include "task.h"

typedef struct {
    uint8_t forced_down;
    uint8_t drop_percent;
    uint8_t corrupt_percent;
    uint32_t delay_ms;
    uint32_t frames_seen;
    uint32_t dropped;
    uint32_t rx_dropped;
    uint32_t corrupted;
    uint32_t delayed;
} pwos_fault_port_t;

static pwos_fault_port_t g_fault_ports[PWOS_FAULT_MAX_PORTS];
static uint8_t g_reboot_pending;
static TickType_t g_reboot_at;

void pwos_fault_control_init(void)
{
    memset(g_fault_ports, 0, sizeof(g_fault_ports));
    g_reboot_pending = 0u;
    g_reboot_at = 0u;
}

int pwos_fault_control_before_tx(
    uint8_t port_id,
    uint8_t *data,
    size_t len)
{
#ifdef PWOS_ENABLE_FAULT_INJECTION
    pwos_fault_port_t state;
    uint32_t drop_sample;
    uint32_t corrupt_sample;
    uint8_t drop = 0u;
    uint8_t corrupt = 0u;

    if (port_id >= PWOS_FAULT_MAX_PORTS || data == NULL || len == 0u) {
        return 0;
    }

    taskENTER_CRITICAL();
    ++g_fault_ports[port_id].frames_seen;
    drop_sample = g_fault_ports[port_id].frames_seen % 100u;
    corrupt_sample = (g_fault_ports[port_id].frames_seen * 37u + 17u) % 100u;
    state = g_fault_ports[port_id];
    if (state.forced_down != 0u || drop_sample < state.drop_percent) {
        ++g_fault_ports[port_id].dropped;
        drop = 1u;
    } else if (corrupt_sample < state.corrupt_percent) {
        ++g_fault_ports[port_id].corrupted;
        corrupt = 1u;
    }
    if (state.delay_ms > 0u && drop == 0u) {
        ++g_fault_ports[port_id].delayed;
    }
    taskEXIT_CRITICAL();

    if (drop != 0u) {
        return 1;
    }
    if (state.delay_ms > 0u) {
        vTaskDelay(pdMS_TO_TICKS(state.delay_ms));
    }
    if (corrupt != 0u) {
        /* 修改 CRC 字节，确保接收端 parser 能统计到损坏帧。 */
        data[len - 1u] ^= 0x5Au;
    }
#else
    (void)port_id;
    (void)data;
    (void)len;
#endif
    return 0;
}

int pwos_fault_control_drop_rx(uint8_t port_id)
{
#ifdef PWOS_ENABLE_FAULT_INJECTION
    int drop = 0;

    if (port_id >= PWOS_FAULT_MAX_PORTS) {
        return 0;
    }
    taskENTER_CRITICAL();
    if (g_fault_ports[port_id].forced_down != 0u) {
        ++g_fault_ports[port_id].rx_dropped;
        drop = 1;
    }
    taskEXIT_CRITICAL();
    return drop;
#else
    (void)port_id;
    return 0;
#endif
}

int pwos_fault_control_command(const uint8_t *data, uint16_t len)
{
#ifdef PWOS_ENABLE_FAULT_INJECTION
    char command[96];
    unsigned int port;
    unsigned int value;
    int parsed;

    if (data == NULL || len == 0u || len >= sizeof(command)) {
        return -1;
    }
    memcpy(command, data, len);
    command[len] = '\0';

    if (strcmp(command, "clear") == 0) {
        taskENTER_CRITICAL();
        memset(g_fault_ports, 0, sizeof(g_fault_ports));
        g_reboot_pending = 0u;
        g_reboot_at = 0u;
        taskEXIT_CRITICAL();
        return 0;
    }
    if (strcmp(command, "reboot-self") == 0) {
        taskENTER_CRITICAL();
        g_reboot_pending = 1u;
        g_reboot_at = xTaskGetTickCount() + pdMS_TO_TICKS(300u);
        taskEXIT_CRITICAL();
        return 0;
    }
    parsed = sscanf(command, "drop port %u %u", &port, &value);
    if (parsed == 2 && port < PWOS_FAULT_MAX_PORTS && value <= 100u) {
        taskENTER_CRITICAL();
        g_fault_ports[port].drop_percent = (uint8_t)value;
        taskEXIT_CRITICAL();
        return 0;
    }
    parsed = sscanf(command, "corrupt port %u %u", &port, &value);
    if (parsed == 2 && port < PWOS_FAULT_MAX_PORTS && value <= 100u) {
        taskENTER_CRITICAL();
        g_fault_ports[port].corrupt_percent = (uint8_t)value;
        taskEXIT_CRITICAL();
        return 0;
    }
    parsed = sscanf(command, "delay port %u %u", &port, &value);
    if (parsed == 2 && port < PWOS_FAULT_MAX_PORTS && value <= 5000u) {
        taskENTER_CRITICAL();
        g_fault_ports[port].delay_ms = value;
        taskEXIT_CRITICAL();
        return 0;
    }
    parsed = sscanf(command, "down port %u", &port);
    if (parsed == 1 && port < PWOS_FAULT_MAX_PORTS) {
        taskENTER_CRITICAL();
        g_fault_ports[port].forced_down = 1u;
        taskEXIT_CRITICAL();
        return 0;
    }
    parsed = sscanf(command, "recover port %u", &port);
    if (parsed == 1 && port < PWOS_FAULT_MAX_PORTS) {
        taskENTER_CRITICAL();
        g_fault_ports[port].forced_down = 0u;
        taskEXIT_CRITICAL();
        return 0;
    }
    return -1;
#else
    (void)data;
    (void)len;
    return -2;
#endif
}

void pwos_fault_control_poll(void)
{
#ifdef PWOS_ENABLE_FAULT_INJECTION
    uint8_t reboot = 0u;
    TickType_t now = xTaskGetTickCount();

    taskENTER_CRITICAL();
    if (g_reboot_pending != 0u &&
        (int32_t)(now - g_reboot_at) >= 0) {
        g_reboot_pending = 0u;
        reboot = 1u;
    }
    taskEXIT_CRITICAL();
    if (reboot != 0u) {
        NVIC_SystemReset();
    }
#endif
}

uint8_t pwos_fault_control_reboot_pending(void)
{
    uint8_t pending;

    taskENTER_CRITICAL();
    pending = g_reboot_pending;
    taskEXIT_CRITICAL();
    return pending;
}

size_t pwos_fault_control_get_snapshot(
    pwos_fault_port_snapshot_t *out,
    size_t out_capacity)
{
    size_t i;
    size_t count;

    if (out == NULL || out_capacity == 0u) {
        return 0u;
    }
    count = out_capacity < PWOS_FAULT_MAX_PORTS ?
        out_capacity : PWOS_FAULT_MAX_PORTS;
    taskENTER_CRITICAL();
    for (i = 0u; i < count; ++i) {
#ifdef PWOS_ENABLE_FAULT_INJECTION
        out[i].enabled = 1u;
#else
        out[i].enabled = 0u;
#endif
        out[i].port_id = (uint8_t)i;
        out[i].forced_down = g_fault_ports[i].forced_down;
        out[i].drop_percent = g_fault_ports[i].drop_percent;
        out[i].corrupt_percent = g_fault_ports[i].corrupt_percent;
        out[i].delay_ms = g_fault_ports[i].delay_ms;
        out[i].frames_seen = g_fault_ports[i].frames_seen;
        out[i].dropped = g_fault_ports[i].dropped;
        out[i].rx_dropped = g_fault_ports[i].rx_dropped;
        out[i].corrupted = g_fault_ports[i].corrupted;
        out[i].delayed = g_fault_ports[i].delayed;
    }
    taskEXIT_CRITICAL();
    return count;
}
