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
    struct m9p_dirent entries[20];
    uint16_t written = 0u;
    uint8_t data[1024];
    uint16_t iounit = 0u;
    uint16_t count = 0u;
    uint16_t page_count = 0u;
    uint32_t dir_offset = 0u;
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
    expect_true(entry_count == 2u, "root dir entry count without lfs");
    expect_true(strcmp(entries[0].name, "sys") == 0, "root contains sys");
    expect_true(strcmp(entries[1].name, "compute") == 0, "root contains compute");

    count = 0u;
    expect_true(ops->stat(&vfs, "/compute", &stat) == 0, "stat compute");
    expect_true((stat.flags & M9P_STAT_DIR) != 0u, "compute is dir");
    expect_true(ops->read(
        &vfs, "/compute", 0u, M9P_OREAD,
        data, sizeof(data), &count) == 0, "read compute dir");
    entry_count = m9p_parse_dirents(
        data, count, entries, sizeof(entries) / sizeof(entries[0]));
    expect_true(entry_count == 3u, "compute dir entry count");
    expect_true(strcmp(entries[0].name, "caps") == 0, "compute contains caps");
    expect_true(strcmp(entries[1].name, "load") == 0, "compute contains load");
    expect_true(strcmp(entries[2].name, "jobs") == 0, "compute contains jobs");

    count = 0u;
    expect_true(ops->open(&vfs, "/sys", M9P_OREAD, &qid, &iounit) == 0, "open sys dir");
    expect_true(ops->read(&vfs, "/sys", 0u, M9P_OREAD, data, sizeof(data), &count) == 0, "read sys dir");
    entry_count = m9p_parse_dirents(data, count, entries, sizeof(entries) / sizeof(entries[0]));
    expect_true(entry_count >= 11u, "sys diagnostic entries");
    expect_true(strcmp(entries[0].name, "health") == 0, "sys contains health");

    /* 小页读取也必须只返回完整 dirent，下一页 offset 可直接继续解析。 */
    entry_count = 0u;
    do {
        size_t parsed;

        page_count = 0u;
        expect_true(ops->read(
            &vfs, "/sys", dir_offset, M9P_OREAD,
            data, 64u, &page_count) == 0, "paged sys read");
        parsed = m9p_parse_dirents(
            data, page_count, entries,
            sizeof(entries) / sizeof(entries[0]));
        expect_true(page_count == 0u || parsed > 0u, "paged dirent complete");
        entry_count += parsed;
        dir_offset += page_count;
    } while (page_count > 0u);
    expect_true(entry_count >= 11u, "paged sys entry count");

    expect_true(ops->stat(&vfs, "/missing", &stat) == -(int)M9P_ERR_ENOENT, "missing path");
    expect_true(ops->write(&vfs, "/sys/health", 0u, M9P_OWRITE, data, 1u, &written) ==
                    -(int)M9P_ERR_EPERM,
                "write health denied");
    expect_true(ops->open(&vfs, "/sys/fault", M9P_OWRITE, &qid, &iounit) == 0,
                "open fault write");
    expect_true(ops->write(&vfs, "/sys/fault", 0u, M9P_OWRITE, data, 1u, &written) ==
                    -(int)M9P_ERR_ENOTSUP,
                "fault callback unavailable");

    if (g_failures != 0) {
        printf("local_vfs_test: %d failure(s)\n", g_failures);
        return 1;
    }

    puts("local_vfs_test: ok");
    return 0;
}
