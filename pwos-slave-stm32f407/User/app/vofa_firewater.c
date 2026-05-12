/**
 * @file vofa_firewater.c
 * @author hb (huobin92@gmail.com)
 * @brief 上位机 vofa 串口调试工具的 firewater 通信协议实现
 * @version 0.1
 * @date 2026-04-21
 * 
 * @copyright Copyright (c) 2026
 * 
 */

#include "vofa_firewater.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>

#define VOFA_UART_TIMEOUT_MS 100U

/**
 * @brief 发送字符串缓冲区到 UART
 * 
 * @param buffer 
 * @param length 
 * @return int 
 */
static int vofa_uart_send_buffer(const char *buffer, uint16_t length) {
    if ((buffer == NULL) || (length == 0U)) {
        return -1;
    }

    return (HAL_UART_Transmit(&huart2, (const uint8_t *)buffer, length,
                              VOFA_UART_TIMEOUT_MS) == HAL_OK)
               ? 0
               : -1;
}

/**
 * @brief 发送文本到 VOFA
 * 
 * @param text 要发送的文本
 * @return int 0 表示成功，非 0 表示失败
 */
int vofa_firewater_send_text(const char *text) {
    char frame[192];
    int length;

    if (text == NULL) {
        return -1;
    }

    length = snprintf(frame, sizeof(frame), "%s\n", text);
    if ((length <= 0) || ((size_t)length >= sizeof(frame))) {
        return -1;
    }

    return vofa_uart_send_buffer(frame, (uint16_t)length);
}

int vofa_firewater_send_fs_report(const FS_SelfTestReport *report,
                                  uint32_t uptime_ms) {
    char frame[256];
    int pass;
    int length;

    if (report == NULL) {
        return -1;
    }

    pass = (report->init_status == 0) &&
           (report->mkdir_status == 0) &&
           (report->write_status == 0) &&
           (report->stat_status == 0) &&
           (report->read_status == 0) &&
           (report->dir_status == 0) &&
           (report->compare_status == 0);

    length = snprintf(
        frame, sizeof(frame),
        "fs:%d,%lu,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%lu,%lu,%lu,%lu\n",
        pass,
        (unsigned long)report->run_count,
        (long)report->init_status,
        (long)report->mkdir_status,
        (long)report->write_status,
        (long)report->stat_status,
        (long)report->read_status,
        (long)report->dir_status,
        (long)report->compare_status,
        (unsigned long)report->file_size,
        (unsigned long)report->bytes_written,
        (unsigned long)report->bytes_read,
        (unsigned long)uptime_ms);
    if ((length <= 0) || ((size_t)length >= sizeof(frame))) {
        return -1;
    }

    return vofa_uart_send_buffer(frame, (uint16_t)length);
}
