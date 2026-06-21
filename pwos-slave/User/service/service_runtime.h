#ifndef PWOS_SERVICE_RUNTIME_H
#define PWOS_SERVICE_RUNTIME_H

#include <stdint.h>

#include "frame_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t initialized;
    uint32_t mini9p_rx;
    uint32_t mini9p_tx;
    uint32_t bad_frames;
    uint32_t unsupported_frames;
    uint32_t server_errors;
    uint32_t tx_failures;
    uint8_t last_src;
    uint8_t last_dst;
    uint8_t last_m9p_type;
    uint16_t last_m9p_tag;
} pwos_service_runtime_stats_t;

/*
 * 初始化本机 service runtime。
 *
 * 当前 M5 第一阶段挂载 local_vfs，只暴露 /、/sys、/sys/health，用于先验证
 * DATA_MINI9P 数据面闭环。后续再切换到 node_vfs，把 /sys/routes、/dev、/fs 接入。
 */
int pwos_service_runtime_init(void);

/*
 * 判断一个本机数据面帧是否应交给 service_task。
 *
 * mesh_ctrl_task 先调用 node_control；只有 node_control 返回未处理的本机 DATA
 * 帧才会走到这里。
 */
int pwos_service_runtime_accepts(const pwos_frame_block_t *block);

/*
 * 处理一帧本机 DATA_MINI9P 请求。
 *
 * 调用者仍拥有 request block，处理完成后由调用者释放。
 */
void pwos_service_runtime_process(pwos_frame_block_t *block);

void pwos_service_runtime_get_stats(pwos_service_runtime_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_SERVICE_RUNTIME_H */
