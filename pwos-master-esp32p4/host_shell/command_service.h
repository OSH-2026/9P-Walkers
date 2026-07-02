/* ========================================================================
 * 命令服务 —— 公共头文件
 *
 * 提供类 Unix Shell 的命令行解释器，支持:
 *   - 文件操作: ls / cat / stat / echo
 *   - RPC 调用: rpc / stream / notify
 *   - Job 管理: job (caps/submit/list/status/result/cancel/retry)
 *   - LLM 推理: llm (prompt/@host/status/result)
 *   - 故障注入: fault (测试用)
 *   - 系统诊断: mesh / sessions / hosts / host / net
 *
 * 设计为纯 C，依赖注入式架构 (所有 I/O 通过函数指针回调)。
 * ======================================================================== */

#ifndef PWOS_COMMAND_SERVICE_H
#define PWOS_COMMAND_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "mini9p_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * 第一节: 配置常量
 * ======================================================================== */

#define PWOS_COMMAND_MAX_LINE            256u   /* 输入命令行最大字节数           */
#define PWOS_COMMAND_DEFAULT_DEADLINE_MS 1200u  /* 默认 I/O 超时 (毫秒)          */

/* RPC 调用模式 */
#define PWOS_COMMAND_RPC_CALL    0u   /* 同步请求-响应                         */
#define PWOS_COMMAND_RPC_ONEWAY  1u   /* 单向通知 (不等待响应)                   */
#define PWOS_COMMAND_RPC_STREAM  2u   /* 流式请求 (分块响应)                    */

/* LLM 操作模式 */
#define PWOS_LLM_MODE_SUBMIT  0u      /* 提交 prompt 开始推理                   */
#define PWOS_LLM_MODE_RESULT  1u      /* 读取已生成的文本                       */
#define PWOS_LLM_MODE_STATUS  2u      /* 读取推理状态 JSON                      */

/* ========================================================================
 * 第二节: 回调函数类型 (依赖注入接口)
 * ======================================================================== */

/**
 * 读取虚拟文件内容。
 * @param in_out_len  输入: 缓冲区大小; 输出: 实际读取字节数
 */
typedef int (*pwos_command_read_fn)(
    void       *ctx,
    const char *path,
    uint8_t    *buf,
    uint16_t   *in_out_len,
    uint32_t    deadline_ms);

/**
 * 写入虚拟文件。
 * @param out_written  输出: 实际写入字节数
 */
typedef int (*pwos_command_write_fn)(
    void          *ctx,
    const char    *path,
    const uint8_t *data,
    uint16_t       len,
    uint16_t      *out_written,
    uint32_t       deadline_ms);

/**
 * 列出目录内容。
 * @param out_count  输出: 实际条目数
 */
typedef int (*pwos_command_list_fn)(
    void              *ctx,
    const char        *path,
    struct m9p_dirent *entries,
    size_t             max_entries,
    size_t            *out_count,
    uint32_t           deadline_ms);

/**
 * 获取文件属性。
 */
typedef int (*pwos_command_stat_fn)(
    void           *ctx,
    const char     *path,
    struct m9p_stat *out_stat,
    uint32_t        deadline_ms);

/**
 * 发起 RPC 调用 (call / stream / oneway 三种模式)。
 *
 * @param mode         PWOS_COMMAND_RPC_CALL / ONEWAY / STREAM
 * @param response     响应缓冲区 (ONEWAY 模式为 NULL)
 * @param in_out_response_len  输入: 缓冲区大小; 输出: 响应字节数
 * @param out_status   远程 RPC 状态码 (ONEWAY 模式为 NULL)
 * @param out_chunk_count  流式分块数 (仅 STREAM 模式)
 */
typedef int (*pwos_command_rpc_fn)(
    void          *ctx,
    const char    *target,
    const char    *service,
    const char    *method,
    const uint8_t *payload,
    uint16_t       payload_len,
    uint32_t       deadline_ms,
    uint8_t        mode,
    uint8_t       *response,
    uint16_t      *in_out_response_len,
    uint16_t      *out_status,
    uint16_t      *out_chunk_count);

/**
 * 执行 Job 管理命令 (caps/submit/list/status/result/cancel/retry)。
 *
 * @param args      子命令及参数
 * @param out_len   输出: 实际写入字节数
 */
typedef int (*pwos_command_job_fn)(
    void       *ctx,
    const char *args,
    char       *output,
    size_t      output_cap,
    size_t     *out_len,
    uint32_t    deadline_ms);

/**
 * LLM 推理操作 (统一入口，通过 mode 区分操作)。
 *
 * @param hostname  NULL=本地, 非NULL=远程主机名
 * @param mode      PWOS_LLM_MODE_SUBMIT / RESULT / STATUS
 * @param prompt    仅 SUBMIT 模式使用 (可为 NULL)
 * @param out_len   输出: 实际写入字节数
 */
typedef int (*pwos_command_llm_fn)(
    void       *ctx,
    const char *hostname,
    uint8_t     mode,
    const char *prompt,
    uint8_t    *output,
    size_t      output_cap,
    size_t     *out_len,
    uint32_t    deadline_ms);

/* ========================================================================
 * 第三节: 服务配置 & 运行时结构
 * ======================================================================== */

/** 命令服务配置 (所有 I/O 通过回调注入) */
typedef struct {
    void                  *io_ctx;                /* 回调上下文 (传给所有回调)    */
    pwos_command_read_fn   read_path;             /* 文件读回调                   */
    pwos_command_write_fn  write_path;            /* 文件写回调                   */
    pwos_command_list_fn   list;                  /* 目录列表回调                  */
    pwos_command_stat_fn   stat;                  /* 文件属性回调                  */
    pwos_command_rpc_fn    rpc;                   /* RPC 回调 (可为 NULL)          */
    pwos_command_job_fn    job;                   /* Job 回调 (可为 NULL)          */
    pwos_command_llm_fn    llm;                   /* LLM 回调 (可为 NULL)          */
    uint32_t               default_deadline_ms;   /* 默认超时 (0 = 使用全局默认)   */
} pwos_command_service_config_t;

/** 命令服务运行时状态 */
typedef struct {
    pwos_command_service_config_t config;   /* 配置副本                     */
    uint32_t executed;                      /* 累计执行命令数                */
    uint32_t failed;                        /* 累计失败命令数                */
    uint32_t truncated;                     /* 累计输出截断次数               */
} pwos_command_service_t;

/* ========================================================================
 * 第四节: 公开 API
 * ======================================================================== */

/**
 * 初始化命令服务 (纯 C, 零堆分配)。
 * 回调至少需要 read_path / write_path / list / stat。
 *
 * @return 0=成功, -M9P_ERR_EINVAL=参数无效
 */
int pwos_command_service_init(
    pwos_command_service_t              *service,
    const pwos_command_service_config_t *config);

/**
 * 执行一行命令。
 *
 * 处理流程:
 *   1. 拷贝输入行到内部缓冲区 (防止修改原始字符串)
 *   2. 分割命令名与参数
 *   3. 查找命令表 → 调用对应执行函数
 *   4. 更新统计计数器 (executed / failed / truncated)
 *
 * @param line        输入命令行 (null 结尾, 最大 PWOS_COMMAND_MAX_LINE-1 字节)
 * @param output      输出缓冲区
 * @param output_cap  输出缓冲区容量
 * @param out_len     输出: 实际输出字节数
 * @return 0=成功, 负数=错误码 (M9P 兼容)
 */
int pwos_command_service_execute(
    pwos_command_service_t *service,
    const char             *line,
    char                   *output,
    size_t                  output_cap,
    size_t                 *out_len);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_COMMAND_SERVICE_H */
