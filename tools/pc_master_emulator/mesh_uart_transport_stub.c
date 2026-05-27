/**
 * @file mesh_uart_transport_stub.c
 * @brief mesh_uart_transport 接口的空桩实现
 *
 * 此文件为 tools/pc_master_emulator 提供一个 mesh_uart_transport 的桩实现，
 * 所有函数均返回 -MESH_ERR_INVALID_STATE，表示传输层尚未就绪。
 *
 * pc_master_emulator 实际通过 main.c 中的 read_exact / write_all 直接操作
 * 串口 fd，不经过 mesh_uart_transport 层，因此此桩函数永远不会被调用。
 *
 * 此文件仅用于链接：master mesh transport manager 依赖 mesh_uart_transport
 * 的函数签名，如果缺少此编译单元会导致链接错误。真实传输逻辑见 main.c 中的
 * pc_mesh_send_frame 和 pc_mesh_receive_frame。
 *
 * @note 不要将此文件用于生产代码。此为联调桩，非真实传输实现。
 */

#include "mesh_uart_transport.h"

#include <string.h>

/**
 * @brief 获取默认传输层配置（桩）
 *
 * 将 out_config 所有字段清零，io_timeout_ms 设为 MESH_UART_TRANSPORT_DEFAULT_TIMEOUT_MS。
 *
 * @param[out] out_config 输出配置结构指针，调用方保证非 NULL
 */
void mesh_uart_transport_get_default_config(struct mesh_uart_transport_config *out_config)
{
    if (out_config != NULL) {
        memset(out_config, 0, sizeof(*out_config));
        out_config->io_timeout_ms = MESH_UART_TRANSPORT_DEFAULT_TIMEOUT_MS;
    }
}

/**
 * @brief 初始化传输层（桩）
 *
 * 清零传输结构并返回 -MESH_ERR_INVALID_STATE，表示传输层不可用。
 *
 * @param[in] transport 传输层实例，NULL 时返回错误码
 * @param[in] config    配置参数（未使用）
 * @return -MESH_ERR_INVALID_STATE（始终）
 */
int mesh_uart_transport_init(struct mesh_uart_transport *transport, const struct mesh_uart_transport_config *config)
{
    (void)config;
    if (transport == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    memset(transport, 0, sizeof(*transport));
    return -(int)MESH_ERR_INVALID_STATE;
}

/**
 * @brief 反初始化传输层（桩）
 *
 * 仅将传输结构清零，不释放任何底层资源（因为没有真实资源被分配）。
 *
 * @param[in] transport 传输层实例，NULL 时为空操作
 */
void mesh_uart_transport_deinit(struct mesh_uart_transport *transport)
{
    if (transport != NULL) {
        memset(transport, 0, sizeof(*transport));
    }
}

/**
 * @brief 发送一帧数据（桩）
 *
 * 所有参数均被忽略，返回 -MESH_ERR_INVALID_STATE。
 *
 * @param[in] transport 传输层实例
 * @param[in] tx_data   待发送数据
 * @param[in] tx_len     数据长度
 * @return -MESH_ERR_INVALID_STATE（始终）
 */
int mesh_uart_transport_send_frame(struct mesh_uart_transport *transport, const uint8_t *tx_data, size_t tx_len)
{
    (void)transport;
    (void)tx_data;
    (void)tx_len;
    return -(int)MESH_ERR_INVALID_STATE;
}

/**
 * @brief 接收一帧数据（桩）
 *
 * 将 *rx_len 设为 0 并返回 -MESH_ERR_INVALID_STATE。
 *
 * @param[in]  transport 传输层实例
 * @param[out] rx_data   接收缓冲区
 * @param[in]  rx_cap    接收缓冲区容量
 * @param[out] rx_len    实际接收字节数输出指针，置为 0
 * @return -MESH_ERR_INVALID_STATE（始终）
 */
int mesh_uart_transport_receive_frame(
    struct mesh_uart_transport *transport,
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
    return -(int)MESH_ERR_INVALID_STATE;
}

/**
 * @brief 初始化默认传输层单例（桩）
 *
 * 返回 -MESH_ERR_INVALID_STATE。
 *
 * @return -MESH_ERR_INVALID_STATE（始终）
 */
int mesh_uart_transport_init_default(void)
{
    return -(int)MESH_ERR_INVALID_STATE;
}

/**
 * @brief 获取默认传输层单例指针（桩）
 *
 * 永远返回 NULL。
 *
 * @return NULL
 */
struct mesh_uart_transport *mesh_uart_transport_default(void)
{
    return NULL;
}