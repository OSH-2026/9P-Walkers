#include "job_command.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mini9p_protocol.h"

typedef struct {
    char *data;
    size_t cap;
    size_t len;
    uint8_t truncated;
} job_output_t;

static void output_append(job_output_t *output, const char *format, ...)
{
    va_list args;
    int written;

    if (output == NULL || output->data == NULL || output->cap == 0u ||
        output->len >= output->cap - 1u) {
        if (output != NULL) {
            output->truncated = 1u;
        }
        return;
    }
    va_start(args, format);
    written = vsnprintf(
        output->data + output->len,
        output->cap - output->len,
        format,
        args);
    va_end(args);
    if (written < 0) {
        return;
    }
    if ((size_t)written >= output->cap - output->len) {
        output->len = output->cap - 1u;
        output->truncated = 1u;
    } else {
        output->len += (size_t)written;
    }
}

static char *trim_left(char *text)
{
    while (text != NULL && (*text == ' ' || *text == '\t')) {
        ++text;
    }
    return text;
}

static void trim_right(char *text)
{
    size_t len;

    if (text == NULL) {
        return;
    }
    len = strlen(text);
    while (len > 0u &&
           (text[len - 1u] == ' ' || text[len - 1u] == '\t' ||
            text[len - 1u] == '\r' || text[len - 1u] == '\n')) {
        text[--len] = '\0';
    }
}

static char *take_token(char **cursor)
{
    char *token;
    char *end;

    if (cursor == NULL || *cursor == NULL) {
        return NULL;
    }
    token = trim_left(*cursor);
    if (*token == '\0') {
        *cursor = token;
        return NULL;
    }
    end = token;
    while (*end != '\0' && *end != ' ' && *end != '\t') {
        ++end;
    }
    if (*end != '\0') {
        *end++ = '\0';
    }
    *cursor = end;
    return token;
}

static int parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *out)
{
    char *end;
    unsigned long value;

    if (text == NULL || out == NULL || *text == '\0') {
        return -1;
    }
    value = strtoul(text, &end, 10);
    if (*end != '\0' || value < min || value > max) {
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

static int resolve_target(
    pwos_job_command_t *command,
    const char *target,
    uint8_t *out_addr,
    uint32_t *out_boot_id)
{
    if (target == NULL || target[0] == '\0') {
        return -(int)M9P_ERR_EINVAL;
    }
    /* 命令层只认识 mcuN 字符串；addr/boot_id 一定交给外部 resolver。 */
    return command->config.resolve(
        command->config.resolve_ctx, target, out_addr, out_boot_id);
}

static int build_submit_input(
    uint8_t kernel,
    char *cursor,
    uint8_t *input,
    uint16_t *out_len,
    job_output_t *output)
{
    char *arg;

    if (kernel == PWOS_JOB_KERNEL_HASH) {
        /* hash 的 payload 就是原始文本字节，STM32 端计算 FNV-1a。 */
        char *text = trim_left(cursor);
        size_t len;

        trim_right(text);
        len = strlen(text);
        if (len == 0u || len > PWOS_JOB_MAX_PAYLOAD_LEN) {
            output_append(output, "usage: job submit <mcuN> hash <text>\r\n");
            return -(int)M9P_ERR_EINVAL;
        }
        memcpy(input, text, len);
        *out_len = (uint16_t)len;
        return 0;
    }

    if (kernel == PWOS_JOB_KERNEL_VECTOR_ADD) {
        /* vector_add payload: count + int16 向量 A + int16 向量 B。 */
        uint32_t count = 8u;
        uint32_t i;

        arg = take_token(&cursor);
        if ((arg != NULL && parse_u32(arg, 1u, 64u, &count) != 0) ||
            take_token(&cursor) != NULL) {
            output_append(output,
                "usage: job submit <mcuN> vector_add [count:1..64]\r\n");
            return -(int)M9P_ERR_EINVAL;
        }
        pwos_job_put_le16(input, (uint16_t)count);
        for (i = 0u; i < count; ++i) {
            pwos_job_put_i16(input + 2u + i * 2u, (int16_t)i);
            pwos_job_put_i16(
                input + 2u + count * 2u + i * 2u,
                (int16_t)(count - i));
        }
        *out_len = (uint16_t)(2u + count * 4u);
        return 0;
    }

    if (kernel == PWOS_JOB_KERNEL_MATMUL) {
        /* matmul 这里内置 2x2 验收样例，便于答辩现场演示。 */
        static const int16_t a[] = {1, 2, 3, 4};
        static const int16_t b[] = {5, 6, 7, 8};
        size_t i;

        if (take_token(&cursor) != NULL) {
            output_append(output, "usage: job submit <mcuN> matmul\r\n");
            return -(int)M9P_ERR_EINVAL;
        }
        /* 默认矩阵用于端到端验收，期望结果为 [19,22;43,50]。 */
        input[0] = 2u;
        input[1] = 2u;
        input[2] = 2u;
        input[3] = 0u;
        for (i = 0u; i < 4u; ++i) {
            pwos_job_put_i16(input + 4u + i * 2u, a[i]);
            pwos_job_put_i16(input + 12u + i * 2u, b[i]);
        }
        *out_len = 20u;
        return 0;
    }

    if (kernel == PWOS_JOB_KERNEL_MANDELBROT) {
        /* Mandelbrot 使用 Q16 定点坐标参数，避免 MCU 浮点依赖。 */
        uint32_t width = 16u;
        uint32_t height = 16u;
        uint32_t max_iter = 80u;

        arg = take_token(&cursor);
        if (arg != NULL && parse_u32(arg, 2u, 16u, &width) != 0) {
            goto bad_mandelbrot;
        }
        arg = take_token(&cursor);
        if (arg != NULL && parse_u32(arg, 2u, 16u, &height) != 0) {
            goto bad_mandelbrot;
        }
        arg = take_token(&cursor);
        if (arg != NULL && parse_u32(arg, 1u, 255u, &max_iter) != 0) {
            goto bad_mandelbrot;
        }
        if (take_token(&cursor) != NULL) {
            goto bad_mandelbrot;
        }
        input[0] = (uint8_t)width;
        input[1] = (uint8_t)height;
        pwos_job_put_le16(input + 2u, (uint16_t)max_iter);
        pwos_job_put_i32(input + 4u, -2 * 65536);
        pwos_job_put_i32(input + 8u, -1 * 65536);
        pwos_job_put_i32(input + 12u, (int32_t)((3u * 65536u) / (width - 1u)));
        pwos_job_put_i32(input + 16u, (int32_t)((2u * 65536u) / (height - 1u)));
        *out_len = 20u;
        return 0;

bad_mandelbrot:
        output_append(output,
            "usage: job submit <mcuN> mandelbrot [width:2..16 height:2..16 iter:1..255]\r\n");
        return -(int)M9P_ERR_EINVAL;
    }

    output_append(output, "job: unsupported kernel\r\n");
    return -(int)M9P_ERR_ENOTSUP;
}

static int kernel_from_name(const char *name, uint8_t *out_kernel)
{
    if (name == NULL || out_kernel == NULL) {
        return -1;
    }
    if (strcmp(name, "hash") == 0) {
        *out_kernel = PWOS_JOB_KERNEL_HASH;
    } else if (strcmp(name, "vector_add") == 0) {
        *out_kernel = PWOS_JOB_KERNEL_VECTOR_ADD;
    } else if (strcmp(name, "matmul") == 0) {
        *out_kernel = PWOS_JOB_KERNEL_MATMUL;
    } else if (strcmp(name, "mandelbrot") == 0) {
        *out_kernel = PWOS_JOB_KERNEL_MANDELBROT;
    } else {
        return -1;
    }
    return 0;
}

static void render_entry(job_output_t *output, const pwos_job_entry_t *entry)
{
    output_append(output,
        "id=%lu target=%s addr=%u remote=%lu kernel=%s state=%s progress=%u.%u%% result=%lu status=%s(%u) error=%ld\r\n",
        (unsigned long)entry->host_job_id,
        entry->target,
        entry->addr,
        (unsigned long)entry->remote_job_id,
        pwos_job_kernel_name(entry->kernel),
        pwos_job_state_name(entry->state),
        entry->progress_permille / 10u,
        entry->progress_permille % 10u,
        (unsigned long)entry->result_len,
        pwos_job_status_name(entry->remote_status),
        entry->remote_status,
        (long)entry->last_error);
}

static int execute_caps(
    pwos_job_command_t *command,
    char *cursor,
    uint32_t deadline_ms,
    job_output_t *output)
{
    uint8_t caps[PWOS_JOB_MAX_PAYLOAD_LEN + 1u];
    uint8_t addr;
    uint32_t boot_id;
    uint16_t len = PWOS_JOB_MAX_PAYLOAD_LEN;
    uint16_t status = PWOS_JOB_STATUS_OK;
    char *target = take_token(&cursor);
    int rc;

    if (target == NULL || take_token(&cursor) != NULL) {
        output_append(output, "usage: job caps <mcuN>\r\n");
        return -(int)M9P_ERR_EINVAL;
    }
    rc = resolve_target(command, target, &addr, &boot_id);
    if (rc == 0) {
        rc = pwos_job_manager_caps(
            command->config.manager,
            addr,
            boot_id,
            caps,
            &len,
            deadline_ms,
            &status);
    }
    if (rc != 0) {
        output_append(output, "job caps: transport error=%d\r\n", rc);
        return rc;
    }
    caps[len] = '\0';
    output_append(output, "%s: %s\r\n", target, (const char *)caps);
    return status == PWOS_JOB_STATUS_OK ? 0 : -(int)M9P_ERR_EIO;
}

static int execute_submit(
    pwos_job_command_t *command,
    char *cursor,
    uint32_t deadline_ms,
    job_output_t *output)
{
    uint8_t input[PWOS_JOB_MAX_PAYLOAD_LEN];
    uint8_t addr;
    uint8_t kernel;
    uint16_t input_len = 0u;
    uint16_t status = PWOS_JOB_STATUS_OK;
    uint32_t boot_id;
    uint32_t job_id = 0u;
    char *target = take_token(&cursor);
    char *kernel_name = take_token(&cursor);
    pwos_job_entry_t entry;
    int rc;

    if (target == NULL || kernel_from_name(kernel_name, &kernel) != 0) {
        output_append(output,
            "usage: job submit <mcuN> <hash|vector_add|matmul|mandelbrot> [args]\r\n");
        return -(int)M9P_ERR_EINVAL;
    }
    rc = build_submit_input(kernel, cursor, input, &input_len, output);
    if (rc != 0) {
        return rc;
    }
    rc = resolve_target(command, target, &addr, &boot_id);
    if (rc == 0) {
        /* 文本命令最终只调用 job_manager_submit()，不直接接触 session_manager。 */
        rc = pwos_job_manager_submit(
            command->config.manager,
            target,
            addr,
            boot_id,
            kernel,
            input,
            input_len,
            deadline_ms,
            &job_id,
            &status);
    }
    if (job_id != 0u && pwos_job_manager_get(
            command->config.manager, job_id, &entry) == 0) {
        render_entry(output, &entry);
    }
    if (rc != 0) {
        output_append(output, "job submit: transport error=%d\r\n", rc);
        return rc;
    }
    return status == PWOS_JOB_STATUS_OK ? 0 : -(int)M9P_ERR_EIO;
}

static int parse_job_id(char *cursor, uint32_t *out_job_id)
{
    char *id_text = take_token(&cursor);

    if (parse_u32(id_text, 1u, UINT32_MAX, out_job_id) != 0 ||
        take_token(&cursor) != NULL) {
        return -1;
    }
    return 0;
}

static void render_result(
    job_output_t *output,
    const pwos_job_entry_t *entry,
    const uint8_t *result,
    uint16_t result_len)
{
    uint32_t i;

    if (entry->kernel == PWOS_JOB_KERNEL_HASH && result_len == 4u) {
        output_append(output, "hash=0x%08lx\r\n",
            (unsigned long)pwos_job_get_le32(result));
    } else if (entry->kernel == PWOS_JOB_KERNEL_VECTOR_ADD && result_len >= 2u) {
        uint16_t count = pwos_job_get_le16(result);

        output_append(output, "vector=[");
        for (i = 0u; i < count && 2u + (i + 1u) * 4u <= result_len; ++i) {
            output_append(output, "%s%ld", i == 0u ? "" : ",",
                (long)pwos_job_get_i32(result + 2u + i * 4u));
        }
        output_append(output, "]\r\n");
    } else if (entry->kernel == PWOS_JOB_KERNEL_MATMUL && result_len >= 4u) {
        uint8_t rows = result[0];
        uint8_t cols = result[1];

        output_append(output, "matrix=%ux%u [", rows, cols);
        for (i = 0u; i < (uint32_t)rows * cols &&
             4u + (i + 1u) * 4u <= result_len; ++i) {
            output_append(output, "%s%ld",
                i == 0u ? "" : (i % cols == 0u ? ";" : ","),
                (long)pwos_job_get_i32(result + 4u + i * 4u));
        }
        output_append(output, "]\r\n");
    } else if (entry->kernel == PWOS_JOB_KERNEL_MANDELBROT && result_len >= 4u) {
        static const char palette[] = " .:-=+*#%@";
        uint8_t width = result[0];
        uint8_t height = result[1];
        uint16_t max_iter = pwos_job_get_le16(result + 2u);

        output_append(output, "mandelbrot=%ux%u iter=%u\r\n", width, height, max_iter);
        for (i = 0u; i < (uint32_t)width * height && 4u + i < result_len; ++i) {
            uint8_t iter = result[4u + i];
            size_t shade = iter >= max_iter ? sizeof(palette) - 2u :
                ((size_t)iter * (sizeof(palette) - 2u)) / max_iter;

            output_append(output, "%c", palette[shade]);
            if (i % width == width - 1u) {
                output_append(output, "\r\n");
            }
        }
    } else {
        output_append(output, "result: unsupported or malformed payload (%u bytes)\r\n",
            result_len);
    }
}

static int execute_existing(
    pwos_job_command_t *command,
    const char *operation,
    char *cursor,
    uint32_t deadline_ms,
    job_output_t *output)
{
    uint8_t result[PWOS_JOB_MAX_PAYLOAD_LEN];
    uint16_t result_len = sizeof(result);
    uint16_t status = PWOS_JOB_STATUS_OK;
    uint32_t job_id;
    pwos_job_entry_t entry;
    int rc;

    if (parse_job_id(cursor, &job_id) != 0) {
        output_append(output, "usage: job %s <id>\r\n", operation);
        return -(int)M9P_ERR_EINVAL;
    }
    if (strcmp(operation, "status") == 0) {
        rc = pwos_job_manager_status(
            command->config.manager, job_id, deadline_ms, &entry, &status);
    } else if (strcmp(operation, "result") == 0) {
        rc = pwos_job_manager_result(
            command->config.manager,
            job_id,
            deadline_ms,
            result,
            &result_len,
            &entry,
            &status);
    } else {
        rc = pwos_job_manager_cancel(
            command->config.manager, job_id, deadline_ms, &entry, &status);
    }
    if (rc != 0) {
        output_append(output, "job %s: transport error=%d\r\n", operation, rc);
        return rc;
    }
    /* 所有已有 job 操作先输出 job 元数据，再按需输出 result 内容。 */
    render_entry(output, &entry);
    if (status == PWOS_JOB_STATUS_OK && strcmp(operation, "result") == 0) {
        render_result(output, &entry, result, result_len);
    }
    if (status == PWOS_JOB_STATUS_NOT_READY) {
        output_append(output, "result not ready\r\n");
        return -(int)M9P_ERR_EAGAIN;
    }
    return status == PWOS_JOB_STATUS_OK ? 0 : -(int)M9P_ERR_EIO;
}

static int execute_retry(
    pwos_job_command_t *command,
    char *cursor,
    uint32_t deadline_ms,
    job_output_t *output)
{
    pwos_job_entry_t old;
    pwos_job_entry_t retried;
    uint8_t addr;
    uint16_t status = PWOS_JOB_STATUS_OK;
    uint32_t boot_id;
    uint32_t job_id;
    uint32_t new_job_id = 0u;
    int rc;

    if (parse_job_id(cursor, &job_id) != 0 ||
        pwos_job_manager_get(command->config.manager, job_id, &old) != 0) {
        output_append(output, "usage: job retry <id>\r\n");
        return -(int)M9P_ERR_EINVAL;
    }
    rc = resolve_target(command, old.target, &addr, &boot_id);
    if (rc == 0) {
        rc = pwos_job_manager_retry(
            command->config.manager,
            job_id,
            addr,
            boot_id,
            deadline_ms,
            &new_job_id,
            &status);
    }
    if (new_job_id != 0u && pwos_job_manager_get(
            command->config.manager, new_job_id, &retried) == 0) {
        render_entry(output, &retried);
    }
    if (rc != 0) {
        output_append(output, "job retry: error=%d\r\n", rc);
        return rc;
    }
    return status == PWOS_JOB_STATUS_OK ? 0 : -(int)M9P_ERR_EIO;
}

static int execute_list(pwos_job_command_t *command, job_output_t *output)
{
    size_t i;
    size_t count = 0u;

    for (i = 0u; i < PWOS_JOB_MANAGER_MAX_JOBS; ++i) {
        pwos_job_entry_t entry;

        if (pwos_job_manager_get_at(command->config.manager, i, &entry) != 0) {
            continue;
        }
        render_entry(output, &entry);
        ++count;
    }
    if (count == 0u) {
        output_append(output, "(empty)\r\n");
    }
    return 0;
}

int pwos_job_command_init(
    pwos_job_command_t *command,
    const pwos_job_command_config_t *config)
{
    if (command == NULL || config == NULL || config->manager == NULL ||
        config->resolve == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    memset(command, 0, sizeof(*command));
    command->config = *config;
    return 0;
}

int pwos_job_command_execute(
    pwos_job_command_t *command,
    const char *args,
    char *output,
    size_t output_cap,
    size_t *out_len,
    uint32_t deadline_ms)
{
    char line[PWOS_JOB_COMMAND_MAX_LINE];
    char *cursor;
    char *operation;
    job_output_t builder;
    int rc;

    if (command == NULL || args == NULL || output == NULL || output_cap == 0u ||
        out_len == NULL || strlen(args) >= sizeof(line)) {
        return -(int)M9P_ERR_EINVAL;
    }
    memcpy(line, args, strlen(args) + 1u);
    trim_right(line);
    cursor = trim_left(line);
    operation = take_token(&cursor);
    memset(&builder, 0, sizeof(builder));
    builder.data = output;
    builder.cap = output_cap;
    output[0] = '\0';

    if (operation == NULL || strcmp(operation, "help") == 0) {
        output_append(&builder,
            "job caps <mcuN>\r\n"
            "job submit <mcuN> <hash|vector_add|matmul|mandelbrot> [args]\r\n"
            "job list | status <id> | result <id> | cancel <id> | retry <id>\r\n");
        rc = 0;
    } else if (strcmp(operation, "caps") == 0) {
        rc = execute_caps(command, cursor, deadline_ms, &builder);
    } else if (strcmp(operation, "submit") == 0) {
        rc = execute_submit(command, cursor, deadline_ms, &builder);
    } else if (strcmp(operation, "list") == 0 && take_token(&cursor) == NULL) {
        rc = execute_list(command, &builder);
    } else if (strcmp(operation, "status") == 0 ||
               strcmp(operation, "result") == 0 ||
               strcmp(operation, "cancel") == 0) {
        rc = execute_existing(command, operation, cursor, deadline_ms, &builder);
    } else if (strcmp(operation, "retry") == 0) {
        rc = execute_retry(command, cursor, deadline_ms, &builder);
    } else {
        output_append(&builder, "job: unknown operation\r\n");
        rc = -(int)M9P_ERR_EINVAL;
    }
    *out_len = builder.len;
    if (builder.truncated != 0u && rc == 0) {
        return -(int)M9P_ERR_EMSIZE;
    }
    return rc;
}
