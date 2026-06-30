#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "command_service.h"
#include "pwos_rpc_protocol.h"
#include "session_manager.h"

static int g_failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        ++g_failures; \
        printf("CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

typedef struct {
    int read_rc;
    uint32_t last_deadline;
    char last_path[128];
    char written[128];
    char rpc_target[16];
    char rpc_service[32];
    char rpc_method[32];
    char rpc_payload[128];
    uint8_t rpc_mode;
    uint16_t rpc_status;
    char job_args[128];
} fake_io_t;

static int fake_read(
    void *ctx,
    const char *path,
    uint8_t *buf,
    uint16_t *in_out_len,
    uint32_t deadline_ms)
{
    fake_io_t *io = (fake_io_t *)ctx;
    const char *text = strcmp(path, "/host/sys/health") == 0 ? "host ok\n" : "ok\n";
    size_t len = strlen(text);

    io->last_deadline = deadline_ms;
    snprintf(io->last_path, sizeof(io->last_path), "%s", path);
    if (io->read_rc != 0) {
        return io->read_rc;
    }
    if (len > *in_out_len) {
        return -(int)M9P_ERR_EMSIZE;
    }
    memcpy(buf, text, len);
    *in_out_len = (uint16_t)len;
    return 0;
}

static int fake_write(
    void *ctx,
    const char *path,
    const uint8_t *data,
    uint16_t len,
    uint16_t *out_written,
    uint32_t deadline_ms)
{
    fake_io_t *io = (fake_io_t *)ctx;

    io->last_deadline = deadline_ms;
    snprintf(io->last_path, sizeof(io->last_path), "%s", path);
    memcpy(io->written, data, len);
    io->written[len] = '\0';
    *out_written = len;
    return 0;
}

static int fake_list(
    void *ctx,
    const char *path,
    struct m9p_dirent *entries,
    size_t max_entries,
    size_t *out_count,
    uint32_t deadline_ms)
{
    fake_io_t *io = (fake_io_t *)ctx;

    io->last_deadline = deadline_ms;
    snprintf(io->last_path, sizeof(io->last_path), "%s", path);
    if (max_entries < 2u) {
        return -(int)M9P_ERR_EMSIZE;
    }
    memset(entries, 0, sizeof(*entries) * 2u);
    snprintf(entries[0].name, sizeof(entries[0].name), "host");
    entries[0].qid.type = M9P_QID_DIR;
    snprintf(entries[1].name, sizeof(entries[1].name), "mcu1");
    entries[1].qid.type = M9P_QID_DIR;
    *out_count = 2u;
    return 0;
}

static int fake_stat(
    void *ctx,
    const char *path,
    struct m9p_stat *out_stat,
    uint32_t deadline_ms)
{
    fake_io_t *io = (fake_io_t *)ctx;

    io->last_deadline = deadline_ms;
    snprintf(io->last_path, sizeof(io->last_path), "%s", path);
    memset(out_stat, 0, sizeof(*out_stat));
    snprintf(out_stat->name, sizeof(out_stat->name), "health");
    out_stat->size = 3u;
    out_stat->qid.object_id = 7u;
    return 0;
}

static int fake_rpc(
    void *ctx,
    const char *target,
    const char *service,
    const char *method,
    const uint8_t *payload,
    uint16_t payload_len,
    uint32_t deadline_ms,
    uint8_t mode,
    uint8_t *response,
    uint16_t *in_out_response_len,
    uint16_t *out_status,
    uint16_t *out_chunk_count)
{
    fake_io_t *io = (fake_io_t *)ctx;
    static const char reply[] = "pong";

    io->last_deadline = deadline_ms;
    io->rpc_mode = mode;
    snprintf(io->rpc_target, sizeof(io->rpc_target), "%s", target);
    snprintf(io->rpc_service, sizeof(io->rpc_service), "%s", service);
    snprintf(io->rpc_method, sizeof(io->rpc_method), "%s", method);
    assert(payload_len < sizeof(io->rpc_payload));
    memcpy(io->rpc_payload, payload, payload_len);
    io->rpc_payload[payload_len] = '\0';
    if (mode == PWOS_COMMAND_RPC_ONEWAY) {
        return 0;
    }
    assert(response != NULL && in_out_response_len != NULL && out_status != NULL);
    assert(*in_out_response_len >= sizeof(reply) - 1u);
    memcpy(response, reply, sizeof(reply) - 1u);
    *in_out_response_len = sizeof(reply) - 1u;
    *out_status = io->rpc_status;
    if (mode == PWOS_COMMAND_RPC_STREAM) {
        assert(out_chunk_count != NULL);
        *out_chunk_count = 2u;
    }
    return 0;
}

static int fake_job(
    void *ctx,
    const char *args,
    char *output,
    size_t output_cap,
    size_t *out_len,
    uint32_t deadline_ms)
{
    fake_io_t *io = (fake_io_t *)ctx;
    static const char reply[] = "id=7 state=queued\r\n";

    io->last_deadline = deadline_ms;
    snprintf(io->job_args, sizeof(io->job_args), "%s", args);
    assert(output_cap > sizeof(reply));
    memcpy(output, reply, sizeof(reply));
    *out_len = sizeof(reply) - 1u;
    return 0;
}

static void init_service(pwos_command_service_t *service, fake_io_t *io)
{
    pwos_command_service_config_t config;

    memset(service, 0, sizeof(*service));
    memset(io, 0, sizeof(*io));
    memset(&config, 0, sizeof(config));
    config.io_ctx = io;
    config.read_path = fake_read;
    config.write_path = fake_write;
    config.list = fake_list;
    config.stat = fake_stat;
    config.rpc = fake_rpc;
    config.job = fake_job;
    config.default_deadline_ms = 1500u;
    CHECK(pwos_command_service_init(service, &config) == 0);
}

static int execute(
    pwos_command_service_t *service,
    const char *line,
    char *output,
    size_t cap)
{
    size_t len = 0u;
    int rc = pwos_command_service_execute(service, line, output, cap, &len);

    CHECK(len == strlen(output));
    return rc;
}

static void test_paths_and_aliases(void)
{
    pwos_command_service_t service;
    fake_io_t io;
    char output[512];

    init_service(&service, &io);
    CHECK(execute(&service, "ls /", output, sizeof(output)) == 0);
    CHECK(strstr(output, "host/") != NULL);
    CHECK(strstr(output, "mcu1/") != NULL);

    CHECK(execute(&service, "cat /mcu1/sys/health", output, sizeof(output)) == 0);
    CHECK(strcmp(io.last_path, "/mcu1/sys/health") == 0);
    CHECK(strcmp(output, "ok\n") == 0);

    CHECK(execute(&service, "host", output, sizeof(output)) == 0);
    CHECK(strcmp(io.last_path, "/host/sys/health") == 0);
    CHECK(io.last_deadline == 1500u);
}

static void test_error_does_not_poison_next_command(void)
{
    pwos_command_service_t service;
    fake_io_t io;
    char output[512];

    init_service(&service, &io);
    io.read_rc = PWOS_SESSION_ERR_DEADLINE;
    CHECK(execute(&service, "cat /mcu2/sys/health", output, sizeof(output)) ==
          PWOS_SESSION_ERR_DEADLINE);
    CHECK(strstr(output, "deadline") != NULL);

    io.read_rc = 0;
    CHECK(execute(&service, "cat /mcu1/sys/health", output, sizeof(output)) == 0);
    CHECK(strcmp(output, "ok\n") == 0);
    CHECK(service.executed == 2u);
    CHECK(service.failed == 1u);
}

static void test_write_and_stat(void)
{
    pwos_command_service_t service;
    fake_io_t io;
    char output[512];

    init_service(&service, &io);
    CHECK(execute(&service, "echo 42 > /mcu1/dev/pwm", output, sizeof(output)) == 0);
    CHECK(strcmp(io.written, "42") == 0);
    CHECK(strcmp(io.last_path, "/mcu1/dev/pwm") == 0);

    CHECK(execute(&service, "stat /mcu1/sys/health", output, sizeof(output)) == 0);
    CHECK(strstr(output, "name=health") != NULL);
    CHECK(strstr(output, "size=3") != NULL);
}

static void test_fault_command(void)
{
    pwos_command_service_t service;
    fake_io_t io;
    char output[512];

    init_service(&service, &io);
    CHECK(execute(
        &service,
        "fault mcu2 drop port 0 25",
        output,
        sizeof(output)) == 0);
    CHECK(strcmp(io.last_path, "/mcu2/sys/fault") == 0);
    CHECK(strcmp(io.written, "drop port 0 25") == 0);
    CHECK(strstr(output, "configured on mcu2") != NULL);

    CHECK(execute(&service, "fault mcu2 status", output, sizeof(output)) == 0);
    CHECK(strcmp(io.last_path, "/mcu2/sys/fault") == 0);
}

static void test_rpc_commands(void)
{
    pwos_command_service_t service;
    fake_io_t io;
    char output[512];

    init_service(&service, &io);
    CHECK(execute(
        &service,
        "rpc mcu2 system.ping hello --deadline=250",
        output,
        sizeof(output)) == 0);
    CHECK(strcmp(io.rpc_target, "mcu2") == 0);
    CHECK(strcmp(io.rpc_service, "system") == 0);
    CHECK(strcmp(io.rpc_method, "ping") == 0);
    CHECK(strcmp(io.rpc_payload, "hello") == 0);
    CHECK(io.last_deadline == 250u);
    CHECK(strcmp(output, "pong\r\n") == 0);

    CHECK(execute(
        &service,
        "notify mcu1 system.notify event",
        output,
        sizeof(output)) == 0);
    CHECK(io.rpc_mode == PWOS_COMMAND_RPC_ONEWAY);
    CHECK(strcmp(io.rpc_payload, "event") == 0);
    CHECK(strcmp(output, "queued\r\n") == 0);

    io.rpc_status = PWOS_RPC_STATUS_DEADLINE;
    CHECK(execute(
        &service,
        "rpc mcu1 system.delay 500 --deadline=20",
        output,
        sizeof(output)) != 0);
    CHECK(strstr(output, "remote deadline") != NULL);

    io.rpc_status = PWOS_RPC_STATUS_OK;
    CHECK(execute(
        &service,
        "stream mcu2 system.stream abcdef",
        output,
        sizeof(output)) == 0);
    CHECK(io.rpc_mode == PWOS_COMMAND_RPC_STREAM);
    CHECK(strstr(output, "pong") != NULL);
    CHECK(strstr(output, "stream chunks=2 bytes=4") != NULL);
}

static void test_job_command(void)
{
    pwos_command_service_t service;
    fake_io_t io;
    char output[512];

    init_service(&service, &io);
    CHECK(execute(
        &service,
        "job submit mcu1 matmul",
        output,
        sizeof(output)) == 0);
    CHECK(strcmp(io.job_args, "submit mcu1 matmul") == 0);
    CHECK(io.last_deadline == 1500u);
    CHECK(strcmp(output, "id=7 state=queued\r\n") == 0);
}

int main(void)
{
    test_paths_and_aliases();
    test_error_does_not_poison_next_command();
    test_write_and_stat();
    test_fault_command();
    test_rpc_commands();
    test_job_command();

    if (g_failures != 0) {
        printf("pwos command service tests failed: %d\n", g_failures);
        return 1;
    }
    printf("pwos command service tests passed\n");
    return 0;
}
