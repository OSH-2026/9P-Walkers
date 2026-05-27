/**
 * @file mini9p_board_service.c
 * @brief Initializes pwos-slave local VFS backends and injects them into Mini9P service.
 */

#include "mini9p_board_service.h"

#include <string.h>

#include "dev_vfs.h"
#include "fs_selftest.h"
#include "lfs_port.hpp"
#include "lfs_vfs.h"
#include "main.h"
#include "mini9p_service.h"
#include "node_vfs.h"
#include "sys_vfs.h"

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;

static struct lfs_vfs g_lfs_vfs;
static struct sys_vfs g_sys_vfs;
static struct dev_vfs g_dev_vfs;
static struct node_vfs g_node_vfs;
static FS_SelfTestReport g_fs_report;

int mini9p_board_service_init(void)
{
    struct sys_vfs_config sys_config;
    struct dev_vfs_config dev_config;
    struct lfs_vfs_config lfs_config;
    struct node_vfs_config node_config;
    struct mini9p_service_backend backend;
    UART_HandleTypeDef *mesh_uarts[2];
    struct lfs_vfs *active_lfs = NULL;
    int rc;

#ifndef PWOS_SKIP_LFS_MOUNT
    lfs_vfs_get_default_config(&lfs_config);
    rc = lfs_vfs_init(&g_lfs_vfs, &lfs_config);
    if (rc == 0) {
#ifdef PWOS_ENABLE_LFS_SELFTEST
        (void)fs_selftest_run_on_fs(lfs_vfs_fs(&g_lfs_vfs), lfs_port_backend_name(), &g_fs_report);
#endif
        active_lfs = &g_lfs_vfs;
    }
#else
    (void)lfs_config;
#endif

    memset(&sys_config, 0, sizeof(sys_config));
    sys_config.info_text = active_lfs != NULL ? "pwos node lfs=ok\n" : "pwos node lfs=unavailable\n";
    sys_config.iounit = SYS_VFS_DEFAULT_IOUNIT;
    rc = sys_vfs_init(&g_sys_vfs, &sys_config);
    if (rc != 0) {
        return rc;
    }

    memset(&dev_config, 0, sizeof(dev_config));
    dev_config.iounit = DEV_VFS_DEFAULT_IOUNIT;
    rc = dev_vfs_init(&g_dev_vfs, &dev_config);
    if (rc != 0) {
        return rc;
    }

    memset(&node_config, 0, sizeof(node_config));
    node_config.sys_ops = sys_vfs_ops();
    node_config.sys_ctx = &g_sys_vfs;
    node_config.dev_ops = dev_vfs_ops();
    node_config.dev_ctx = &g_dev_vfs;
    if (active_lfs != NULL) {
        node_config.lfs_ops = lfs_vfs_ops();
        node_config.lfs_ctx = active_lfs;
    }
    node_config.iounit = NODE_VFS_DEFAULT_IOUNIT;
    rc = node_vfs_init(&g_node_vfs, &node_config);
    if (rc != 0) {
        return rc;
    }

    memset(&backend, 0, sizeof(backend));
    backend.ops = node_vfs_ops();
    backend.ops_ctx = &g_node_vfs;
    backend.default_iounit = g_node_vfs.iounit;
    mesh_uarts[0] = &huart1;
    mesh_uarts[1] = &huart2;
    backend.uarts = mesh_uarts;
    backend.uart_count = sizeof(mesh_uarts) / sizeof(mesh_uarts[0]);
    return mini9p_service_init_with_backend(&backend);
}

int mini9p_board_service_poll_once(void)
{
    return mini9p_service_poll_once();
}
