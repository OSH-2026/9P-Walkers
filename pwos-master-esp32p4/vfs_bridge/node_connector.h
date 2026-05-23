#ifndef NODE_CONNECTOR_H
#define NODE_CONNECTOR_H

/**
 * @brief 初始化静态直连从机节点，将其注册进 cluster_vfs 并执行 TATTACH。
 *
 * 在 cluster_config_init_mesh_host() 之后调用。
 * 若从机未连接（TATTACH 超时），仅打印警告，不返回 fatal 错误；
 * 可随后通过 Lua shell 执行 vfs.attach("mcu1") 手动重试。
 *
 * @return 0 表示 transport + 路由注册成功；负错误码表示 UART 初始化失败。
 */
int node_connector_init_static_slave(void);

#endif /* NODE_CONNECTOR_H */
