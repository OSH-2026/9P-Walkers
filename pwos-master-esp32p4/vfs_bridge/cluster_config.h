#ifndef CLUSTER_CONFIG_H
#define CLUSTER_CONFIG_H

/**
 * @brief 注册静态配置的集群节点。
 *
 * 该启动辅助函数维护当前 Master 侧的节点配置，并把用户可见的节点名
 * （例如 "mcu1"）注册到 cluster_vfs 中。
 *
 * @return 0 表示静态注册完成；负 Mini9P 风格错误码只表示注册失败。
 *         当前启动策略下，attach 失败不会被视为致命错误。
 */
int cluster_init_static_nodes(void);

#endif /* CLUSTER_CONFIG_H */
