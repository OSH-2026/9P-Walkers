/*
 * 系统角色约定：ESP32 是主控（master），STM32 是从机（slave）。
 * 但 transport 这一层不再把自己绑定死到“只能做 server”这个角色上。
 *
 * 这份 STM32 UART transport 现在同时支持两类用法：
 * 1. request 模式：主动发送一帧请求，再等待一帧响应。
 * 2. serve_once 模式：被动接收一帧请求，调用处理器，再回发一帧响应。
 *
 * 这样做的目的，是让主控和从机两边都能同时挂 mini9P client 和 mini9P server，
 * 上层如果要做双向 RPC、管理通道或者反向控制，就不必重新发明第二套串口收发逻辑。
 */
#include "uart_transport.h"

#include <limits.h>
#include <string.h>

/* 全局默认 transport，给“开箱即用”的初始化接口使用。 */
static struct m9p_uart_transport g_default_transport;

/*
 * 统一做一个“毫秒超时兜底”。
 * 如果调用方把超时设置成 0，这里强制改成 1ms，避免 HAL 出现不明确行为。
 */
static unsigned int transport_timeout_ms(const struct m9p_uart_transport *transport)
{
    if (transport->config.io_timeout_ms == 0u) {
        return 1u;
    }

    return transport->config.io_timeout_ms;
}

/*
 * STM32 HAL 和 mini9P 使用的错误语义不同。
 * 这一层把 HAL 的返回状态统一翻译成 mini9P 错误码，
 * 这样上层 client/server 看到的始终是一套风格一致的返回值。
 */
static int hal_status_to_m9p(HAL_StatusTypeDef status)
{
    switch (status) {
    case HAL_OK:
        return 0;
    case HAL_TIMEOUT:
        return -(int)M9P_ERR_EAGAIN;
    case HAL_BUSY:
        return -(int)M9P_ERR_EBUSY;
    case HAL_ERROR:
    default:
        return -(int)M9P_ERR_EIO;
    }
}

/*
 * 当前 STM32 侧没有引入 RTOS 互斥锁，因此这里把占用拆成三个轻量标志：
 * 1. tx_busy：raw send_frame 使用
 * 2. rx_busy：raw receive_frame 使用
 * 3. exchange_busy：request / serve_once 使用
 *
 * 这样 raw TX 和 raw RX 可以并行发生，更接近 UART 的物理全双工；
 * 而 request / serve_once 仍保持单事务 helper 语义，避免一轮 exchange 被打散。
 */
static int transport_claim_flag(bool *flag)
{
    if (*flag) {
        return -(int)M9P_ERR_EBUSY;
    }

    *flag = true;
    return 0;
}

static int transport_claim_tx(struct m9p_uart_transport *transport)
{
    return transport_claim_flag(&transport->tx_busy);
}

static int transport_claim_rx(struct m9p_uart_transport *transport)
{
    return transport_claim_flag(&transport->rx_busy);
}

static int transport_claim_exchange(struct m9p_uart_transport *transport)
{
    return transport_claim_flag(&transport->exchange_busy);
}

/* 和 transport_claim_* 配套的释放动作。 */
static void transport_release_flag(bool *flag)
{
    *flag = false;
}

static void transport_release_tx(struct m9p_uart_transport *transport)
{
    transport_release_flag(&transport->tx_busy);
}

static void transport_release_rx(struct m9p_uart_transport *transport)
{
    transport_release_flag(&transport->rx_busy);
}

static void transport_release_exchange(struct m9p_uart_transport *transport)
{
    transport_release_flag(&transport->exchange_busy);
}

/*
 * 把串口接收寄存器和 FIFO 里残留的旧字节尽量清掉。
 * 典型用途是：
 * 1. request 模式下，发新请求前先清碎片；
 * 2. serve_once 模式下，如果上层确认当前阶段只想处理“下一帧完整新请求”，
 *    也可以选择先丢弃残留数据。
 */
static int drain_rx_fifo(struct m9p_uart_transport *transport)
{
    uint8_t byte;

    while (HAL_UART_Receive(transport->config.uart, &byte, 1u, 1u) == HAL_OK) {
    }
    __HAL_UART_FLUSH_DRREGISTER(transport->config.uart);
    return 0;
}

/*
 * HAL_UART_Receive 一次不保证拿满所有目标字节。
 * 所以这里循环读，直到：
 * 1. 读满 len 个字节；
 * 2. 某次接收返回超时；
 * 3. 某次接收返回错误或 busy。
 */
static int read_exact(struct m9p_uart_transport *transport, uint8_t *buf, size_t len)
{
    size_t total = 0u;

    while (total < len) {
        size_t chunk = len - total;
        int ret;

        if (chunk > (size_t)UINT16_MAX) {
            chunk = (size_t)UINT16_MAX;
        }

        ret = hal_status_to_m9p(HAL_UART_Receive(transport->config.uart,
                                                 buf + total,
                                                 (uint16_t)chunk,
                                                 transport_timeout_ms(transport)));
        if (ret < 0) {
            return ret;
        }

        total += chunk;
    }

    return 0;
}

/*
 * 发送一整帧原始 UART 数据。
 * 这一层只负责“把这帧真正发出去”，不关心它是请求还是响应。
 */
static int send_frame_locked(struct m9p_uart_transport *transport,
                             const uint8_t *tx_data,
                             size_t tx_len)
{
    if (tx_len > (size_t)UINT16_MAX) {
        return -(int)M9P_ERR_EMSIZE;
    }

    return hal_status_to_m9p(HAL_UART_Transmit(transport->config.uart,
                                               tx_data,
                                               (uint16_t)tx_len,
                                               transport_timeout_ms(transport)));
}

/*
 * 接收一整帧原始 UART 数据。
 * 实现方式和主控侧一致：先拿 4 字节固定帧头，再根据长度字段决定后续还要读多少。
 * 这样可以在不浪费缓冲区的前提下，确保调用方拿到的是完整帧。
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
 * @brief 获取 UART 传输默认配置。
 * @param[out] out_config 输出配置结构体指针，不能为空。
 * @note 默认配置绑定到 STM32 工程里现成的 huart2，同时默认不开启任何收发前清空策略。
 */
void m9p_uart_transport_get_default_config(struct m9p_uart_transport_config *out_config)
{
    if (out_config == NULL) {
        return;
    }

    out_config->uart = &huart2;
    out_config->io_timeout_ms = M9P_UART_TRANSPORT_DEFAULT_TIMEOUT_MS;
    out_config->flush_before_request = false;
    out_config->flush_before_receive = false;
}

/**
 * @brief 初始化 UART 传输上下文（STM32 实现）。
 * @param[in,out] transport 传输上下文指针。
 * @param[in] config 配置指针。
 * @retval 0 成功
 * @retval <0 错误码
 * @note 这里不初始化 UART 外设本身；UART 的底层初始化仍由 CubeMX 生成的 MX_USART2_UART_Init 完成。
 */
int m9p_uart_transport_init(struct m9p_uart_transport *transport,
                            const struct m9p_uart_transport_config *config)
{
    if (transport == NULL || config == NULL || config->uart == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    transport->config = *config;
    transport->tx_busy = false;
    transport->rx_busy = false;
    transport->exchange_busy = false;
    transport->initialized = true;
    return 0;
}

/**
 * @brief 反初始化 UART 传输上下文（STM32 实现）。
 * @param[in,out] transport 传输上下文指针。
 * @note 不会关闭或反初始化 HAL UART 外设，只会清零本 transport 内部状态。
 */
void m9p_uart_transport_deinit(struct m9p_uart_transport *transport)
{
    if (transport == NULL) {
        return;
    }

    memset(transport, 0, sizeof(*transport));
}

/**
 * @brief 发送一整帧原始 UART 数据（STM32 实现）。
 * @param[in,out] transport 传输上下文指针。
 * @param[in] tx_data 待发送帧数据。
 * @param[in] tx_len 帧长度。
 * @retval 0 成功
 * @retval <0 错误码
 * @note 该接口只占用 TX 方向；因此 raw send_frame 与 raw receive_frame 可以并行发生。
 */
int m9p_uart_transport_send_frame(struct m9p_uart_transport *transport,
                                  const uint8_t *tx_data,
                                  size_t tx_len)
{
    int ret;

    if (transport == NULL || !transport->initialized || tx_data == NULL || tx_len == 0u) {
        return -(int)M9P_ERR_EINVAL;
    }

    ret = transport_claim_tx(transport);
    if (ret < 0) {
        return ret;
    }

    ret = send_frame_locked(transport, tx_data, tx_len);
    transport_release_tx(transport);
    return ret;
}

/**
 * @brief 接收一整帧原始 UART 数据（STM32 实现）。
 * @param[in,out] transport 传输上下文指针。
 * @param[out] rx_data 接收缓冲区。
 * @param[in] rx_cap 接收缓冲区容量。
 * @param[out] rx_len 实际接收长度。
 * @retval 0 成功
 * @retval <0 错误码
 * @note 该接口只占用 RX 方向；因此 raw receive_frame 与 raw send_frame 可以并行发生。
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

    ret = transport_claim_rx(transport);
    if (ret < 0) {
        return ret;
    }

    if (transport->config.flush_before_receive) {
        ret = drain_rx_fifo(transport);
        if (ret < 0) {
            transport_release_rx(transport);
            return ret;
        }
    }

    ret = receive_frame_locked(transport, rx_data, rx_cap, rx_len);
    transport_release_rx(transport);
    return ret;
}

/**
 * @brief 主动发送一帧请求，并等待一帧响应（STM32 实现）。
 * @param[in] transport_ctx 传输上下文指针（struct m9p_uart_transport *）。
 * @param[in] tx_data 待发送请求帧。
 * @param[in] tx_len 请求帧长度。
 * @param[out] rx_data 接收响应帧缓冲区。
 * @param[in] rx_cap 接收缓冲区容量。
 * @param[out] rx_len 实际接收长度。
 * @retval 0 成功
 * @retval <0 错误码
 * @note 这是单事务 helper：它会同时占住 exchange/TX/RX，确保一轮 request/response
 *       不会和其他 raw API 交织。真正的双向并发调度仍需要上层 dispatcher。
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
    bool rx_claimed = false;
    bool tx_claimed = false;

    if (transport == NULL || !transport->initialized || tx_data == NULL || tx_len == 0u ||
        rx_data == NULL || rx_len == NULL || rx_cap < M9P_FRAME_OVERHEAD) {
        return -(int)M9P_ERR_EINVAL;
    }

    *rx_len = 0u;

    ret = transport_claim_exchange(transport);
    if (ret < 0) {
        return ret;
    }

    ret = transport_claim_rx(transport);
    if (ret < 0) {
        goto out;
    }
    rx_claimed = true;

    ret = transport_claim_tx(transport);
    if (ret < 0) {
        goto out;
    }
    tx_claimed = true;

    if (transport->config.flush_before_request) {
        ret = drain_rx_fifo(transport);
        if (ret < 0) {
            goto out;
        }
    }

    ret = send_frame_locked(transport, tx_data, tx_len);
    if (ret < 0) {
        goto out;
    }

    ret = receive_frame_locked(transport, rx_data, rx_cap, rx_len);

out:
    if (tx_claimed) {
        transport_release_tx(transport);
    }
    if (rx_claimed) {
        transport_release_rx(transport);
    }
    transport_release_exchange(transport);
    return ret;
}

/**
 * @brief 接收一帧请求、调用处理器并回发响应（STM32 实现）。
 * @param[in,out] transport 传输上下文指针。
 * @param[in] handler 服务端处理器，负责把请求帧转成响应帧。
 * @param[in] server_ctx 回调上下文。
 * @param[out] rx_data 请求帧缓冲区。
 * @param[in] rx_cap 请求帧缓冲区容量。
 * @param[out] rx_len 实际接收的请求帧长度。
 * @param[out] tx_data 响应帧缓冲区。
 * @param[in] tx_cap 响应帧缓冲区容量。
 * @param[out] tx_len 实际发送的响应帧长度。
 * @retval 0 成功
 * @retval <0 错误码
 * @note 这是单事务 helper：它会同时占住 exchange/TX/RX，确保本轮
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
    bool rx_claimed = false;
    bool tx_claimed = false;

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

    ret = transport_claim_exchange(transport);
    if (ret < 0) {
        return ret;
    }

    ret = transport_claim_rx(transport);
    if (ret < 0) {
        goto out;
    }
    rx_claimed = true;

    ret = transport_claim_tx(transport);
    if (ret < 0) {
        goto out;
    }
    tx_claimed = true;

    if (transport->config.flush_before_receive) {
        ret = drain_rx_fifo(transport);
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
    if (tx_claimed) {
        transport_release_tx(transport);
    }
    if (rx_claimed) {
        transport_release_rx(transport);
    }
    transport_release_exchange(transport);
    return ret;
}

/**
 * @brief 初始化全局默认 UART 传输实例。
 * @retval 0 成功
 * @retval <0 错误码
 * @note 该接口只初始化 transport 包装层，不会代替 CubeMX 生成的外设初始化入口。
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