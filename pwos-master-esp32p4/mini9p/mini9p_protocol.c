#include "mini9p_protocol.h"

#include <string.h>

static void put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint16_t get_le16(const uint8_t *src)
{
    return (uint16_t)src[0] | (uint16_t)((uint16_t)src[1] << 8);
}

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] |
        ((uint32_t)src[1] << 8) |
        ((uint32_t)src[2] << 16) |
        ((uint32_t)src[3] << 24);
}

static void encode_qid(uint8_t *dst, const struct m9p_qid *qid)
{
    dst[0] = qid->type;
    dst[1] = qid->reserved;
    put_le16(dst + 2, qid->version);
    put_le32(dst + 4, qid->object_id);
}

static void decode_qid(const uint8_t *src, struct m9p_qid *qid)
{
    qid->type = src[0];
    qid->reserved = src[1];
    qid->version = get_le16(src + 2);
    qid->object_id = get_le32(src + 4);
}

static bool payload_to_frame(
    uint8_t type,
    uint16_t tag,
    const uint8_t *payload,
    uint16_t payload_len,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    return m9p_encode_frame(type, tag, payload, payload_len, out_frame, out_cap, out_len);
}

uint16_t m9p_crc16_ccitt_false(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    size_t i;

    for (i = 0; i < len; ++i) {
        unsigned int bit;

        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (bit = 0; bit < 8u; ++bit) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

bool m9p_encode_frame(
    uint8_t type,
    uint16_t tag,
    const uint8_t *payload,
    uint16_t payload_len,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    const uint16_t frame_len_field = (uint16_t)(4u + payload_len);
    const size_t total_len = 2u + 2u + (size_t)frame_len_field + 2u;
    uint16_t crc;

    if (out_frame == NULL || out_len == NULL) {
        return false;
    }
    if (payload_len > 0u && payload == NULL) {
        return false;
    }
    if (out_cap < total_len) {
        return false;
    }

    out_frame[0] = (uint8_t)'9';
    out_frame[1] = (uint8_t)'P';
    put_le16(out_frame + 2, frame_len_field);
    out_frame[4] = M9P_VERSION;
    out_frame[5] = type;
    put_le16(out_frame + 6, tag);
    if (payload_len > 0u) {
        memcpy(out_frame + 8, payload, payload_len);
    }
    crc = m9p_crc16_ccitt_false(out_frame + 4, frame_len_field);
    put_le16(out_frame + 8 + payload_len, crc);
    *out_len = total_len;
    return true;
}

bool m9p_decode_frame(const uint8_t *frame, size_t frame_len, struct m9p_frame_view *out_view)
{
    uint16_t frame_len_field;
    uint16_t actual_crc;
    uint16_t expected_crc;

    if (frame == NULL || out_view == NULL || frame_len < M9P_FRAME_OVERHEAD) {
        return false;
    }
    if (frame[0] != (uint8_t)'9' || frame[1] != (uint8_t)'P') {
        return false;
    }

    frame_len_field = get_le16(frame + 2);
    if (frame_len_field < 4u) {
        return false;
    }
    if ((size_t)(frame_len_field + 6u) != frame_len) {
        return false;
    }

    actual_crc = get_le16(frame + frame_len - 2u);
    expected_crc = m9p_crc16_ccitt_false(frame + 4, frame_len_field);
    if (actual_crc != expected_crc) {
        return false;
    }

    out_view->version = frame[4];
    out_view->type = frame[5];
    out_view->tag = get_le16(frame + 6);
    out_view->payload = frame + 8;
    out_view->payload_len = (uint16_t)(frame_len_field - 4u);
    return true;
}

bool m9p_build_tattach(
    uint16_t tag,
    uint16_t fid,
    uint16_t requested_msize,
    uint8_t requested_inflight,
    uint8_t attach_flags,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    uint8_t payload[6];

    put_le16(payload, fid);
    put_le16(payload + 2, requested_msize);
    payload[4] = requested_inflight;
    payload[5] = attach_flags;
    return payload_to_frame(M9P_TATTACH, tag, payload, sizeof(payload), out_frame, out_cap, out_len);
}

bool m9p_build_twalk(
    uint16_t tag,
    uint16_t fid,
    uint16_t newfid,
    const char *path,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    uint8_t payload[4u + 1u + M9P_MAX_PATH_LEN];
    size_t path_len = 0u;

    if (path != NULL && path[0] != '\0' && !(path[0] == '/' && path[1] == '\0')) {
        path_len = strlen(path);
    }
    if (path_len > M9P_MAX_PATH_LEN) {
        return false;
    }

    put_le16(payload, fid);
    put_le16(payload + 2, newfid);
    payload[4] = (uint8_t)path_len;
    if (path_len > 0u) {
        memcpy(payload + 5, path, path_len);
    }
    return payload_to_frame(
        M9P_TWALK,
        tag,
        payload,
        (uint16_t)(5u + path_len),
        out_frame,
        out_cap,
        out_len);
}

bool m9p_build_topen(
    uint16_t tag,
    uint16_t fid,
    uint8_t mode,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    uint8_t payload[3];

    put_le16(payload, fid);
    payload[2] = mode;
    return payload_to_frame(M9P_TOPEN, tag, payload, sizeof(payload), out_frame, out_cap, out_len);
}

bool m9p_build_tread(
    uint16_t tag,
    uint16_t fid,
    uint32_t offset,
    uint16_t count,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    uint8_t payload[8];

    put_le16(payload, fid);
    put_le32(payload + 2, offset);
    put_le16(payload + 6, count);
    return payload_to_frame(M9P_TREAD, tag, payload, sizeof(payload), out_frame, out_cap, out_len);
}

bool m9p_build_twrite(
    uint16_t tag,
    uint16_t fid,
    uint32_t offset,
    const uint8_t *data,
    uint16_t count,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    uint8_t payload[8u + 256u];

    if (count > 256u) {
        return false;
    }
    if (count > 0u && data == NULL) {
        return false;
    }

    put_le16(payload, fid);
    put_le32(payload + 2, offset);
    put_le16(payload + 6, count);
    if (count > 0u) {
        memcpy(payload + 8, data, count);
    }
    return payload_to_frame(
        M9P_TWRITE,
        tag,
        payload,
        (uint16_t)(8u + count),
        out_frame,
        out_cap,
        out_len);
}

bool m9p_build_tstat(
    uint16_t tag,
    uint16_t fid,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    uint8_t payload[2];

    put_le16(payload, fid);
    return payload_to_frame(M9P_TSTAT, tag, payload, sizeof(payload), out_frame, out_cap, out_len);
}

bool m9p_build_tclunk(
    uint16_t tag,
    uint16_t fid,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    uint8_t payload[2];

    put_le16(payload, fid);
    return payload_to_frame(M9P_TCLUNK, tag, payload, sizeof(payload), out_frame, out_cap, out_len);
}

bool m9p_parse_rattach(const struct m9p_frame_view *frame, struct m9p_attach_result *out_result)
{
    if (frame == NULL || out_result == NULL || frame->type != M9P_RATTACH || frame->payload_len < 16u) {
        return false;
    }

    out_result->negotiated_msize = get_le16(frame->payload);
    out_result->max_fids = frame->payload[2];
    out_result->max_inflight = frame->payload[3];
    out_result->feature_bits = get_le32(frame->payload + 4);
    decode_qid(frame->payload + 8, &out_result->root_qid);
    return true;
}

bool m9p_parse_rwalk(const struct m9p_frame_view *frame, struct m9p_qid *out_qid)
{
    if (frame == NULL || out_qid == NULL || frame->type != M9P_RWALK || frame->payload_len < 8u) {
        return false;
    }

    decode_qid(frame->payload, out_qid);
    return true;
}

bool m9p_parse_ropen(const struct m9p_frame_view *frame, struct m9p_open_result *out_result)
{
    if (frame == NULL || out_result == NULL || frame->type != M9P_ROPEN || frame->payload_len < 10u) {
        return false;
    }

    decode_qid(frame->payload, &out_result->qid);
    out_result->iounit = get_le16(frame->payload + 8);
    return true;
}

bool m9p_parse_rread(
    const struct m9p_frame_view *frame,
    uint8_t *out_data,
    uint16_t out_cap,
    uint16_t *out_count)
{
    uint16_t count;

    if (frame == NULL || out_count == NULL || frame->type != M9P_RREAD || frame->payload_len < 2u) {
        return false;
    }

    count = get_le16(frame->payload);
    if ((uint16_t)(2u + count) > frame->payload_len) {
        return false;
    }
    if (count > 0u && (out_data == NULL || out_cap < count)) {
        return false;
    }

    if (count > 0u) {
        memcpy(out_data, frame->payload + 2, count);
    }
    *out_count = count;
    return true;
}

bool m9p_parse_rwrite(const struct m9p_frame_view *frame, uint16_t *out_count)
{
    if (frame == NULL || out_count == NULL || frame->type != M9P_RWRITE || frame->payload_len < 2u) {
        return false;
    }

    *out_count = get_le16(frame->payload);
    return true;
}

bool m9p_parse_rstat(const struct m9p_frame_view *frame, struct m9p_stat *out_stat)
{
    size_t name_len;

    if (frame == NULL || out_stat == NULL || frame->type != M9P_RSTAT || frame->payload_len < 19u) {
        return false;
    }

    decode_qid(frame->payload, &out_stat->qid);
    out_stat->perm = frame->payload[8];
    out_stat->flags = frame->payload[9];
    out_stat->size = get_le32(frame->payload + 10);
    out_stat->mtime = get_le32(frame->payload + 14);
    name_len = frame->payload[18];
    if (19u + name_len > frame->payload_len) {
        return false;
    }
    if (name_len > M9P_MAX_NAME_LEN) {
        name_len = M9P_MAX_NAME_LEN;
    }
    memcpy(out_stat->name, frame->payload + 19, name_len);
    out_stat->name[name_len] = '\0';
    return true;
}

bool m9p_parse_rerror(const struct m9p_frame_view *frame, struct m9p_error *out_error)
{
    size_t msg_len;

    if (frame == NULL || out_error == NULL || frame->type != M9P_RERROR || frame->payload_len < 3u) {
        return false;
    }

    out_error->code = get_le16(frame->payload);
    msg_len = frame->payload[2];
    if (3u + msg_len > frame->payload_len) {
        return false;
    }
    if (msg_len > M9P_MAX_ERROR_TEXT) {
        msg_len = M9P_MAX_ERROR_TEXT;
    }
    memcpy(out_error->msg, frame->payload + 3, msg_len);
    out_error->msg[msg_len] = '\0';
    return true;
}

size_t m9p_parse_dirents(
    const uint8_t *data,
    size_t data_len,
    struct m9p_dirent *entries,
    size_t max_entries)
{
    size_t offset = 0u;
    size_t produced = 0u;

    while (offset + 11u <= data_len && produced < max_entries) {
        size_t name_len = data[offset + 10u];

        if (offset + 11u + name_len > data_len) {
            break;
        }

        if (entries != NULL) {
            decode_qid(data + offset, &entries[produced].qid);
            entries[produced].perm = data[offset + 8u];
            entries[produced].flags = data[offset + 9u];
            if (name_len > M9P_MAX_NAME_LEN) {
                name_len = M9P_MAX_NAME_LEN;
            }
            memcpy(entries[produced].name, data + offset + 11u, name_len);
            entries[produced].name[name_len] = '\0';
        }

        offset += 11u + data[offset + 10u];
        ++produced;
    }

    return produced;
}

const char *m9p_error_name(uint16_t code)
{
    switch (code) {
    case M9P_ERR_EINVAL:
        return "EINVAL";
    case M9P_ERR_ENOENT:
        return "ENOENT";
    case M9P_ERR_EPERM:
        return "EPERM";
    case M9P_ERR_EBUSY:
        return "EBUSY";
    case M9P_ERR_ENOTDIR:
        return "ENOTDIR";
    case M9P_ERR_EISDIR:
        return "EISDIR";
    case M9P_ERR_EOFFS:
        return "EOFFS";
    case M9P_ERR_EMSIZE:
        return "EMSIZE";
    case M9P_ERR_EIO:
        return "EIO";
    case M9P_ERR_ENOTSUP:
        return "ENOTSUP";
    case M9P_ERR_ETAG:
        return "ETAG";
    case M9P_ERR_EFID:
        return "EFID";
    case M9P_ERR_EAGAIN:
        return "EAGAIN";
    default:
        return "UNKNOWN";
    }
}