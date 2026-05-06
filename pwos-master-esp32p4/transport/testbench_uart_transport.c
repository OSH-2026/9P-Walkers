// testbench_uart_transport.c
// 在 PC 外部仿真环境中验证非 ESP32 桩分支的接口行为。
#include "uart_transport.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    struct m9p_uart_transport transport;
    struct m9p_uart_transport_config config;
    m9p_uart_transport_get_default_config(&config);
    int ret = m9p_uart_transport_init(&transport, &config);
    assert(ret == 0);
    uint8_t rx[16];
    size_t rx_len = 0;
    ret = m9p_uart_transport_request(&transport, (const uint8_t*)"test", 4, rx, sizeof(rx), &rx_len);
    assert(ret < 0); // 非 ESP32 平台下应返回 ENOTSUP
    printf("request ret=%d\n", ret);
    m9p_uart_transport_deinit(&transport);
    return 0;
}
