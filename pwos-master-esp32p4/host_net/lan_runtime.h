#ifndef PWOS_LAN_RUNTIME_H
#define PWOS_LAN_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_LAN_HOSTNAME_PREFIX "pwos"
#define PWOS_LAN_HOSTNAME_CAP 32u

typedef struct {
    uint8_t initialized;
    uint8_t started;
    uint8_t link_up;
    uint8_t has_ip;
    uint8_t mdns_ready;
    uint8_t host_rpc_mdns_ready;
    uint8_t mac[6];
    char hostname[PWOS_LAN_HOSTNAME_CAP];
    char ip[16];
    char netmask[16];
    char gateway[16];
    uint32_t start_attempts;
    uint32_t start_failures;
    uint32_t link_up_events;
    uint32_t link_down_events;
    uint32_t got_ip_events;
    int32_t last_error;
} pwos_lan_runtime_status_t;

/* 启动有线网络。失败只返回错误，不会中止 coordinator。 */
int pwos_lan_runtime_start(void);

int pwos_lan_runtime_publish_host_rpc(
    const uint32_t uid[3],
    uint32_t epoch,
    uint16_t priority,
    uint16_t port);

void pwos_lan_runtime_get_status(pwos_lan_runtime_status_t *out_status);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_LAN_RUNTIME_H */
