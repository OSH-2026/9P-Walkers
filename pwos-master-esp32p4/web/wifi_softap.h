#ifndef PWOS_MASTER_WIFI_SOFTAP_H
#define PWOS_MASTER_WIFI_SOFTAP_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 初始化 NVS、netif 和事件循环，然后以 WiFi 接入点 (SoftAP)
 * 模式启动 ESP32-P4，以便浏览器可以在没有路由器的情况下访问 Web Shell。
 *
 * 默认 AP:  SSID "9P-Walkers"  密码 "pwos1234"
 * 默认 IP:  192.168.4.1  (与 index.html 中的回退地址一致)
 */
void wifi_softap_init(void);

#ifdef __cplusplus
}
#endif

#endif
