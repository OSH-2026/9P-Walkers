#include "pwos_link_crc.h"

uint16_t pwos_link_crc16_ccitt_false(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    size_t i;

    if (data == NULL && len == 0u) {
        return crc;
    }

    for (i = 0u; i < len; ++i) {
        unsigned int bit;

        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (bit = 0u; bit < 8u; ++bit) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }

    // 返回 uint16_t 类型的 CRC 校验值
    return crc;
}
