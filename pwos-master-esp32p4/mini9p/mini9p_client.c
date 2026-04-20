#include "mini9p_client.h"

#include <string.h>

static int request_with_frame(
    struct m9p_client *client,
    size_t tx_len,
    uint16_t expected_tag,
    uint8_t expected_type,
    struct m9p_frame_view *out_frame)
{
    size_t rx_len = 0u;
    struct m9p_frame_view frame;
    struct m9p_error error;
    int rc;

    if (client == NULL || client->transport == NULL || out_frame == NULL) {
        return -(int)M9P_ERR_ENOTSUP;
    }

    rc = client->transport(
        client->transport_ctx,
        client->tx_buffer,
        tx_len,
        client->rx_buffer,
        sizeof(client->rx_buffer),
        &rx_len);
    if (rc != 0) {
        return rc;
    }
    if (!m9p_decode_frame(client->rx_buffer, rx_len, &frame)) {
        return -(int)M9P_ERR_EIO;
    }
    if (frame.version != M9P_VERSION) {
        return -(int)M9P_ERR_EINVAL;
    }
    if (frame.tag != expected_tag) {
        return -(int)M9P_ERR_ETAG;
    }
    if (frame.type == M9P_RERROR) {
        if (m9p_parse_rerror(&frame, &error)) {
            return -(int)error.code;
        }
        return -(int)M9P_ERR_EIO;
    }
    if (frame.type != expected_type) {
        return -(int)M9P_ERR_EIO;
    }

    *out_frame = frame;
    return 0;
}

void m9p_client_init(struct m9p_client *client, m9p_transport_fn transport, void *transport_ctx)
{
    if (client == NULL) {
        return;
    }

    memset(client, 0, sizeof(*client));
    client->transport = transport;
    client->transport_ctx = transport_ctx;
    client->next_tag = 1u;
    client->next_fid = M9P_FIRST_DYNAMIC_FID;
    client->negotiated_msize = 256u;
}

uint16_t m9p_client_alloc_fid(struct m9p_client *client)
{
    uint16_t fid;

    if (client == NULL) {
        return 0u;
    }

    fid = client->next_fid;
    ++client->next_fid;
    if (client->next_fid == 0u || client->next_fid == M9P_ROOT_FID) {
        client->next_fid = M9P_FIRST_DYNAMIC_FID;
    }
    return fid;
}

int m9p_client_attach(
    struct m9p_client *client,
    uint16_t requested_msize,
    uint8_t requested_inflight,
    uint8_t attach_flags)
{
    struct m9p_frame_view frame;
    struct m9p_attach_result attach_result;
    size_t tx_len = 0u;
    uint16_t tag;

    if (client == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    tag = client->next_tag++;
    if (!m9p_build_tattach(
            tag,
            M9P_ROOT_FID,
            requested_msize,
            requested_inflight,
            attach_flags,
            client->tx_buffer,
            sizeof(client->tx_buffer),
            &tx_len)) {
        return -(int)M9P_ERR_EMSIZE;
    }

    {
        int rc = request_with_frame(client, tx_len, tag, M9P_RATTACH, &frame);

        if (rc != 0) {
            return rc;
        }
    }
    if (!m9p_parse_rattach(&frame, &attach_result)) {
        return -(int)M9P_ERR_EIO;
    }

    client->negotiated_msize = attach_result.negotiated_msize;
    client->max_fids = attach_result.max_fids;
    client->max_inflight = attach_result.max_inflight;
    client->feature_bits = attach_result.feature_bits;
    client->root_qid = attach_result.root_qid;
    client->attached = true;
    return 0;
}

int m9p_client_walk(struct m9p_client *client, uint16_t fid, uint16_t newfid, const char *path, struct m9p_qid *out_qid)
{
    struct m9p_frame_view frame;
    size_t tx_len = 0u;
    uint16_t tag;
    int rc;

    if (client == NULL || out_qid == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    tag = client->next_tag++;
    if (!m9p_build_twalk(tag, fid, newfid, path, client->tx_buffer, sizeof(client->tx_buffer), &tx_len)) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = request_with_frame(client, tx_len, tag, M9P_RWALK, &frame);
    if (rc != 0) {
        return rc;
    }
    if (!m9p_parse_rwalk(&frame, out_qid)) {
        return -(int)M9P_ERR_EIO;
    }

    return 0;
}

int m9p_client_open(struct m9p_client *client, uint16_t fid, uint8_t mode, struct m9p_open_result *out_result)
{
    struct m9p_frame_view frame;
    size_t tx_len = 0u;
    uint16_t tag;
    int rc;

    if (client == NULL || out_result == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    tag = client->next_tag++;
    if (!m9p_build_topen(tag, fid, mode, client->tx_buffer, sizeof(client->tx_buffer), &tx_len)) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = request_with_frame(client, tx_len, tag, M9P_ROPEN, &frame);
    if (rc != 0) {
        return rc;
    }
    if (!m9p_parse_ropen(&frame, out_result)) {
        return -(int)M9P_ERR_EIO;
    }

    return 0;
}

int m9p_client_read(
    struct m9p_client *client,
    uint16_t fid,
    uint32_t offset,
    uint8_t *data,
    uint16_t *in_out_count)
{
    struct m9p_frame_view frame;
    size_t tx_len = 0u;
    uint16_t tag;
    int rc;

    if (client == NULL || in_out_count == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    tag = client->next_tag++;
    if (!m9p_build_tread(
            tag,
            fid,
            offset,
            *in_out_count,
            client->tx_buffer,
            sizeof(client->tx_buffer),
            &tx_len)) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = request_with_frame(client, tx_len, tag, M9P_RREAD, &frame);
    if (rc != 0) {
        return rc;
    }
    if (!m9p_parse_rread(&frame, data, *in_out_count, in_out_count)) {
        return -(int)M9P_ERR_EIO;
    }

    return 0;
}

int m9p_client_write(
    struct m9p_client *client,
    uint16_t fid,
    uint32_t offset,
    const uint8_t *data,
    uint16_t count,
    uint16_t *out_written)
{
    struct m9p_frame_view frame;
    size_t tx_len = 0u;
    uint16_t tag;
    int rc;

    if (client == NULL || out_written == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    tag = client->next_tag++;
    if (!m9p_build_twrite(
            tag,
            fid,
            offset,
            data,
            count,
            client->tx_buffer,
            sizeof(client->tx_buffer),
            &tx_len)) {
        return -(int)M9P_ERR_EMSIZE;
    }

    rc = request_with_frame(client, tx_len, tag, M9P_RWRITE, &frame);
    if (rc != 0) {
        return rc;
    }
    if (!m9p_parse_rwrite(&frame, out_written)) {
        return -(int)M9P_ERR_EIO;
    }

    return 0;
}

int m9p_client_stat(struct m9p_client *client, uint16_t fid, struct m9p_stat *out_stat)
{
    struct m9p_frame_view frame;
    size_t tx_len = 0u;
    uint16_t tag;
    int rc;

    if (client == NULL || out_stat == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    tag = client->next_tag++;
    if (!m9p_build_tstat(tag, fid, client->tx_buffer, sizeof(client->tx_buffer), &tx_len)) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = request_with_frame(client, tx_len, tag, M9P_RSTAT, &frame);
    if (rc != 0) {
        return rc;
    }
    if (!m9p_parse_rstat(&frame, out_stat)) {
        return -(int)M9P_ERR_EIO;
    }

    return 0;
}

int m9p_client_clunk(struct m9p_client *client, uint16_t fid)
{
    struct m9p_frame_view frame;
    size_t tx_len = 0u;
    uint16_t tag;
    int rc;

    if (client == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    tag = client->next_tag++;
    if (!m9p_build_tclunk(tag, fid, client->tx_buffer, sizeof(client->tx_buffer), &tx_len)) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = request_with_frame(client, tx_len, tag, M9P_RCLUNK, &frame);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

int m9p_client_walk_path(struct m9p_client *client, const char *path, uint16_t *out_fid, struct m9p_qid *out_qid)
{
    uint16_t fid;

    if (client == NULL || path == NULL || out_fid == NULL || out_qid == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    if (!client->attached) {
        return -(int)M9P_ERR_EPERM;
    }

    fid = m9p_client_alloc_fid(client);
    if (fid == 0u) {
        return -(int)M9P_ERR_EFID;
    }
    {
        int rc = m9p_client_walk(client, M9P_ROOT_FID, fid, path, out_qid);

        if (rc != 0) {
            return rc;
        }
    }

    *out_fid = fid;
    return 0;
}

int m9p_client_open_path(
    struct m9p_client *client,
    const char *path,
    uint8_t mode,
    uint16_t *out_fid,
    struct m9p_open_result *out_result)
{
    struct m9p_qid qid;
    uint16_t fid;
    int rc;

    if (client == NULL || path == NULL || out_fid == NULL || out_result == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = m9p_client_walk_path(client, path, &fid, &qid);
    if (rc != 0) {
        return rc;
    }

    rc = m9p_client_open(client, fid, mode, out_result);
    if (rc != 0) {
        (void)m9p_client_clunk(client, fid);
        return rc;
    }

    *out_fid = fid;
    return 0;
}