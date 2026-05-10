#include "local_vfs.h"
#include "mini9p_protocol.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

static void expect_true(bool condition, const char *message)
{
    if (!condition) {
        ++g_failures;
        printf("FAIL: %s\n", message);
    }
}

int main(void)
{
    struct local_vfs vfs;
    struct local_vfs_config config;
    const struct m9p_server_ops *ops;
    struct m9p_stat stat;
    struct m9p_qid qid;
    struct m9p_dirent entries[4];
    uint16_t written = 0u;
    uint8_t data[128];
    uint16_t iounit = 0u;
    uint16_t count = 0u;
    size_t entry_count;

    local_vfs_get_default_config(&config);
    config.iounit = 64u;
    expect_true(local_vfs_init(&vfs, &config) == 0, "vfs init");

    ops = local_vfs_ops();
    expect_true(ops != NULL, "ops present");

    expect_true(ops->stat(&vfs, "/", &stat) == 0, "stat root");
    expect_true((stat.flags & M9P_STAT_DIR) != 0u, "root is dir");

    expect_true(ops->stat(&vfs, "/sys", &stat) == 0, "stat sys");
    expect_true((stat.flags & M9P_STAT_DIR) != 0u, "sys is dir");

    expect_true(ops->stat(&vfs, "/sys/health", &stat) == 0, "stat health");
    expect_true(stat.size == 3u, "health size");
    expect_true(strcmp(stat.name, "health") == 0, "health name");
    expect_true((stat.qid.type & M9P_QID_READONLY) != 0u, "health readonly qid");

    expect_true(ops->open(&vfs, "/sys/health", M9P_OREAD, &qid, &iounit) == 0, "open health");
    expect_true(iounit == 64u, "open iounit");
    expect_true(ops->read(&vfs, "/sys/health", 0u, M9P_OREAD, data, sizeof(data), &count) == 0, "read health");
    expect_true(count == 3u, "health count");
    expect_true(memcmp(data, "ok\n", 3u) == 0, "health data");
    expect_true(ops->clunk(&vfs, "/sys/health", true) == 0, "clunk health");

    count = 0u;
    expect_true(ops->open(&vfs, "/", M9P_OREAD, &qid, &iounit) == 0, "open root dir");
    expect_true(ops->read(&vfs, "/", 0u, M9P_OREAD, data, sizeof(data), &count) == 0, "read root dir");
    entry_count = m9p_parse_dirents(data, count, entries, sizeof(entries) / sizeof(entries[0]));
    expect_true(entry_count == 1u, "root dir entry count without lfs");
    expect_true(strcmp(entries[0].name, "sys") == 0, "root contains sys");

    count = 0u;
    expect_true(ops->open(&vfs, "/sys", M9P_OREAD, &qid, &iounit) == 0, "open sys dir");
    expect_true(ops->read(&vfs, "/sys", 0u, M9P_OREAD, data, sizeof(data), &count) == 0, "read sys dir");
    entry_count = m9p_parse_dirents(data, count, entries, sizeof(entries) / sizeof(entries[0]));
    expect_true(entry_count == 1u, "sys dir entry count");
    expect_true(strcmp(entries[0].name, "health") == 0, "sys contains health");

    expect_true(ops->stat(&vfs, "/missing", &stat) == -(int)M9P_ERR_ENOENT, "missing path");
    expect_true(ops->write(&vfs, "/sys/health", 0u, M9P_OWRITE, data, 1u, &written) ==
                    -(int)M9P_ERR_ENOTSUP,
                "write unsupported");

    if (g_failures != 0) {
        printf("local_vfs_test: %d failure(s)\n", g_failures);
        return 1;
    }

    puts("local_vfs_test: ok");
    return 0;
}
