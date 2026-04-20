/**
 * @file lfs_port.h
 * @author hb (huobin92@gmail.com)
 * @brief 
 * @version 0.1
 * @date 2026-04-20
 * 
 * @copyright Copyright (c) 2026
 * 
 */

#ifndef LFS_PORT_HPP
#define LFS_PORT_HPP

#include "lfs.h"

int lfs_port_init(void);
lfs_t *lfs_port_fs(void);

#endif