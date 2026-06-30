#include "command_service.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pwos_rpc_protocol.h"
#include "session_manager.h"

#define PWOS_COMMAND_READ_CAP 1024u
#define PWOS_COMMAND_LIST_CAP 24u
#define PWOS_COMMAND_RPC_RESPONSE_CAP PWOS_RPC_MAX_FRAME_LEN

typedef struct {
    char *data;
    size_t cap;
    size_t len;
    uint8_t truncated;
} output_builder_t;

static void output_append(output_builder_t *builder, const char *fmt, ...)
{
    va_list args;
    int written;

    if (builder == NULL || builder->data == NULL || builder->cap == 0u ||
        builder->len >= builder->cap - 1u) {
        if (builder != NULL) {
            builder->truncated = 1u;
        }
        return;
    }

    va_start(args, fmt);
    written = vsnprintf(
        builder->data + builder->len,
        builder->cap - builder->len,
        fmt,
        args);
    va_end(args);
    if (written < 0) {
        return;
    }
    if ((size_t)written >= builder->cap - builder->len) {
        builder->len = builder->cap - 1u;
        builder->truncated = 1u;
    } else {
        builder->len += (size_t)written;
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

static const char *error_name(int rc)
{
    if (rc <= PWOS_SESSION_ERR_NO_ROUTE && rc >= PWOS_SESSION_ERR_STALE_BOOT) {
        return pwos_session_error_name(rc);
    }
    if (rc < 0 && -rc <= (int)UINT16_MAX) {
        return m9p_error_name((uint16_t)(-rc));
    }
    return "io_error";
}

static int execute_cat(
    pwos_command_service_t *service,
    const char *path,
    output_builder_t *output)
{
    uint8_t data[PWOS_COMMAND_READ_CAP + 1u];
    uint16_t len = PWOS_COMMAND_READ_CAP;
    int rc;

    if (path == NULL || path[0] == '\0') {
        output_append(output, "usage: cat <path>\r\n");
        return -(int)M9P_ERR_EINVAL;
    }
    rc = service->config.read_path(
        service->config.io_ctx,
        path,
        data,
        &len,
        service->config.default_deadline_ms);
    if (rc != 0) {
        output_append(output, "cat: %s (%d)\r\n", error_name(rc), rc);
        return rc;
    }

    data[len] = '\0';
    output_append(output, "%s", (const char *)data);
    if (len == 0u || data[len - 1u] != (uint8_t)'\n') {
        output_append(output, "\r\n");
    }
    return 0;
}

static int execute_ls(
    pwos_command_service_t *service,
    const char *path,
    output_builder_t *output)
{
    struct m9p_dirent entries[PWOS_COMMAND_LIST_CAP];
    size_t count = 0u;
    size_t i;
    int rc;

    if (path == NULL || path[0] == '\0') {
        path = "/";
    }
    memset(entries, 0, sizeof(entries));
    rc = service->config.list(
        service->config.io_ctx,
        path,
        entries,
        PWOS_COMMAND_LIST_CAP,
        &count,
        service->config.default_deadline_ms);
    if (rc != 0) {
        output_append(output, "ls: %s (%d)\r\n", error_name(rc), rc);
        return rc;
    }
    for (i = 0u; i < count; ++i) {
        output_append(
            output,
            "%s%s\r\n",
            entries[i].name,
            (entries[i].qid.type & M9P_QID_DIR) != 0u ? "/" : "");
    }
    if (count == 0u) {
        output_append(output, "(empty)\r\n");
    }
    return 0;
}

static int execute_stat(
    pwos_command_service_t *service,
    const char *path,
    output_builder_t *output)
{
    struct m9p_stat stat;
    int rc;

    if (path == NULL || path[0] == '\0') {
        output_append(output, "usage: stat <path>\r\n");
        return -(int)M9P_ERR_EINVAL;
    }
    memset(&stat, 0, sizeof(stat));
    rc = service->config.stat(
        service->config.io_ctx,
        path,
        &stat,
        service->config.default_deadline_ms);
    if (rc != 0) {
        output_append(output, "stat: %s (%d)\r\n", error_name(rc), rc);
        return rc;
    }
    output_append(
        output,
        "name=%s type=0x%02x size=%lu qid=%lu version=%u\r\n",
        stat.name,
        stat.qid.type,
        (unsigned long)stat.size,
        (unsigned long)stat.qid.object_id,
        stat.qid.version);
    return 0;
}

static int execute_echo(
    pwos_command_service_t *service,
    char *args,
    output_builder_t *output)
{
    char *separator;
    char *text;
    char *path;
    uint16_t written = 0u;
    size_t text_len;
    int rc;

    separator = args == NULL ? NULL : strchr(args, '>');
    if (separator == NULL) {
        output_append(output, "usage: echo <text> > <path>\r\n");
        return -(int)M9P_ERR_EINVAL;
    }
    *separator = '\0';
    text = trim_left(args);
    trim_right(text);
    path = trim_left(separator + 1u);
    trim_right(path);
    text_len = strlen(text);
    if (path[0] == '\0' || text_len > UINT16_MAX) {
        output_append(output, "usage: echo <text> > <path>\r\n");
        return -(int)M9P_ERR_EINVAL;
    }

    rc = service->config.write_path(
        service->config.io_ctx,
        path,
        (const uint8_t *)text,
        (uint16_t)text_len,
        &written,
        service->config.default_deadline_ms);
    if (rc != 0) {
        output_append(output, "echo: %s (%d)\r\n", error_name(rc), rc);
        return rc;
    }
    output_append(output, "wrote %u byte(s)\r\n", written);
    return 0;
}

static int execute_fault(
    pwos_command_service_t *service,
    char *args,
    output_builder_t *output)
{
    char *target;
    char *action;
    char *separator;
    char path[64];
    uint16_t written = 0u;
    size_t action_len;
    int rc;

    target = trim_left(args);
    separator = target;
    while (*separator != '\0' && *separator != ' ' && *separator != '\t') {
        ++separator;
    }
    if (*separator == '\0') {
        output_append(output,
            "usage: fault <mcuN> <status|clear|drop|delay|corrupt|down|recover|reboot-self ...>\r\n");
        return -(int)M9P_ERR_EINVAL;
    }
    *separator++ = '\0';
    action = trim_left(separator);
    trim_right(action);
    if (target[0] == '\0' || strchr(target, '/') != NULL || action[0] == '\0' ||
        snprintf(path, sizeof(path), "/%s/sys/fault", target) >= (int)sizeof(path)) {
        output_append(output, "fault: invalid target or action\r\n");
        return -(int)M9P_ERR_EINVAL;
    }
    if (strcmp(action, "status") == 0) {
        return execute_cat(service, path, output);
    }

    action_len = strlen(action);
    if (action_len > UINT16_MAX) {
        return -(int)M9P_ERR_EINVAL;
    }
    rc = service->config.write_path(
        service->config.io_ctx,
        path,
        (const uint8_t *)action,
        (uint16_t)action_len,
        &written,
        service->config.default_deadline_ms);
    if (rc != 0) {
        output_append(output, "fault: %s (%d)\r\n", error_name(rc), rc);
        return rc;
    }
    output_append(output, "fault configured on %s\r\n", target);
    return 0;
}

static int parse_rpc_deadline(
    char *payload,
    uint32_t default_deadline_ms,
    uint32_t *out_deadline_ms)
{
    static const char prefix[] = "--deadline=";
    char *option;
    char *end;
    unsigned long value;

    if (payload == NULL || out_deadline_ms == NULL) {
        return -1;
    }
    trim_right(payload);
    option = strstr(payload, prefix);
    while (option != NULL && option != payload && option[-1] != ' ' && option[-1] != '\t') {
        option = strstr(option + 1u, prefix);
    }
    if (option == NULL) {
        *out_deadline_ms = default_deadline_ms;
        return 0;
    }
    value = strtoul(option + sizeof(prefix) - 1u, &end, 10);
    end = trim_left(end);
    if (value == 0u || value > UINT32_MAX || *end != '\0') {
        return -1;
    }
    if (option != payload) {
        char *trim = option;

        while (trim > payload && (trim[-1] == ' ' || trim[-1] == '\t')) {
            --trim;
        }
        *trim = '\0';
    } else {
        *payload = '\0';
    }
    *out_deadline_ms = (uint32_t)value;
    return 0;
}

static int execute_rpc(
    pwos_command_service_t *service,
    char *args,
    uint8_t mode,
    output_builder_t *output)
{
    uint8_t response[PWOS_COMMAND_RPC_RESPONSE_CAP];
    uint16_t response_len = sizeof(response);
    uint16_t status = PWOS_RPC_STATUS_OK;
    uint16_t chunk_count = 0u;
    uint32_t deadline_ms;
    char *cursor = args;
    char *target = take_token(&cursor);
    char *endpoint = take_token(&cursor);
    char *method;
    char *payload;
    size_t payload_len;
    int rc;

    if (service->config.rpc == NULL || target == NULL || endpoint == NULL) {
        output_append(output,
            "usage: %s <mcuN> <service.method> [payload] [--deadline=<ms>]\r\n",
            mode == PWOS_COMMAND_RPC_ONEWAY ? "notify" :
                (mode == PWOS_COMMAND_RPC_STREAM ? "stream" : "rpc"));
        return -(int)M9P_ERR_EINVAL;
    }
    method = strchr(endpoint, '.');
    if (method == NULL || method == endpoint || method[1] == '\0') {
        output_append(output, "rpc: endpoint must be service.method\r\n");
        return -(int)M9P_ERR_EINVAL;
    }
    *method++ = '\0';
    payload = trim_left(cursor);
    if (parse_rpc_deadline(
            payload, service->config.default_deadline_ms, &deadline_ms) != 0) {
        output_append(output, "rpc: invalid deadline\r\n");
        return -(int)M9P_ERR_EINVAL;
    }
    payload_len = strlen(payload);
    if (payload_len > UINT16_MAX) {
        return -(int)M9P_ERR_EMSIZE;
    }

    rc = service->config.rpc(
        service->config.io_ctx,
        target,
        endpoint,
        method,
        (const uint8_t *)payload,
        (uint16_t)payload_len,
        deadline_ms,
        mode,
        mode == PWOS_COMMAND_RPC_ONEWAY ? NULL : response,
        mode == PWOS_COMMAND_RPC_ONEWAY ? NULL : &response_len,
        mode == PWOS_COMMAND_RPC_ONEWAY ? NULL : &status,
        mode == PWOS_COMMAND_RPC_STREAM ? &chunk_count : NULL);
    if (rc != 0) {
        output_append(output, "rpc: %s (%d)\r\n", error_name(rc), rc);
        return rc;
    }
    if (mode == PWOS_COMMAND_RPC_ONEWAY) {
        output_append(output, "queued\r\n");
        return 0;
    }
    if (status != PWOS_RPC_STATUS_OK) {
        output_append(output,
            "rpc: remote %s (%u)\r\n", pwos_rpc_status_name(status), status);
        return -(int)M9P_ERR_EIO;
    }
    if (response_len > 0u) {
        output_append(output, "%.*s", (int)response_len, (const char *)response);
    } else {
        output_append(output, "ok");
    }
    output_append(output, "\r\n");
    if (mode == PWOS_COMMAND_RPC_STREAM) {
        output_append(output,
            "stream chunks=%u bytes=%u\r\n", chunk_count, response_len);
    }
    return 0;
}

static int execute_job(
    pwos_command_service_t *service,
    const char *args,
    output_builder_t *output)
{
    size_t produced = 0u;
    size_t remaining;
    int rc;

    if (service->config.job == NULL) {
        output_append(output, "job: unavailable\r\n");
        return -(int)M9P_ERR_ENOTSUP;
    }
    if (output->len >= output->cap) {
        output->truncated = 1u;
        return -(int)M9P_ERR_EMSIZE;
    }
    remaining = output->cap - output->len;
    rc = service->config.job(
        service->config.io_ctx,
        args,
        output->data + output->len,
        remaining,
        &produced,
        service->config.default_deadline_ms);
    if (produced >= remaining) {
        output->len = output->cap - 1u;
        output->data[output->len] = '\0';
        output->truncated = 1u;
        return rc == 0 ? -(int)M9P_ERR_EMSIZE : rc;
    }
    output->len += produced;
    output->data[output->len] = '\0';
    if (rc != 0 && produced == 0u) {
        output_append(output, "job: error (%d)\r\n", rc);
    }
    return rc;
}

int pwos_command_service_init(
    pwos_command_service_t *service,
    const pwos_command_service_config_t *config)
{
    if (service == NULL || config == NULL || config->read_path == NULL ||
        config->write_path == NULL || config->list == NULL || config->stat == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    memset(service, 0, sizeof(*service));
    service->config = *config;
    if (service->config.default_deadline_ms == 0u) {
        service->config.default_deadline_ms = PWOS_COMMAND_DEFAULT_DEADLINE_MS;
    }
    return 0;
}

int pwos_command_service_execute(
    pwos_command_service_t *service,
    const char *line,
    char *output,
    size_t output_cap,
    size_t *out_len)
{
    char command_line[PWOS_COMMAND_MAX_LINE];
    char *command;
    char *args;
    char *separator;
    output_builder_t builder;
    int rc = 0;

    if (service == NULL || line == NULL || output == NULL || output_cap == 0u ||
        out_len == NULL || strlen(line) >= sizeof(command_line)) {
        return -(int)M9P_ERR_EINVAL;
    }

    memset(&builder, 0, sizeof(builder));
    builder.data = output;
    builder.cap = output_cap;
    output[0] = '\0';
    memcpy(command_line, line, strlen(line) + 1u);
    trim_right(command_line);
    command = trim_left(command_line);
    separator = command;
    while (*separator != '\0' && *separator != ' ' && *separator != '\t') {
        ++separator;
    }
    if (*separator != '\0') {
        *separator++ = '\0';
        args = trim_left(separator);
        trim_right(args);
    } else {
        args = separator;
    }

    ++service->executed;
    if (command[0] == '\0') {
        rc = 0;
    } else if (strcmp(command, "help") == 0) {
        output_append(&builder,
            "help  ls [path]  cat <path>  stat <path>\r\n"
            "echo <text> > <path>  mesh  sessions  net status  host\r\n"
            "rpc <mcuN> <service.method> [payload] [--deadline=<ms>]\r\n"
            "stream <mcuN> <service.method> [payload] [--deadline=<ms>]\r\n"
            "notify <mcuN> <service.method> [payload] [--deadline=<ms>]\r\n"
            "job <caps|submit|list|status|result|cancel|retry> ...\r\n"
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
    } else if (strcmp(command, "mesh") == 0) {
        rc = execute_cat(service, "/host/sys/topology", &builder);
    } else if (strcmp(command, "sessions") == 0) {
        rc = execute_cat(service, "/host/sys/sessions", &builder);
    } else if (strcmp(command, "host") == 0) {
        rc = execute_cat(service, "/host/sys/health", &builder);
    } else if (strcmp(command, "net") == 0 && strcmp(args, "status") == 0) {
        rc = execute_cat(service, "/host/sys/web", &builder);
    } else if (strcmp(command, "clear") == 0) {
        output_append(&builder, "\033[2J\033[H");
    } else {
        output_append(&builder, "%s: command not found\r\n", command);
        rc = -(int)M9P_ERR_ENOTSUP;
    }

    if (rc != 0) {
        ++service->failed;
    }
    if (builder.truncated != 0u) {
        ++service->truncated;
    }
    *out_len = builder.len;
    return rc;
}
