#ifndef PWOS_DIST_INFERENCE_SERVICE_H
#define PWOS_DIST_INFERENCE_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 分布式推理服务
 *
 * 把本地 LLM 推理通过虚拟文件 /llm/prompt、/llm/result、/llm/status
 * 暴露给 host_rpc，使任意 ESP 主机都能远程提交 prompt 并读取结果。
 *
 * 写 /llm/prompt → 触发本地推理（prompt 文本写入即提交）
 * 读 /llm/result → 读取已生成的文本（分块读，offset 从 0 开始）
 * 读 /llm/status → 读取推理状态快照（JSON 格式）
 */

/* 初始化分布式推理服务，绑定本地 inference_runtime。 */
int pwos_dist_inference_service_init(void);

/*
 * 尝试处理虚拟路径读写。
 * 路径以 "/llm/" 开头时由本服务处理，返回 0 表示成功。
 * 不匹配时返回 -1（M9P_ERR_ENOENT），让调用方继续查找其他后端。
 */
int pwos_dist_inference_service_read(
    const char *path,
    uint8_t *out,
    uint16_t *in_out_len,
    uint32_t deadline_ms);

int pwos_dist_inference_service_write(
    const char *path,
    const uint8_t *data,
    uint16_t data_len,
    uint16_t *out_written,
    uint32_t deadline_ms);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_DIST_INFERENCE_SERVICE_H */

