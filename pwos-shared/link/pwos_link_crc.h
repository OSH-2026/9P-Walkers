#ifndef PWOS_LINK_CRC_H
#define PWOS_LINK_CRC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 计算 CRC-16/CCITT-FALSE。
 *
 * 参数固定为：
 * - poly   = 0x1021
 * - init   = 0xFFFF
 * - refin  = false
 * - refout = false
 * - xorout = 0x0000
 *
 * 这个函数不依赖硬件 CRC 外设，PC 单测、STM32 和 ESP32 都应得到同样结果。
 * 当 data == NULL 且 len == 0 时返回空输入 CRC，也就是 0xFFFF。
 */
uint16_t pwos_link_crc16_ccitt_false(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
