/* ========================================================================
 * 命令服务 —— 实现
 *
 * 提供类 Unix Shell 的命令行解释器。
 *
 * 架构特点:
 *   - 纯 C 实现，无堆分配 (output_builder 使用调用者提供的缓冲区)
 *   - 依赖注入: 所有 I/O 操作通过 config 中的函数指针回调
 *   - 命令表驱动: pwos_command_service_execute() 中的 if-else 链
 *   - 跨平台: ESP-IDF (FreeRTOS) / POSIX 双兼容
 *
 * 支持的命令 (共 15 个):
 *   文件:   ls / cat / stat / echo
 *   RPC:    rpc / stream / notify
 *   任务:   job
 *   推理:   llm
 *   诊断:   mesh / sessions / hosts / host / net
 *   测试:   fault
 *   交互:   help / clear
 * ======================================================================== */

#ifndef ESP_PLATFORM
#define _POSIX_C_SOURCE 200809L
#endif

#include "command_service.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pwos_rpc_protocol.h"
#include "session_manager.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <time.h>
#endif

/* ========================================================================
 * 第一节: 配置常量
 * ======================================================================== */

#define PWOS_COMMAND_READ_CAP          1024u   /* cat/llm 读取缓冲区大小        */
#define PWOS_COMMAND_LIST_CAP          24u     /* ls 最大列出条目数             */
#define PWOS_COMMAND_RPC_RESPONSE_CAP  PWOS_RPC_MAX_FRAME_LEN  /* RPC 响应缓冲区 */

/* ========================================================================
 * 第二节: 输出构建器 (带截断保护的格式化输出)
 * ======================================================================== */

/**
 * 输出构建器: 将格式化文本追加到固定大小缓冲区。
 * 缓冲区满后自动设置 truncated 标志，后续写入静默丢弃。
 */
typedef struct {
    char   *data;       /* 输出缓冲区 (调用者提供)       */
    size_t  cap;        /* 缓冲区总容量                  */
    size_t  len;        /* 已写入字节数 (不含 null)       */
    uint8_t truncated;  /* 是否发生过截断                */
} output_builder_t;

/**
 * 向构建器追加格式化文本 (printf 风格)。
 * 线程不安全 (调用者负责同步)。
 */
static void output_append(output_builder_t *builder, const char *fmt, ...)
{
    /* 缓冲区已满或无效 → 静默丢弃并标记截断 */
    if (!builder || !builder->data || !builder->cap ||
        builder->len >= builder->cap - 1u) {
        if (builder) builder->truncated = 1u;
        return;
    }

    va_list args;
    va_start(args, fmt);

    int written = vsnprintf(builder->data + builder->len,
                            builder->cap - builder->len,
                            fmt, args);
    va_end(args);

    if (written < 0) return;  /* 格式化错误 */

    /* 判断是否发生截断:
     * vsnprintf 返回"完整字符串需要的字节数(不含null)"。
     * 若 >= 剩余空间，说明输出被截断。 */
    if ((size_t)written >= builder->cap - builder->len) {
        builder->len       = builder->cap - 1u;
        builder->truncated = 1u;
    } else {
        builder->len += (size_t)written;
    }
}

/* ========================================================================
 * 第三节: 字符串工具函数
 * ======================================================================== */

/** 跳过前导空白字符 (空格/Tab)，返回首个非空白字符位置 */
static char *trim_left(char *text)
{
    if (!text) return NULL;
    while (*text == ' ' || *text == '\t') text++;
    return text;
}

/** 原地去除尾部空白字符 (空格/Tab/回车/换行) */
static void trim_right(char *text)
{
    if (!text) return;
    size_t len = strlen(text);
    while (len > 0u &&
           (text[len - 1u] == ' '  || text[len - 1u] == '\t' ||
            text[len - 1u] == '\r' || text[len - 1u] == '\n')) {
        text[--len] = '\0';
    }
}

/**
 * 从命令行字符串中提取下一个空白分隔的 token。
 * 原地修改 *cursor (在 token 末尾写入 '\0')。
 *
 * @param cursor  指向当前解析位置的指针 (会被更新)
 * @return token 指针 (指向 *cursor 缓冲区内部)，无更多 token 时返回 NULL
 */
static char *take_token(char **cursor)
{
    if (!cursor || !*cursor) return NULL;

    char *token = trim_left(*cursor);
    if (*token == '\0') {
        *cursor = token;
        return NULL;
    }

    /* 找到 token 末尾 (下一个空白或字符串结尾) */
    char *end = token;
    while (*end != '\0' && *end != ' ' && *end != '\t') end++;

    if (*end != '\0') *end++ = '\0';  /* 终止 token，end 指向剩余部分 */
    *cursor = end;
    return token;
}

/* ========================================================================
 * 第四节: 错误名称映射
 * ======================================================================== */

/**
 * 将错误码映射为可读字符串。
 * 优先匹配 session 层错误码，其次匹配 9P 协议错误码。
 */
static const char *error_name(int rc)
{
    if (rc <= PWOS_SESSION_ERR_NO_ROUTE && rc >= PWOS_SESSION_ERR_STALE_BOOT)
        return pwos_session_error_name(rc);

    if (rc < 0 && -rc <= (int)UINT16_MAX)
        return m9p_error_name((uint16_t)(-rc));

    return "io_error";
}

/* ========================================================================
 * 第五节: 跨平台延时
 * ======================================================================== */

static void command_sleep_ms(uint32_t delay_ms)
{
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
#else
    struct timespec delay = {
        .tv_sec  = (time_t)(delay_ms / 1000u),
        .tv_nsec = (long)(delay_ms % 1000u) * 1000000L,
    };
    (void)nanosleep(&delay, NULL);
#endif
}

/* ========================================================================
 * 第六节: 文件操作命令 (cat / ls / stat / echo)
 * ======================================================================== */

/**
 * cat <path> —— 读取并显示虚拟文件内容。
 */
static int execute_cat(pwos_command_service_t *service,
                       const char *path, output_builder_t *output)
{
    if (!path || !path[0]) {
        output_append(output, "usage: cat <path>\r\n");
        return -(int)M9P_ERR_EINVAL;
    }

    uint8_t  buf[PWOS_COMMAND_READ_CAP + 1u];
    uint16_t len = PWOS_COMMAND_READ_CAP;
    int rc = service->config.read_path(service->config.io_ctx, path,
                                       buf, &len,
                                       service->config.default_deadline_ms);
    if (rc != 0) {
        output_append(output, "cat: %s (%d)\r\n", error_name(rc), rc);
        return rc;
    }

    /* 确保以换行结尾，方便终端显示 */
    buf[len] = '\0';
    output_append(output, "%s", (const char *)buf);
    if (len == 0u || buf[len - 1u] != (uint8_t)'\n')
        output_append(output, "\r\n");
    return 0;
}

/**
 * ls [path] —— 列出目录内容。默认 path = "/"。
 */
static int execute_ls(pwos_command_service_t *service,
                      const char *path, output_builder_t *output)
{
    if (!path || !path[0]) path = "/";

    struct m9p_dirent entries[PWOS_COMMAND_LIST_CAP];
    memset(entries, 0, sizeof(entries));

    size_t count = 0u;
    int rc = service->config.list(service->config.io_ctx, path,
                                  entries, PWOS_COMMAND_LIST_CAP, &count,
                                  service->config.default_deadline_ms);
    if (rc != 0) {
        output_append(output, "ls: %s (%d)\r\n", error_name(rc), rc);
        return rc;
    }

    for (size_t i = 0u; i < count; i++) {
        /* 目录项添加 '/' 后缀 */
        output_append(output, "%s%s\r\n", entries[i].name,
                      (entries[i].qid.type & M9P_QID_DIR) ? "/" : "");
    }
    if (count == 0u) output_append(output, "(empty)\r\n");
    return 0;
}

/**
 * stat <path> —— 显示虚拟文件属性 (name/type/size/qid)。
 */
static int execute_stat(pwos_command_service_t *service,
                        const char *path, output_builder_t *output)
{
    if (!path || !path[0]) {
        output_append(output, "usage: stat <path>\r\n");
        return -(int)M9P_ERR_EINVAL;
    }

    struct m9p_stat st;
    memset(&st, 0, sizeof(st));

    int rc = service->config.stat(service->config.io_ctx, path, &st,
                                  service->config.default_deadline_ms);
    if (rc != 0) {
        output_append(output, "stat: %s (%d)\r\n", error_name(rc), rc);
        return rc;
    }

    output_append(output,
        "name=%s type=0x%02x size=%lu qid=%lu version=%u\r\n",
        st.name, st.qid.type,
        (unsigned long)st.size, (unsigned long)st.qid.object_id,
        st.qid.version);
    return 0;
}

/**
 * echo <text> > <path> —— 将文本写入虚拟文件。
 *
 * 解析: 以 '>' 为分隔符，左边为文本，右边为目标路径。
 * 原地修改 args 字符串 (在 '>' 处插入 '\0')。
 */
static int execute_echo(pwos_command_service_t *service,
                        char *args, output_builder_t *output)
{
    if (!args) {
        output_append(output, "usage: echo <text> > <path>\r\n");
        return -(int)M9P_ERR_EINVAL;
    }

    /* 以 '>' 分割文本和目标路径 */
    char *sep = strchr(args, '>');
    if (!sep) {
        output_append(output, "usage: echo <text> > <path>\r\n");
        return -(int)M9P_ERR_EINVAL;
    }
    *sep = '\0';

    char *text = trim_left(args);
    trim_right(text);
    char *path = trim_left(sep + 1);
    trim_right(path);

    size_t text_len = strlen(text);
    if (!path[0] || text_len > UINT16_MAX) {
        output_append(output, "usage: echo <text> > <path>\r\n");
        return -(int)M9P_ERR_EINVAL;
    }

    uint16_t written = 0u;
    int rc = service->config.write_path(service->config.io_ctx, path,
                                        (const uint8_t *)text,
                                        (uint16_t)text_len, &written,
                                        service->config.default_deadline_ms);
    if (rc != 0) {
        output_append(output, "echo: %s (%d)\r\n", error_name(rc), rc);
        return rc;
    }
    output_append(output, "wrote %u byte(s)\r\n", written);
    return 0;
}

/* ========================================================================
 * 第七节: LLM 推理命令 (llm)
 *
 * 用法:
 *   llm <prompt>              → 本地推理
 *   llm @<hostname> <prompt>  → 远程推理
 *   llm status                → 本地状态
 *   llm result                → 本地结果
 *   llm @<host> status        → 远程状态
 *   llm @<host> result        → 远程结果
 *
 * 工作流程:
 *   1. 提交 prompt → 异步推理
 *   2. 轮询 status (最多 30s, 每秒一次)
 *   3. 读取 result → 输出
 * ======================================================================== */

static int execute_llm(pwos_command_service_t *service,
                       char *args, output_builder_t *output)
{
    /* LLM 是可选能力；没有推理后端的主机仍可使用其余命令 */
    if (!service->config.llm) {
        output_append(output, "llm: unavailable\r\n");
        return -(int)M9P_ERR_ENOTSUP;
    }

    if (!args || !args[0]) {
        output_append(output,
            "usage: llm <prompt>\r\n"
            "       llm @<host> <prompt>\r\n"
            "       llm status | result | @<host> status | @<host> result\r\n");
        return -(int)M9P_ERR_EINVAL;
    }

    /* ---- 解析 @hostname 前缀 ---- */
    char *hostname = NULL;
    char *prompt;

    if (args[0] == '@') {
        char *space = strchr(args, ' ');
        if (!space) {
            output_append(output, "llm: missing prompt after @hostname\r\n");
            return -(int)M9P_ERR_EINVAL;
        }
        *space   = '\0';
        hostname = args + 1;             /* 跳过 '@' */
        prompt   = trim_left(space + 1);
    } else {
        prompt = args;
    }

    if (!prompt[0]) {
        output_append(output, "llm: empty prompt\r\n");
        return -(int)M9P_ERR_EINVAL;
    }

    /* ---- 处理 status / result 子命令 ---- */
    uint8_t  buf[PWOS_COMMAND_READ_CAP];
    size_t   out_len = 0u;
    uint32_t deadline = service->config.default_deadline_ms;
    int      rc;

    if (strcmp(prompt, "status") == 0) {
        rc = service->config.llm(service->config.io_ctx, hostname,
                                 PWOS_LLM_MODE_STATUS, NULL,
                                 buf, sizeof(buf), &out_len, deadline);
        if (rc != 0) {
            output_append(output, "llm status: %s (%d)\r\n", error_name(rc), rc);
            return rc;
        }
        output_append(output, "%.*s\r\n", (int)out_len, (char *)buf);
        return 0;
    }

    if (strcmp(prompt, "result") == 0) {
        rc = service->config.llm(service->config.io_ctx, hostname,
                                 PWOS_LLM_MODE_RESULT, NULL,
                                 buf, sizeof(buf), &out_len, deadline);
        if (rc != 0) {
            output_append(output, "llm result: %s (%d)\r\n", error_name(rc), rc);
            return rc;
        }
        output_append(output, "%.*s\r\n", (int)out_len, (char *)buf);
        return 0;
    }

    /* ---- 提交 prompt ---- */
    rc = service->config.llm(service->config.io_ctx, hostname,
                             PWOS_LLM_MODE_SUBMIT, prompt,
                             buf, sizeof(buf), &out_len, deadline);
    if (rc != 0) {
        output_append(output, "llm submit: %s (%d)\r\n", error_name(rc), rc);
        return rc;
    }
    output_append(output, "llm: prompt submitted to %s, generating...\r\n",
                  hostname ? hostname : "local");

    /* ---- 轮询等待推理完成 (最多 30 秒，每秒一次) ---- */
    for (int wait_ms = 0; wait_ms < 30000; wait_ms += 1000) {
        command_sleep_ms(1000u);

        rc = service->config.llm(service->config.io_ctx, hostname,
                                 PWOS_LLM_MODE_STATUS, NULL,
                                 buf, sizeof(buf), &out_len, deadline);
        if (rc != 0) continue;

        /* 解析 status JSON 中的 "state" 字段 */
        char *p = strstr((char *)buf, "\"state\":");
        int state = p ? atoi(p + 8 /* 跳过 "state": */) : -1;

        if (state == 5) break;         /* PWOS_INFERENCE_DONE */
        if (state == 6) {              /* PWOS_INFERENCE_ERROR */
            output_append(output, "llm: inference error\r\n");
            return -(int)M9P_ERR_EIO;
        }
    }

    /* ---- 读取最终结果 ---- */
    rc = service->config.llm(service->config.io_ctx, hostname,
                             PWOS_LLM_MODE_RESULT, NULL,
                             buf, sizeof(buf), &out_len, deadline);
    if (rc != 0) {
        output_append(output, "llm result: %s (%d)\r\n", error_name(rc), rc);
        return rc;
    }
    output_append(output, "%.*s\r\n", (int)out_len, (char *)buf);
    return 0;
}

/* ========================================================================
 * 第八节: 故障注入命令 (fault) —— 仅开发/测试用
 *
 * 用法: fault <mcuN> <action>
 *   action: status / clear / drop / delay / corrupt / down / recover / reboot-self
 * ======================================================================== */

static int execute_fault(pwos_command_service_t *service,
                         char *args, output_builder_t *output)
{
    /* ---- 解析 target 和 action ---- */
    char *target = trim_left(args);
    char *sep    = target;
    while (*sep && *sep != ' ' && *sep != '\t') sep++;

    if (!*sep) {
        output_append(output,
            "usage: fault <mcuN> <status|clear|drop|delay|corrupt|down|recover|reboot-self ...>\r\n");
        return -(int)M9P_ERR_EINVAL;
    }
    *sep++ = '\0';
    char *action = trim_left(sep);
    trim_right(action);

    /* 安全校验: target 不能包含 '/' (防路径穿越) */
    char path[64];
    if (!target[0] || strchr(target, '/') || !action[0] ||
        snprintf(path, sizeof(path), "/%s/sys/fault", target) >= (int)sizeof(path)) {
        output_append(output, "fault: invalid target or action\r\n");
        return -(int)M9P_ERR_EINVAL;
    }

    /* status 子命令 = 读取 /<target>/sys/fault */
    if (strcmp(action, "status") == 0)
        return execute_cat(service, path, output);

    /* 其他 action = 写入 /<target>/sys/fault */
    size_t action_len = strlen(action);
    if (action_len > UINT16_MAX) return -(int)M9P_ERR_EINVAL;

    uint16_t written = 0u;
    int rc = service->config.write_path(service->config.io_ctx, path,
                                        (const uint8_t *)action,
                                        (uint16_t)action_len, &written,
                                        service->config.default_deadline_ms);
    if (rc != 0) {
        output_append(output, "fault: %s (%d)\r\n", error_name(rc), rc);
        return rc;
    }
    output_append(output, "fault configured on %s\r\n", target);
    return 0;
}

/* ========================================================================
 * 第九节: RPC 命令 (rpc / stream / notify)
 *
 * 用法:
 *   rpc    <mcuN> <service.method> [payload] [--deadline=<ms>]
 *   stream <mcuN> <service.method> [payload] [--deadline=<ms>]
 *   notify <mcuN> <service.method> [payload] [--deadline=<ms>]
 * ======================================================================== */

/**
 * 解析 payload 末尾的 "--deadline=<ms>" 选项。
 * 原地修改 payload 字符串 (移除 deadline 选项)。
 *
 * @param out_deadline_ms  输出: 解析到的 deadline (若未找到则用默认值)
 * @return 0=成功, -1=格式错误
 */
static int parse_rpc_deadline(char *payload, uint32_t default_deadline_ms,
                              uint32_t *out_deadline_ms)
{
    if (!payload || !out_deadline_ms) return -1;

    static const char prefix[] = "--deadline=";
    trim_right(payload);

    /* 找到最后一个独立的 --deadline= 选项 (前面是空白或开头) */
    char *opt = strstr(payload, prefix);
    while (opt && opt != payload && opt[-1] != ' ' && opt[-1] != '\t')
        opt = strstr(opt + 1, prefix);

    if (!opt) {
        *out_deadline_ms = default_deadline_ms;
        return 0;  /* 未指定 deadline，用默认值 */
    }

    /* 解析数值 */
    char *end;
    unsigned long value = strtoul(opt + sizeof(prefix) - 1, &end, 10);
    end = trim_left(end);

    if (value == 0u || value > UINT32_MAX || *end != '\0')
        return -1;  /* 格式错误 */

    /* 从 payload 中移除 deadline 选项 (截断字符串) */
    if (opt != payload) {
        char *trim = opt;
        while (trim > payload && (trim[-1] == ' ' || trim[-1] == '\t')) trim--;
        *trim = '\0';
    } else {
        *payload = '\0';
    }

    *out_deadline_ms = (uint32_t)value;
    return 0;
}

/**
 * 执行 RPC 调用 (call / stream / oneway 三种模式统一入口)。
 */
static int execute_rpc(pwos_command_service_t *service, char *args,
                       uint8_t mode, output_builder_t *output)
{
    if (!service->config.rpc) {
        output_append(output, "rpc: unavailable\r\n");
        return -(int)M9P_ERR_ENOTSUP;
    }

    /* ---- 解析参数: target service.method [payload] ---- */
    char *cursor   = args;
    char *target   = take_token(&cursor);
    char *endpoint = take_token(&cursor);

    if (!target || !endpoint) {
        const char *verb = (mode == PWOS_COMMAND_RPC_ONEWAY) ? "notify"
                         : (mode == PWOS_COMMAND_RPC_STREAM) ? "stream" : "rpc";
        output_append(output,
            "usage: %s <mcuN> <service.method> [payload] [--deadline=<ms>]\r\n", verb);
        return -(int)M9P_ERR_EINVAL;
    }

    /* 拆分 service.method */
    char *method = strchr(endpoint, '.');
    if (!method || method == endpoint || !method[1]) {
        output_append(output, "rpc: endpoint must be service.method\r\n");
        return -(int)M9P_ERR_EINVAL;
    }
    *method++ = '\0';

    /* 剩余部分 = payload (可能含 --deadline= 选项) */
    char *payload = trim_left(cursor);

    uint32_t deadline_ms;
    if (parse_rpc_deadline(payload, service->config.default_deadline_ms,
                           &deadline_ms) != 0) {
        output_append(output, "rpc: invalid deadline\r\n");
        return -(int)M9P_ERR_EINVAL;
    }

    size_t payload_len = strlen(payload);
    if (payload_len > UINT16_MAX) return -(int)M9P_ERR_EMSIZE;

    /* ---- 发起 RPC ---- */
    uint8_t  response[PWOS_COMMAND_RPC_RESPONSE_CAP];
    uint16_t response_len = sizeof(response);
    uint16_t status       = PWOS_RPC_STATUS_OK;
    uint16_t chunk_count  = 0u;

    int rc = service->config.rpc(
        service->config.io_ctx, target, endpoint, method,
        (const uint8_t *)payload, (uint16_t)payload_len, deadline_ms, mode,
        (mode == PWOS_COMMAND_RPC_ONEWAY) ? NULL : response,
        (mode == PWOS_COMMAND_RPC_ONEWAY) ? NULL : &response_len,
        (mode == PWOS_COMMAND_RPC_ONEWAY) ? NULL : &status,
        (mode == PWOS_COMMAND_RPC_STREAM)  ? &chunk_count : NULL);

    if (rc != 0) {
        output_append(output, "rpc: %s (%d)\r\n", error_name(rc), rc);
        return rc;
    }

    /* ---- 格式化输出 ---- */
    if (mode == PWOS_COMMAND_RPC_ONEWAY) {
        output_append(output, "queued\r\n");
        return 0;
    }

    if (status != PWOS_RPC_STATUS_OK) {
        output_append(output, "rpc: remote %s (%u)\r\n",
                      pwos_rpc_status_name(status), status);
        return -(int)M9P_ERR_EIO;
    }

    if (response_len > 0u)
        output_append(output, "%.*s", (int)response_len, (const char *)response);
    else
        output_append(output, "ok");
    output_append(output, "\r\n");

    if (mode == PWOS_COMMAND_RPC_STREAM)
        output_append(output, "stream chunks=%u bytes=%u\r\n",
                      chunk_count, response_len);
    return 0;
}

/* ========================================================================
 * 第十节: Job 管理命令 (job)
 *
 * 用法: job <caps|submit|list|status|result|cancel|retry> ...
 * 通过 config.job 回调委托给 job_manager。
 * ======================================================================== */

static int execute_job(pwos_command_service_t *service,
                       const char *args, output_builder_t *output)
{
    if (!service->config.job) {
        output_append(output, "job: unavailable\r\n");
        return -(int)M9P_ERR_ENOTSUP;
    }

    /* 输出缓冲区已满 → 标记截断 */
    if (output->len >= output->cap) {
        output->truncated = 1u;
        return -(int)M9P_ERR_EMSIZE;
    }

    size_t remaining = output->cap - output->len;
    size_t produced  = 0u;
    int rc = service->config.job(service->config.io_ctx, args,
                                 output->data + output->len, remaining,
                                 &produced,
                                 service->config.default_deadline_ms);

    /* 处理截断 vs 成功 */
    if (produced >= remaining) {
        output->len       = output->cap - 1u;
        output->data[output->len] = '\0';
        output->truncated = 1u;
        return (rc == 0) ? -(int)M9P_ERR_EMSIZE : rc;
    }

    output->len += produced;
    output->data[output->len] = '\0';

    if (rc != 0 && produced == 0u)
        output_append(output, "job: error (%d)\r\n", rc);

    return rc;
}

/* ========================================================================
 * 第十一节: 服务初始化
 * ======================================================================== */

int pwos_command_service_init(pwos_command_service_t *service,
                              const pwos_command_service_config_t *config)
{
    /* 必选回调校验: read_path / write_path / list / stat */
    if (!service || !config ||
        !config->read_path || !config->write_path ||
        !config->list || !config->stat) {
        return -(int)M9P_ERR_EINVAL;
    }

    memset(service, 0, sizeof(*service));
    service->config = *config;  /* 拷贝配置 (浅拷贝，回调指针为值拷贝) */

    /* 默认超时回填 */
    if (service->config.default_deadline_ms == 0u)
        service->config.default_deadline_ms = PWOS_COMMAND_DEFAULT_DEADLINE_MS;

    return 0;
}

/* ========================================================================
 * 第十二节: 命令执行入口 (命令表分发)
 * ======================================================================== */

int pwos_command_service_execute(pwos_command_service_t *service,
                                 const char *line,
                                 char *output, size_t output_cap,
                                 size_t *out_len)
{
    /* ---- 参数校验 ---- */
    if (!service || !line || !output || output_cap == 0u || !out_len ||
        strlen(line) >= PWOS_COMMAND_MAX_LINE) {
        return -(int)M9P_ERR_EINVAL;
    }

    /* ---- 初始化输出构建器 ---- */
    output_builder_t builder;
    memset(&builder, 0, sizeof(builder));
    builder.data = output;
    builder.cap  = output_cap;
    output[0]    = '\0';

    /* ---- 拷贝输入行 (避免修改原始字符串) ---- */
    char command_line[PWOS_COMMAND_MAX_LINE];
    memcpy(command_line, line, strlen(line) + 1u);
    trim_right(command_line);

    /* ---- 分割命令名与参数 ---- */
    char *command = trim_left(command_line);
    char *sep     = command;
    while (*sep && *sep != ' ' && *sep != '\t') sep++;

    char *args;
    if (*sep) {
        *sep++ = '\0';
        args   = trim_left(sep);
        trim_right(args);
    } else {
        args = sep;  /* 指向空字符串 */
    }

    /* ---- 命令表分发 (15 个命令) ---- */
    int rc = 0;
    service->executed++;

    if (!command[0]) {
        rc = 0;  /* 空行 */
    } else if (strcmp(command, "help") == 0) {
        output_append(&builder,
            "help  ls [path]  cat <path>  stat <path>\r\n"
            "echo <text> > <path>  mesh  sessions  hosts  net status  host\r\n"
            "rpc <mcuN> <service.method> [payload] [--deadline=<ms>]\r\n"
            "stream <mcuN> <service.method> [payload] [--deadline=<ms>]\r\n"
            "notify <mcuN> <service.method> [payload] [--deadline=<ms>]\r\n"
            "job <caps|submit|list|status|result|cancel|retry> ...\r\n"
            "llm [@host] <prompt|status|result>\r\n"
            "fault <mcuN> <status|clear|drop|delay|corrupt|down|recover|reboot-self ...>\r\n");
    } else if (strcmp(command, "ls") == 0) {
        rc = execute_ls(service, args, &builder);
    } else if (strcmp(command, "cat") == 0) {
        rc = execute_cat(service, args, &builder);
    } else if (strcmp(command, "stat") == 0) {
        rc = execute_stat(service, args, &builder);
    } else if (strcmp(command, "echo") == 0) {
        rc = execute_echo(service, args, &builder);
    } else if (strcmp(command, "fault") == 0) {
        rc = execute_fault(service, args, &builder);
    } else if (strcmp(command, "rpc") == 0) {
        rc = execute_rpc(service, args, PWOS_COMMAND_RPC_CALL, &builder);
    } else if (strcmp(command, "stream") == 0) {
        rc = execute_rpc(service, args, PWOS_COMMAND_RPC_STREAM, &builder);
    } else if (strcmp(command, "notify") == 0) {
        rc = execute_rpc(service, args, PWOS_COMMAND_RPC_ONEWAY, &builder);
    } else if (strcmp(command, "job") == 0) {
        rc = execute_job(service, args, &builder);
    } else if (strcmp(command, "llm") == 0) {
        rc = execute_llm(service, args, &builder);
    } else if (strcmp(command, "mesh") == 0) {
        rc = execute_cat(service, "/host/sys/topology", &builder);
    } else if (strcmp(command, "sessions") == 0) {
        rc = execute_cat(service, "/host/sys/sessions", &builder);
    } else if (strcmp(command, "hosts") == 0) {
        rc = execute_cat(service, "/host/sys/hosts", &builder);
    } else if (strcmp(command, "host") == 0) {
        rc = execute_cat(service, "/host/sys/health", &builder);
    } else if (strcmp(command, "net") == 0 && strcmp(args, "status") == 0) {
        rc = execute_cat(service, "/host/sys/web", &builder);
    } else if (strcmp(command, "clear") == 0) {
        output_append(&builder, "\033[2J\033[H");  /* ANSI 清屏 */
    } else {
        output_append(&builder, "%s: command not found\r\n", command);
        rc = -(int)M9P_ERR_ENOTSUP;
    }

    /* ---- 更新统计 ---- */
    if (rc != 0) service->failed++;
    if (builder.truncated) service->truncated++;

    *out_len = builder.len;
    return rc;
}
