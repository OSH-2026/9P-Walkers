/**
 * @file pwos_log.c
 * @brief Global fixed-size ring log for PWOS node diagnostics.
 */

#include "pwos_log.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef MESH_UART_TRANSPORT_USE_STM32_HAL
#include "stm32f4xx_hal.h"
#endif

#define PWOS_LOG_CAP 64u

struct pwos_log_entry {
    uint32_t time_ms;
    uint8_t event;
    uint8_t flags;
    uint16_t len;
    uint32_t a;
    uint32_t b;
    uint32_t c;
    int rc;
};

static struct pwos_log_entry g_pwos_log[PWOS_LOG_CAP];
static size_t g_pwos_log_next;
static size_t g_pwos_log_count;

static uint32_t pwos_log_time_ms(void)
{
#ifdef MESH_UART_TRANSPORT_USE_STM32_HAL
    return HAL_GetTick();
#else
    return 0u;
#endif
}

static void pwos_log_record(const struct pwos_log_entry *entry)
{
    if (entry == NULL) {
        return;
    }

    g_pwos_log[g_pwos_log_next] = *entry;
    g_pwos_log_next = (g_pwos_log_next + 1u) % PWOS_LOG_CAP;
    if (g_pwos_log_count < PWOS_LOG_CAP) {
        ++g_pwos_log_count;
    }
}

void pwos_log_init(void)
{
    memset(g_pwos_log, 0, sizeof(g_pwos_log));
    g_pwos_log_next = 0u;
    g_pwos_log_count = 0u;
}

void pwos_log_event(uint8_t event, uint32_t a, uint32_t b, uint32_t c, int rc)
{
    struct pwos_log_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.time_ms = pwos_log_time_ms();
    entry.event = event == 0u ? PWOS_LOG_EVENT_GENERIC : event;
    entry.a = a;
    entry.b = b;
    entry.c = c;
    entry.rc = rc;
    pwos_log_record(&entry);
}

void pwos_log_mesh_tx(
    uint8_t event,
    uint8_t type,
    uint8_t src,
    uint8_t dst,
    uint8_t next_hop,
    uint8_t port_id,
    uint16_t len,
    uint8_t mini9p_type,
    int rc)
{
    struct pwos_log_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.time_ms = pwos_log_time_ms();
    entry.event = event;
    entry.flags = mini9p_type;
    entry.len = len;
    entry.a = ((uint32_t)type << 24) |
              ((uint32_t)src << 16) |
              ((uint32_t)dst << 8) |
              (uint32_t)next_hop;
    entry.b = port_id;
    entry.rc = rc;
    pwos_log_record(&entry);
}

static const char *pwos_log_event_name(uint8_t event)
{
    switch (event) {
    case PWOS_LOG_EVENT_MESH_SEND:
        return "send";
    case PWOS_LOG_EVENT_MESH_SEND_PORT:
        return "send_port";
    default:
        return "event";
    }
}

int pwos_log_format(char *out, size_t out_cap)
{
    size_t used = 0u;
    size_t i;

    if (out == NULL || out_cap == 0u) {
        return -1;
    }

    for (i = 0u; i < g_pwos_log_count && used < out_cap - 1u; ++i) {
        size_t index = (g_pwos_log_next + PWOS_LOG_CAP - g_pwos_log_count + i) % PWOS_LOG_CAP;
        const struct pwos_log_entry *entry = &g_pwos_log[index];
        int written;

        if (entry->event == PWOS_LOG_EVENT_MESH_SEND ||
            entry->event == PWOS_LOG_EVENT_MESH_SEND_PORT) {
            uint8_t type = (uint8_t)(entry->a >> 24);
            uint8_t src = (uint8_t)(entry->a >> 16);
            uint8_t dst = (uint8_t)(entry->a >> 8);
            uint8_t next_hop = (uint8_t)entry->a;

            if (entry->flags != 0xffu) {
                written = snprintf(
                    out + used,
                    out_cap - used,
                    "t=%lu ev=%s type=0x%02x src=0x%02x dst=0x%02x next=0x%02x port=%u len=%u m9p=0x%02x rc=%d\n",
                    (unsigned long)entry->time_ms,
                    pwos_log_event_name(entry->event),
                    type,
                    src,
                    dst,
                    next_hop,
                    (unsigned)entry->b,
                    (unsigned)entry->len,
                    (unsigned)entry->flags,
                    entry->rc);
            } else {
                written = snprintf(
                    out + used,
                    out_cap - used,
                    "t=%lu ev=%s type=0x%02x src=0x%02x dst=0x%02x next=0x%02x port=%u len=%u rc=%d\n",
                    (unsigned long)entry->time_ms,
                    pwos_log_event_name(entry->event),
                    type,
                    src,
                    dst,
                    next_hop,
                    (unsigned)entry->b,
                    (unsigned)entry->len,
                    entry->rc);
            }
        } else {
            written = snprintf(
                out + used,
                out_cap - used,
                "t=%lu ev=event id=%u a=%lu b=%lu c=%lu rc=%d\n",
                (unsigned long)entry->time_ms,
                (unsigned)entry->event,
                (unsigned long)entry->a,
                (unsigned long)entry->b,
                (unsigned long)entry->c,
                entry->rc);
        }

        if (written < 0) {
            return -1;
        }
        if ((size_t)written >= out_cap - used) {
            used = out_cap - 1u;
            break;
        }
        used += (size_t)written;
    }

    out[used] = '\0';
    return 0;
}
