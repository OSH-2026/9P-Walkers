/**
 * @file vofa_firewater.h
 * @author hb (huobin92@gmail.com)
 * @brief 上位机 vofa 串口调试工具的 firewater 通信协议实现
 * @version 0.1
 * @date 2026-04-21
 * 
 * @copyright Copyright (c) 2026
 * 
 */

#ifndef VOFA_FIREWATER_H
#define VOFA_FIREWATER_H

#include <stdint.h>

#include "fs_selftest.h"

#ifdef __cplusplus
extern "C" {
#endif

int vofa_firewater_send_text(const char *text);
int vofa_firewater_send_fs_report(const FS_SelfTestReport *report,
                                  uint32_t uptime_ms);

#ifdef __cplusplus
}
#endif

#endif
