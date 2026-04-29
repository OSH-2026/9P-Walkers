#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mini9p_client.h"
#include "mini9p_server.h"

static struct m9p_server validation_server;

static void require_true(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static int loopback_transport(
    void *transport_ctx,
    const uint8_t *tx_data,
    size_t tx_len,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    struct m9p_server *server = (struct m9p_server *)transport_ctx;

    if (!m9p_server_handle_frame(server, tx_data, tx_len, rx_data, rx_cap, rx_len)) {
        return -1;
    }
    return 0;
}

static void read_text(struct m9p_client *client, const char *path, char *buffer, size_t buffer_cap)
{
    struct m9p_open_result open_result;
    uint16_t fid;
    uint16_t count;
    int rc;

    rc = m9p_client_open_path(client, path, M9P_OREAD, &fid, &open_result);
    require_true(rc == 0, "open_path should succeed");

    count = (uint16_t)(buffer_cap - 1u);
    rc = m9p_client_read(client, fid, 0u, (uint8_t *)buffer, &count);
    require_true(rc == 0, "read should succeed");
    buffer[count] = '\0';

    rc = m9p_client_clunk(client, fid);
    require_true(rc == 0, "clunk after read should succeed");
}

static void validate_root_listing(struct m9p_client *client)
{
    struct m9p_open_result open_result;
    struct m9p_dirent entries[4];
    uint8_t raw[64];
    uint16_t fid;
    uint16_t count = sizeof(raw);
    size_t entry_count;
    int rc;

    rc = m9p_client_open_path(client, "/", M9P_OREAD, &fid, &open_result);
    require_true(rc == 0, "open root should succeed");

    rc = m9p_client_read(client, fid, 0u, raw, &count);
    require_true(rc == 0, "read root directory should succeed");

    entry_count = m9p_parse_dirents(raw, count, entries, 4u);
    require_true(entry_count == 2u, "root directory should expose sys and dev");
    require_true(strcmp(entries[0].name, "sys") == 0, "first root child should be sys");
    require_true(strcmp(entries[1].name, "dev") == 0, "second root child should be dev");

    rc = m9p_client_clunk(client, fid);
    require_true(rc == 0, "clunk after root listing should succeed");
}

static void validate_stat(struct m9p_client *client, const char *path, const char *expected_name)
{
    struct m9p_stat stat_info;
    struct m9p_open_result open_result;
    uint16_t fid;
    int rc;

    rc = m9p_client_open_path(client, path, M9P_OREAD, &fid, &open_result);
    require_true(rc == 0, "open_path for stat should succeed");

    rc = m9p_client_stat(client, fid, &stat_info);
    require_true(rc == 0, "stat should succeed");
    require_true(strcmp(stat_info.name, expected_name) == 0, "stat name should match path leaf");

    rc = m9p_client_clunk(client, fid);
    require_true(rc == 0, "clunk after stat should succeed");
}

static void validate_led_write(struct m9p_client *client)
{
    struct m9p_open_result open_result;
    char buffer[16];
    uint16_t fid;
    uint16_t written = 0u;
    int rc;

    rc = m9p_client_open_path(client, "/dev/led", M9P_ORDWR, &fid, &open_result);
    require_true(rc == 0, "open led as rdwr should succeed");

    rc = m9p_client_write(client, fid, 0u, (const uint8_t *)"1\n", 2u, &written);
    require_true(rc == 0, "write led should succeed");
    require_true(written == 2u, "write should report full byte count");

    rc = m9p_client_clunk(client, fid);
    require_true(rc == 0, "clunk after led write should succeed");

    read_text(client, "/dev/led", buffer, sizeof(buffer));
    require_true(strcmp(buffer, "1\n") == 0, "led readback should reflect last write");
}

int main(void)
{
    struct m9p_client client;
    struct m9p_qid qid;
    char buffer[96];
    int rc;

    m9p_server_init(&validation_server);
    m9p_server_set_temperature(&validation_server, 287);
    m9p_client_init(&client, loopback_transport, &validation_server);

    rc = m9p_client_attach(&client, 512u, 1u, 0u);
    require_true(rc == 0, "attach should succeed");
    require_true(client.attached, "client should mark attach success");

    rc = m9p_client_walk(&client, M9P_ROOT_FID, m9p_client_alloc_fid(&client), "/sys/version", &qid);
    require_true(rc == 0, "walk to /sys/version should succeed");
    require_true((qid.type & M9P_QID_READONLY) != 0u, "version qid should be readonly");

    validate_root_listing(&client);
    read_text(&client, "/sys/version", buffer, sizeof(buffer));
    require_true(strstr(buffer, "9P-Walkers") != NULL, "version payload should contain project name");

    read_text(&client, "/sys/health", buffer, sizeof(buffer));
    require_true(strstr(buffer, "status=ok") != NULL, "health payload should contain ok status");

    read_text(&client, "/dev/temperature", buffer, sizeof(buffer));
    require_true(strcmp(buffer, "28.7\n") == 0, "temperature payload should match configured value");

    validate_stat(&client, "/sys/version", "version");
    validate_led_write(&client);

    puts("mini9p module validation passed");
    return 0;
}