#ifndef MINI9P_CLIENT_H
#define MINI9P_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mini9p_protocol.h"

/* client 侧单帧缓冲，与 link payload 512B 上限保持一致。 */
#define M9P_CLIENT_BUFFER_CAP 512u
/* 默认协商 msize，实际值以 RATTACH 返回为准。 */
#define M9P_DEFAULT_MSIZE 512u
/* 请求的默认并发能力；当前 session_manager 会把同节点 client 串行化。 */
#define M9P_DEFAULT_INFLIGHT 4u
/* attach 后根目录 fid，cluster_vfs 的 walk 都从根 fid 出发。 */
#define M9P_ROOT_FID 0u
/* 动态 fid 从 1 开始，避免和 root fid 冲突。 */
#define M9P_FIRST_DYNAMIC_FID 1u
/* transport 返回 EAGAIN/EBUSY 时，协议 client 最多本地重试 3 次。 */
#define M9P_CLIENT_TIMEOUT_RETRY_ATTEMPTS 3u

typedef int (*m9p_transport_fn)(
    void *transport_ctx,
    const uint8_t *tx_data,
    size_t tx_len,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len);

struct m9p_client {
    /* transport 由 session_manager 注入，负责把一个 mini9P 帧变成 DATA_MINI9P 请求。 */
    m9p_transport_fn transport;
    void *transport_ctx;
    /* 本地 tag 只供 m9p_client 校验；上 wire 前会被 session_manager retag。 */
    uint16_t next_tag;
    /* 下一个动态 fid；open_path() 会 walk 分配新 fid。 */
    uint16_t next_fid;
    /* attach 协商结果，server 可降低 msize/inflight。 */
    uint16_t negotiated_msize;
    uint8_t max_fids;
    uint8_t max_inflight;
    uint32_t feature_bits;
    /* attached=false 时 walk/open/read/write 都不应继续使用旧会话。 */
    bool attached;
    struct m9p_qid root_qid;
    /* client 是 stop-and-wait 风格，所以一个 tx_buffer/rx_buffer 足够。 */
    uint8_t tx_buffer[M9P_CLIENT_BUFFER_CAP];
    uint8_t rx_buffer[M9P_CLIENT_BUFFER_CAP];
};

void m9p_client_init(struct m9p_client *client, m9p_transport_fn transport, void *transport_ctx);
void m9p_client_reset_session(struct m9p_client *client);
uint16_t m9p_client_alloc_fid(struct m9p_client *client);

int m9p_client_attach(
    struct m9p_client *client,
    uint16_t requested_msize,
    uint8_t requested_inflight,
    uint8_t attach_flags);
int m9p_client_walk(struct m9p_client *client, uint16_t fid, uint16_t newfid, const char *path, struct m9p_qid *out_qid);
int m9p_client_open(struct m9p_client *client, uint16_t fid, uint8_t mode, struct m9p_open_result *out_result);
int m9p_client_read(
    struct m9p_client *client,
    uint16_t fid,
    uint32_t offset,
    uint8_t *data,
    uint16_t *in_out_count);
int m9p_client_write(
    struct m9p_client *client,
    uint16_t fid,
    uint32_t offset,
    const uint8_t *data,
    uint16_t count,
    uint16_t *out_written);
int m9p_client_stat(struct m9p_client *client, uint16_t fid, struct m9p_stat *out_stat);
int m9p_client_clunk(struct m9p_client *client, uint16_t fid);

int m9p_client_walk_path(struct m9p_client *client, const char *path, uint16_t *out_fid, struct m9p_qid *out_qid);
int m9p_client_open_path(
    struct m9p_client *client,
    const char *path,
    uint8_t mode,
    uint16_t *out_fid,
    struct m9p_open_result *out_result);

#endif
