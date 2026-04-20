#ifndef MINI9P_CLIENT_H
#define MINI9P_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mini9p_protocol.h"

#define M9P_CLIENT_BUFFER_CAP 512u
#define M9P_ROOT_FID 1u
#define M9P_FIRST_DYNAMIC_FID 2u

typedef int (*m9p_transport_fn)(
    void *transport_ctx,
    const uint8_t *tx_data,
    size_t tx_len,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len);

struct m9p_client {
    m9p_transport_fn transport;
    void *transport_ctx;
    uint16_t next_tag;
    uint16_t next_fid;
    uint16_t negotiated_msize;
    uint8_t max_fids;
    uint8_t max_inflight;
    uint32_t feature_bits;
    bool attached;
    struct m9p_qid root_qid;
    uint8_t tx_buffer[M9P_CLIENT_BUFFER_CAP];
    uint8_t rx_buffer[M9P_CLIENT_BUFFER_CAP];
};

void m9p_client_init(struct m9p_client *client, m9p_transport_fn transport, void *transport_ctx);
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