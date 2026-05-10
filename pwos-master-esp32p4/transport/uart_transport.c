/*
 * 系统角色约定：ESP32 是主控（master），STM32 是从机（slave）。
 * 本文件里的条件编译只区分“是否运行在 ESP32/ESP-IDF 平台上”，
 * 不代表系统中的主从角色。非 ESP32 分支仅用于 PC 外部仿真和接口验证。
 */
#include "uart_transport.h"
#include "mini9p_protocol.h"

// #define ESP_PLATFORM  /* 这个宏由 ESP-IDF 构建系统自动定义，表示当前编译环境是 ESP32 平台。 */

#include <string.h>

/*
 * 真正的 ESP-IDF 构建会自动定义 ESP_PLATFORM。
 * 这里不要手工 #define 它，否则即使在 PC/编辑器环境里，
 * 代码也会强行进入 ESP32 分支，随后出现：
 * 1. 找不到 driver/uart.h 以及 freertos 下的相关头文件
 * 2. 连带找不到 TickType_t、SemaphoreHandle_t 的定义
 */
#ifdef ESP_PLATFORM
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

/* 全局默认 transport，给“开箱即用”的初始化接口使用。 */
static struct m9p_uart_transport g_default_transport;

/**
 * @brief 获取 UART 传输默认配置。
 * @param[out] out_config 输出配置结构体指针，不能为空。
 * @note 默认配置面向 ESP32 主控侧 UART；在非 ESP32 平台下仅用于桩对象初始化。
 */
void m9p_uart_transport_get_default_config(struct m9p_uart_transport_config *out_config)
{
    if (out_config == NULL) {
        return;
    }

    out_config->uart_port = M9P_UART_TRANSPORT_DEFAULT_PORT;
    out_config->tx_pin = M9P_UART_TRANSPORT_DEFAULT_TX_PIN;
    out_config->rx_pin = M9P_UART_TRANSPORT_DEFAULT_RX_PIN;
    out_config->baud_rate = M9P_UART_TRANSPORT_DEFAULT_BAUD_RATE;
    out_config->io_timeout_ms = M9P_UART_TRANSPORT_DEFAULT_TIMEOUT_MS;
    out_config->rx_buffer_size = M9P_UART_TRANSPORT_DEFAULT_RX_BUFFER_SIZE;
    out_config->tx_buffer_size = M9P_UART_TRANSPORT_DEFAULT_TX_BUFFER_SIZE;
    out_config->flush_before_request = true;
    out_config->flush_before_receive = false;
}

#ifndef ESP_PLATFORM

/**
 * @brief 初始化 UART 传输上下文（非 ESP32 平台桩实现）。
 * @param[in,out] transport 传输上下文指针。
 * @param[in] config 配置指针。
 * @retval 0 成功
 * @retval <0 错误码
 * @note 该分支用于 PC 外部仿真环境，lock 固定为 NULL，仅完成结构体赋值。
 */
int m9p_uart_transport_init(struct m9p_uart_transport *transport,
                            const struct m9p_uart_transport_config *config)
{
    if (transport == NULL || config == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    transport->config = *config;
    transport->tx_lock = NULL;
    transport->rx_lock = NULL;
    transport->exchange_lock = NULL;
    transport->initialized = true;
    return 0;
}

/**
 * @brief 发送一整帧数据到 UART（非 ESP32 平台桩实现）。
 * @param[in,out] transport 传输上下文指针。
 * @param[in] tx_data 待发送帧数据。
 * @param[in] tx_len 待发送帧长度。
 * @retval <0 非 ESP32 平台下恒返回 ENOTSUP。
 */
int m9p_uart_transport_send_frame(struct m9p_uart_transport *transport,
                                  const uint8_t *tx_data,
                                  size_t tx_len)
{
    (void)transport;
    (void)tx_data;
    (void)tx_len;

    return -(int)M9P_ERR_ENOTSUP;
}

/**
 * @brief 从 UART 接收一整帧数据（非 ESP32 平台桩实现）。
 * @param[in,out] transport 传输上下文指针。
 * @param[out] rx_data 接收缓冲区。
 * @param[in] rx_cap 接收缓冲区容量。
 * @param[out] rx_len 实际接收长度。
 * @retval <0 非 ESP32 平台下恒返回 ENOTSUP。
 */
int m9p_uart_transport_receive_frame(struct m9p_uart_transport *transport,
                                     uint8_t *rx_data,
                                     size_t rx_cap,
                                     size_t *rx_len)
{
    (void)transport;
    (void)rx_data;
    (void)rx_cap;

    if (rx_len != NULL) {
        *rx_len = 0u;
    }
    return -(int)M9P_ERR_ENOTSUP;
}

/**
 * @brief 反初始化 UART 传输上下文（非 ESP32 平台桩实现）。
 * @param[in,out] transport 传输上下文指针。
 * @note 该分支不持有真实 UART 资源，因此只需要清零结构体。
 */
void m9p_uart_transport_deinit(struct m9p_uart_transport *transport)
{
    if (transport == NULL) {
        return;
    }

    memset(transport, 0, sizeof(*transport));
}

/**
 * @brief 发送 mini9P 请求并接收响应（非 ESP32 平台桩实现）。
 * @param[in] transport_ctx 传输上下文指针。
 * @param[in] tx_data 待发送数据。
 * @param[in] tx_len 发送长度。
 * @param[out] rx_data 接收缓冲区。
 * @param[in] rx_cap 缓冲区容量。
 * @param[out] rx_len 实际接收长度。
 * @retval <0 非 ESP32 平台下恒返回 ENOTSUP。
 * @note 仅用于 PC 外部仿真环境中的接口兼容性验证，不表示主控角色发生变化。
 */
int m9p_uart_transport_request(void *transport_ctx,
                               const uint8_t *tx_data,
                               size_t tx_len,
                               uint8_t *rx_data,
                               size_t rx_cap,
                               size_t *rx_len)
{
    (void)transport_ctx;
    (void)tx_data;
    (void)tx_len;
    (void)rx_data;
    (void)rx_cap;

    if (rx_len != NULL) {
        *rx_len = 0u;
    }
    return -(int)M9P_ERR_ENOTSUP;
}

/**
 * @brief 接收一帧请求、调用处理器并回发响应（非 ESP32 平台桩实现）。
 * @param[in,out] transport 传输上下文指针。
 * @param[in] handler 服务端帧处理回调。
 * @param[in] server_ctx 传给回调的用户上下文。
 * @param[out] rx_data 请求帧缓冲区。
 * @param[in] rx_cap 请求帧缓冲区容量。
 * @param[out] rx_len 实际请求长度。
 * @param[out] tx_data 响应帧缓冲区。
 * @param[in] tx_cap 响应帧缓冲区容量。
 * @param[out] tx_len 实际响应长度。
 * @retval <0 非 ESP32 平台下恒返回 ENOTSUP。
 */
int m9p_uart_transport_serve_once(struct m9p_uart_transport *transport,
                                  m9p_server_transport_fn handler,
                                  void *server_ctx,
                                  uint8_t *rx_data,
                                  size_t rx_cap,
                                  size_t *rx_len,
                                  uint8_t *tx_data,
                                  size_t tx_cap,
                                  size_t *tx_len)
{
    (void)transport;
    (void)handler;
    (void)server_ctx;
    (void)rx_data;
    (void)rx_cap;
    (void)tx_data;
    (void)tx_cap;

    if (rx_len != NULL) {
        *rx_len = 0u;
    }
    if (tx_len != NULL) {
        *tx_len = 0u;
    }
    return -(int)M9P_ERR_ENOTSUP;
}

#else

/*
 * 统一做一个“毫秒超时兜底”。
 * 如果用户把超时配成 0，这里强制改成 1ms，避免后面出现“永远不等”或行为不明确。
 */
static int transport_timeout_ms(const struct m9p_uart_transport *transport)
{
    if (transport->config.io_timeout_ms == 0u) {
        return 1;
    }

    return (int)transport->config.io_timeout_ms;
}

/*
 * TickType_t 是 FreeRTOS 的“系统节拍计数”类型。
 * 它不是毫秒，真正单位取决于 RTOS 的 tick 配置，所以要用 pdMS_TO_TICKS 做换算。
 * 这类定义来自 freertos/FreeRTOS.h。
 */
static TickType_t transport_timeout_ticks(const struct m9p_uart_transport *transport)
{
    TickType_t ticks = pdMS_TO_TICKS((TickType_t)transport_timeout_ms(transport));

    return ticks == 0 ? 1 : ticks;
}

/*
 * 头文件里把锁字段存成 void *，是为了让非 ESP32 平台也能包含 uart_transport.h，
 * 不必被迫依赖 FreeRTOS 头文件；真正到了 ESP32 分支里，再转回具体类型。
 * 这里把 TX、RX 和“整轮事务”拆成三把锁：
 * 1. raw send_frame 只拿 TX 锁
 * 2. raw receive_frame 只拿 RX 锁
 * 3. request / serve_once 再额外拿 exchange 锁，确保 helper 仍是单事务语义
 */
static SemaphoreHandle_t transport_tx_lock(const struct m9p_uart_transport *transport)
{
    return (SemaphoreHandle_t)transport->tx_lock;
}

static SemaphoreHandle_t transport_rx_lock(const struct m9p_uart_transport *transport)
{
    return (SemaphoreHandle_t)transport->rx_lock;
}

static SemaphoreHandle_t transport_exchange_lock(const struct m9p_uart_transport *transport)
{
    return (SemaphoreHandle_t)transport->exchange_lock;
}

/* 拿锁失败直接返回 EBUSY，让调用方知道对应方向当前已被其他事务占用。 */
static int transport_take_mutex(const struct m9p_uart_transport *transport,
                                SemaphoreHandle_t lock)
{
    if (lock == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    if (xSemaphoreTake(lock, transport_timeout_ticks(transport)) != pdTRUE) {
        return -(int)M9P_ERR_EBUSY;
    }

    return 0;
}

static int transport_take_tx_lock(struct m9p_uart_transport *transport)
{
    return transport_take_mutex(transport, transport_tx_lock(transport));
}

static int transport_take_rx_lock(struct m9p_uart_transport *transport)
{
    return transport_take_mutex(transport, transport_rx_lock(transport));
}

static int transport_take_exchange_lock(struct m9p_uart_transport *transport)
{
    return transport_take_mutex(transport, transport_exchange_lock(transport));
}

/* 和 transport_take_*_lock 配套的释放动作，保证所有路径都能把锁还回去。 */
static void transport_give_mutex(SemaphoreHandle_t lock)
{
    if (lock != NULL) {
        xSemaphoreGive(lock);
    }
}

static void transport_give_tx_lock(struct m9p_uart_transport *transport)
{
    transport_give_mutex(transport_tx_lock(transport));
}

static void transport_give_rx_lock(struct m9p_uart_transport *transport)
{
    transport_give_mutex(transport_rx_lock(transport));
}

static void transport_give_exchange_lock(struct m9p_uart_transport *transport)
{
    transport_give_mutex(transport_exchange_lock(transport));
}

static void transport_delete_locks(struct m9p_uart_transport *transport)
{
    if (transport_tx_lock(transport) != NULL) {
        vSemaphoreDelete(transport_tx_lock(transport));
        transport->tx_lock = NULL;
    }
    if (transport_rx_lock(transport) != NULL) {
        vSemaphoreDelete(transport_rx_lock(transport));
        transport->rx_lock = NULL;
    }
    if (transport_exchange_lock(transport) != NULL) {
        vSemaphoreDelete(transport_exchange_lock(transport));
        transport->exchange_lock = NULL;
    }
}

/* ESP-IDF 的错误码和 mini9P 自己的错误码不是一套，这里做一层翻译。 */
static int esp_err_to_m9p(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return 0;
    case ESP_ERR_INVALID_ARG:
        return -(int)M9P_ERR_EINVAL;
    case ESP_ERR_INVALID_STATE:
        return -(int)M9P_ERR_EBUSY;
    case ESP_ERR_TIMEOUT:
        return -(int)M9P_ERR_EAGAIN;
    case ESP_ERR_NO_MEM:
        return -(int)M9P_ERR_EBUSY;
    default:
        return -(int)M9P_ERR_EIO;
    }
}

/* ESP32 分支里所有输入清空都走这一层，便于把底层错误码统一翻译成 mini9P 风格。 */
static int flush_input(struct m9p_uart_transport *transport)
{
    return esp_err_to_m9p(uart_flush_input((uart_port_t)transport->config.uart_port));
}

/*
 * UART 一次读取不一定就把想要的字节全部拿到。
 * 所以这里循环读，直到：
 * 1. 读满 len 个字节
 * 2. 超时
 * 3. 发生串口错误
 */
static int read_exact(struct m9p_uart_transport *transport,
                      uint8_t *buf,
                      size_t len)
{
    TickType_t deadline = xTaskGetTickCount() + transport_timeout_ticks(transport);
    size_t total = 0u;

    while (total < len) {
        TickType_t now = xTaskGetTickCount();
        TickType_t remaining = now < deadline ? (deadline - now) : 0;
        int chunk;

        if (remaining == 0) {
            return -(int)M9P_ERR_EAGAIN;
        }

        chunk = uart_read_bytes((uart_port_t)transport->config.uart_port,
                                buf + total,
                                len - total,
                                remaining);
        if (chunk < 0) {
            return -(int)M9P_ERR_EIO;
        }
        if (chunk == 0) {
            return -(int)M9P_ERR_EAGAIN;
        }

        total += (size_t)chunk;
    }

    return 0;
}

/*
 * 发送一整帧原始 UART 数据。
 * 这个函数只负责“把这帧写出去并确认底层发送完成”，
 * 不关心它是 mini9P 请求还是响应，也不关心后面是不是还要继续接收。
 */
static int send_frame_locked(struct m9p_uart_transport *transport,
                             const uint8_t *tx_data,
                             size_t tx_len)
{
    int written;
    int ret;

    written = uart_write_bytes((uart_port_t)transport->config.uart_port, tx_data, tx_len);
    if (written < 0 || (size_t)written != tx_len) {
        return -(int)M9P_ERR_EIO;
    }

    ret = esp_err_to_m9p(uart_wait_tx_done((uart_port_t)transport->config.uart_port,
                                           transport_timeout_ticks(transport)));
    if (ret < 0) {
        return ret;
    }

    return 0;
}

/*
 * 接收一整帧原始 UART 数据。
 * 它先读 4 字节固定头部，再根据协议中的长度字段把剩余内容读满。
 * 之所以不一次性盲读固定大缓冲，是为了同时兼顾帧完整性和内存占用。
 */
static int receive_frame_locked(struct m9p_uart_transport *transport,
                                uint8_t *rx_data,
                                size_t rx_cap,
                                size_t *rx_len)
{
    uint8_t header[4];
    uint16_t frame_len_field;
    size_t total_len;
    int ret;

    ret = read_exact(transport, header, sizeof(header));
    if (ret < 0) {
        return ret;
    }
    if (header[0] != (uint8_t)'9' || header[1] != (uint8_t)'P') {
        return -(int)M9P_ERR_EIO;
    }

    frame_len_field = (uint16_t)header[2] | (uint16_t)((uint16_t)header[3] << 8);
    if (frame_len_field < 4u) {
        return -(int)M9P_ERR_EIO;
    }

    total_len = (size_t)frame_len_field + 6u;
    if (total_len > rx_cap) {
        (void)flush_input(transport);
        return -(int)M9P_ERR_EMSIZE;
    }

    memcpy(rx_data, header, sizeof(header));
    ret = read_exact(transport, rx_data + sizeof(header), total_len - sizeof(header));
    if (ret < 0) {
        return ret;
    }

    *rx_len = total_len;
    return 0;
}

/**
 * @brief 初始化 UART 传输上下文（ESP32 实现）。
 * @param[in,out] transport 传输上下文指针。
 * @param[in] config 配置指针。
 * @retval 0 成功
 * @retval <0 错误码
 * @note ESP32 下分配 TX/RX/事务三把互斥锁，安装 UART 驱动，配置参数。
 */
int m9p_uart_transport_init(struct m9p_uart_transport *transport,
                            const struct m9p_uart_transport_config *config)
{
    uart_config_t uart_cfg = {0};
    int ret;

    if (transport == NULL || config == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    if (config->rx_buffer_size < M9P_FRAME_OVERHEAD ||
        config->tx_buffer_size < M9P_FRAME_OVERHEAD ||
        config->baud_rate <= 0) {
        return -(int)M9P_ERR_EINVAL;
    }
    if (transport->initialized) {
        return 0;
    }

    transport->config = *config;
    transport->tx_lock = NULL;
    transport->rx_lock = NULL;
    transport->exchange_lock = NULL;
    transport->initialized = false;

    transport->tx_lock = xSemaphoreCreateMutex();
    transport->rx_lock = xSemaphoreCreateMutex();
    transport->exchange_lock = xSemaphoreCreateMutex();
    if (transport_tx_lock(transport) == NULL ||
        transport_rx_lock(transport) == NULL ||
        transport_exchange_lock(transport) == NULL) {
        transport_delete_locks(transport);
        memset(transport, 0, sizeof(*transport));
        return -(int)M9P_ERR_EBUSY;
    }

    ret = esp_err_to_m9p(uart_driver_install((uart_port_t)config->uart_port,
                                             config->rx_buffer_size,
                                             config->tx_buffer_size,
                                             0,
                                             NULL,
                                             0));
    if (ret < 0) {
        transport_delete_locks(transport);
        memset(transport, 0, sizeof(*transport));
        return ret;
    }

    uart_cfg.baud_rate = config->baud_rate;
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    ret = esp_err_to_m9p(uart_param_config((uart_port_t)config->uart_port, &uart_cfg));
    if (ret < 0) {
        (void)uart_driver_delete((uart_port_t)config->uart_port);
        transport_delete_locks(transport);
        memset(transport, 0, sizeof(*transport));
        return ret;
    }

    ret = esp_err_to_m9p(uart_set_pin((uart_port_t)config->uart_port,
                                      config->tx_pin,
                                      config->rx_pin,
                                      UART_PIN_NO_CHANGE,
                                      UART_PIN_NO_CHANGE));
    if (ret < 0) {
        (void)uart_driver_delete((uart_port_t)config->uart_port);
        transport_delete_locks(transport);
        memset(transport, 0, sizeof(*transport));
        return ret;
    }

    (void)uart_flush_input((uart_port_t)config->uart_port);
    transport->initialized = true;
    return 0;
}

/**
 * @brief 发送一整帧原始 UART 数据（ESP32 实现）。
 * @param[in,out] transport 传输上下文指针。
 * @param[in] tx_data 待发送帧数据。
 * @param[in] tx_len 帧长度。
 * @retval 0 成功
 * @retval <0 错误码
 * @note 该接口只拿 TX 锁；因此在 raw API 级别，发送和接收可以并行发生。
 */
int m9p_uart_transport_send_frame(struct m9p_uart_transport *transport,
                                  const uint8_t *tx_data,
                                  size_t tx_len)
{
    int ret;

    if (transport == NULL || !transport->initialized || tx_data == NULL || tx_len == 0u) {
        return -(int)M9P_ERR_EINVAL;
    }

    ret = transport_take_tx_lock(transport);
    if (ret < 0) {
        return ret;
    }

    ret = send_frame_locked(transport, tx_data, tx_len);
    transport_give_tx_lock(transport);
    return ret;
}

/**
 * @brief 接收一整帧原始 UART 数据（ESP32 实现）。
 * @param[in,out] transport 传输上下文指针。
 * @param[out] rx_data 接收缓冲区。
 * @param[in] rx_cap 接收缓冲区容量。
 * @param[out] rx_len 实际接收长度。
 * @retval 0 成功
 * @retval <0 错误码
 * @note 该接口只拿 RX 锁；因此在 raw API 级别，接收和发送可以并行发生。
 */
int m9p_uart_transport_receive_frame(struct m9p_uart_transport *transport,
                                     uint8_t *rx_data,
                                     size_t rx_cap,
                                     size_t *rx_len)
{
    int ret;

    if (transport == NULL || !transport->initialized || rx_data == NULL || rx_len == NULL ||
        rx_cap < M9P_FRAME_OVERHEAD) {
        return -(int)M9P_ERR_EINVAL;
    }

    *rx_len = 0u;

    ret = transport_take_rx_lock(transport);
    if (ret < 0) {
        return ret;
    }

    if (transport->config.flush_before_receive) {
        ret = flush_input(transport);
        if (ret < 0) {
            transport_give_rx_lock(transport);
            return ret;
        }
    }

    ret = receive_frame_locked(transport, rx_data, rx_cap, rx_len);
    transport_give_rx_lock(transport);
    return ret;
}

/**
 * @brief 反初始化 UART 传输上下文（ESP32 实现）。
 * @param[in,out] transport 传输上下文指针。
 * @note 卸载 UART 驱动，释放 TX/RX/事务三把互斥锁，清零结构体。
 */
void m9p_uart_transport_deinit(struct m9p_uart_transport *transport)
{
    if (transport == NULL || !transport->initialized) {
        return;
    }

    (void)uart_driver_delete((uart_port_t)transport->config.uart_port);
    transport_delete_locks(transport);
    memset(transport, 0, sizeof(*transport));
}

/**
 * @brief 发送 mini9P 请求并接收完整响应（ESP32 实现）。
 * @param[in] transport_ctx 传输上下文指针（struct m9p_uart_transport *）。
 * @param[in] tx_data 待发送帧。
 * @param[in] tx_len 发送长度。
 * @param[out] rx_data 接收缓冲区。
 * @param[in] rx_cap 缓冲区容量。
 * @param[out] rx_len 实际接收长度。
 * @retval 0 成功
 * @retval <0 错误码
 * @note 这是“单事务 helper”：它会同时占住 exchange/TX/RX，保证一轮 request/response
 *       不会被其他 raw API 插入；若需要真正的双向并发调度，应改用 raw send/receive
 *       再配合上层 dispatcher 做分发。
 */
int m9p_uart_transport_request(void *transport_ctx,
                               const uint8_t *tx_data,
                               size_t tx_len,
                               uint8_t *rx_data,
                               size_t rx_cap,
                               size_t *rx_len)
{
    struct m9p_uart_transport *transport = (struct m9p_uart_transport *)transport_ctx;
    int ret = 0;

    if (transport == NULL || !transport->initialized || tx_data == NULL || tx_len == 0u ||
        rx_data == NULL || rx_len == NULL || rx_cap < M9P_FRAME_OVERHEAD) {
        return -(int)M9P_ERR_EINVAL;
    }

    /* 先清零输出长度，避免调用方误用旧值。 */
    *rx_len = 0u;

    /* 事务 helper 先拿 exchange 锁，再拿 RX/TX 锁，保证这一轮 request/response 原子完成。 */
    ret = transport_take_exchange_lock(transport);
    if (ret < 0) {
        return ret;
    }

    ret = transport_take_rx_lock(transport);
    if (ret < 0) {
        goto out_exchange;
    }

    ret = transport_take_tx_lock(transport);
    if (ret < 0) {
        goto out_rx;
    }

    if (transport->config.flush_before_request) {
        /* 丢掉串口里残留的旧数据，减少把上一次碎片误当成新响应的风险。 */
        ret = flush_input(transport);
        if (ret < 0) {
            goto out;
        }
    }

    /* 请求模式下要求一个完整原子事务：发完整帧，再接完整帧。 */
    ret = send_frame_locked(transport, tx_data, tx_len);
    if (ret < 0) {
        goto out;
    }

    ret = receive_frame_locked(transport, rx_data, rx_cap, rx_len);

out:
    transport_give_tx_lock(transport);
out_rx:
    transport_give_rx_lock(transport);
out_exchange:
    /* 无论成功失败都要放锁，否则后续调用会被永久卡住。 */
    transport_give_exchange_lock(transport);
    return ret;
}

/**
 * @brief 接收一帧请求、调用处理器并回发响应（ESP32 实现）。
 * @param[in,out] transport 传输上下文指针。
 * @param[in] handler 服务端处理器，负责把请求帧变成响应帧。
 * @param[in] server_ctx 回调上下文。
 * @param[out] rx_data 请求帧缓冲区。
 * @param[in] rx_cap 请求帧缓冲区容量。
 * @param[out] rx_len 实际收到的请求帧长度。
 * @param[out] tx_data 响应帧缓冲区。
 * @param[in] tx_cap 响应帧缓冲区容量。
 * @param[out] tx_len 实际发出的响应帧长度。
 * @retval 0 成功
 * @retval <0 错误码
 * @note 这同样是“单事务 helper”：它会同时占住 exchange/TX/RX，确保本轮
 *       receive -> handler -> send 不会和其他 raw API 交织。
 */
int m9p_uart_transport_serve_once(struct m9p_uart_transport *transport,
                                  m9p_server_transport_fn handler,
                                  void *server_ctx,
                                  uint8_t *rx_data,
                                  size_t rx_cap,
                                  size_t *rx_len,
                                  uint8_t *tx_data,
                                  size_t tx_cap,
                                  size_t *tx_len)
{
    size_t local_rx_len = 0u;
    size_t local_tx_len = 0u;
    int ret;

    if (transport == NULL || !transport->initialized || handler == NULL || rx_data == NULL ||
        tx_data == NULL || rx_cap < M9P_FRAME_OVERHEAD || tx_cap < M9P_FRAME_OVERHEAD) {
        return -(int)M9P_ERR_EINVAL;
    }

    if (rx_len != NULL) {
        *rx_len = 0u;
    }
    if (tx_len != NULL) {
        *tx_len = 0u;
    }

    ret = transport_take_exchange_lock(transport);
    if (ret < 0) {
        return ret;
    }

    ret = transport_take_rx_lock(transport);
    if (ret < 0) {
        goto out_exchange;
    }

    ret = transport_take_tx_lock(transport);
    if (ret < 0) {
        goto out_rx;
    }

    if (transport->config.flush_before_receive) {
        ret = flush_input(transport);
        if (ret < 0) {
            goto out;
        }
    }

    ret = receive_frame_locked(transport, rx_data, rx_cap, &local_rx_len);
    if (ret < 0) {
        goto out;
    }

    ret = handler(server_ctx, rx_data, local_rx_len, tx_data, tx_cap, &local_tx_len);
    if (ret < 0) {
        goto out;
    }
    if (local_tx_len == 0u) {
        ret = -(int)M9P_ERR_EINVAL;
        goto out;
    }

    ret = send_frame_locked(transport, tx_data, local_tx_len);
    if (ret < 0) {
        goto out;
    }

    if (rx_len != NULL) {
        *rx_len = local_rx_len;
    }
    if (tx_len != NULL) {
        *tx_len = local_tx_len;
    }

out:
    transport_give_tx_lock(transport);
out_rx:
    transport_give_rx_lock(transport);
out_exchange:
    transport_give_exchange_lock(transport);
    return ret;
}

#endif

/**
 * @brief 初始化全局默认 UART 传输实例。
 * @retval 0 成功
 * @retval <0 错误码
 * @note 线程安全，重复调用无副作用。
 */
int m9p_uart_transport_init_default(void)
{
    struct m9p_uart_transport_config config;

    if (g_default_transport.initialized) {
        return 0;
    }

    m9p_uart_transport_get_default_config(&config);
    return m9p_uart_transport_init(&g_default_transport, &config);
}

/**
 * @brief 获取全局默认 UART 传输实例指针。
 * @retval struct m9p_uart_transport* 指针
 */
struct m9p_uart_transport *m9p_uart_transport_default(void)
{
    return &g_default_transport;
}
