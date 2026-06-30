#include "pwos_host_rpc_protocol.h"
#include "pwos_host_election.h"
#include "pwos_host_rpc_methods.h"

#include <string.h>

enum {
    HOST_RPC_KEY_VERSION = 1,
    HOST_RPC_KEY_CALL_ID = 2,
    HOST_RPC_KEY_KIND = 3,
    HOST_RPC_KEY_SERVICE = 4,
    HOST_RPC_KEY_METHOD = 5,
    HOST_RPC_KEY_DEADLINE = 6,
    HOST_RPC_KEY_STATUS = 7,
    HOST_RPC_KEY_PAYLOAD = 8,
    HOST_RPC_FIELD_COUNT = 8,
};

typedef struct {
    uint8_t *data;
    size_t cap;
    size_t pos;
} cbor_writer_t;

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} cbor_reader_t;

static void put_be32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

uint32_t pwos_host_rpc_body_len(const uint8_t prefix[PWOS_HOST_RPC_PREFIX_LEN])
{
    if (prefix == NULL) {
        return 0u;
    }
    return ((uint32_t)prefix[0] << 24) |
        ((uint32_t)prefix[1] << 16) |
        ((uint32_t)prefix[2] << 8) |
        (uint32_t)prefix[3];
}

static int write_byte(cbor_writer_t *writer, uint8_t value)
{
    if (writer->pos >= writer->cap) {
        return -1;
    }
    writer->data[writer->pos++] = value;
    return 0;
}

static int write_type_value(cbor_writer_t *writer, uint8_t major, uint32_t value)
{
    if (value < 24u) {
        return write_byte(writer, (uint8_t)((major << 5) | value));
    }
    if (value <= UINT8_MAX) {
        return write_byte(writer, (uint8_t)((major << 5) | 24u)) == 0 &&
            write_byte(writer, (uint8_t)value) == 0 ? 0 : -1;
    }
    if (value <= UINT16_MAX) {
        if (write_byte(writer, (uint8_t)((major << 5) | 25u)) != 0 ||
            writer->cap - writer->pos < 2u) {
            return -1;
        }
        writer->data[writer->pos++] = (uint8_t)(value >> 8);
        writer->data[writer->pos++] = (uint8_t)value;
        return 0;
    }
    if (write_byte(writer, (uint8_t)((major << 5) | 26u)) != 0 ||
        writer->cap - writer->pos < 4u) {
        return -1;
    }
    put_be32(writer->data + writer->pos, value);
    writer->pos += 4u;
    return 0;
}

static int write_uint(cbor_writer_t *writer, uint32_t value)
{
    return write_type_value(writer, 0u, value);
}

static int write_bytes(
    cbor_writer_t *writer,
    uint8_t major,
    const uint8_t *data,
    size_t len)
{
    if (len > UINT32_MAX || write_type_value(writer, major, (uint32_t)len) != 0 ||
        writer->cap - writer->pos < len) {
        return -1;
    }
    if (len > 0u) {
        memcpy(writer->data + writer->pos, data, len);
        writer->pos += len;
    }
    return 0;
}

static int read_type_value(
    cbor_reader_t *reader,
    uint8_t expected_major,
    uint32_t *out_value)
{
    uint8_t initial;
    uint8_t additional;
    uint32_t value;

    if (reader->pos >= reader->len || out_value == NULL) {
        return -1;
    }
    initial = reader->data[reader->pos++];
    if ((initial >> 5) != expected_major) {
        return -1;
    }
    additional = initial & 0x1Fu;
    if (additional < 24u) {
        value = additional;
    } else if (additional == 24u) {
        if (reader->pos >= reader->len) {
            return -1;
        }
        value = reader->data[reader->pos++];
        if (value < 24u) {
            return -1; /* 拒绝非规范化整数编码。 */
        }
    } else if (additional == 25u) {
        if (reader->len - reader->pos < 2u) {
            return -1;
        }
        value = ((uint32_t)reader->data[reader->pos] << 8) |
            reader->data[reader->pos + 1u];
        reader->pos += 2u;
        if (value <= UINT8_MAX) {
            return -1;
        }
    } else if (additional == 26u) {
        if (reader->len - reader->pos < 4u) {
            return -1;
        }
        value = pwos_host_rpc_body_len(reader->data + reader->pos);
        reader->pos += 4u;
        if (value <= UINT16_MAX) {
            return -1;
        }
    } else {
        return -1;
    }
    *out_value = value;
    return 0;
}

static int read_bytes(
    cbor_reader_t *reader,
    uint8_t major,
    const uint8_t **out_data,
    uint32_t *out_len)
{
    uint32_t len;

    if (out_data == NULL || out_len == NULL ||
        read_type_value(reader, major, &len) != 0 ||
        len > reader->len - reader->pos) {
        return -1;
    }
    *out_data = reader->data + reader->pos;
    *out_len = len;
    reader->pos += len;
    return 0;
}

static int kind_valid(uint8_t kind)
{
    return kind >= PWOS_HOST_RPC_KIND_REQUEST &&
        kind <= PWOS_HOST_RPC_KIND_STREAM_END;
}

int pwos_host_rpc_encode(
    uint8_t kind,
    uint32_t call_id,
    uint32_t deadline_ms,
    uint16_t status,
    const char *service,
    const char *method,
    const uint8_t *payload,
    uint16_t payload_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len)
{
    cbor_writer_t writer;
    size_t service_len;
    size_t method_len;

    if (out == NULL || out_len == NULL || !kind_valid(kind) || call_id == 0u ||
        status > PWOS_HOST_RPC_STATUS_CANCELLED || service == NULL || method == NULL ||
        payload_len > PWOS_HOST_RPC_MAX_PAYLOAD_LEN ||
        (payload_len > 0u && payload == NULL) || out_cap < PWOS_HOST_RPC_PREFIX_LEN) {
        return -1;
    }
    service_len = strlen(service);
    method_len = strlen(method);
    if (service_len == 0u || service_len > PWOS_HOST_RPC_MAX_SERVICE_LEN ||
        method_len == 0u || method_len > PWOS_HOST_RPC_MAX_METHOD_LEN) {
        return -1;
    }

    writer.data = out + PWOS_HOST_RPC_PREFIX_LEN;
    writer.cap = out_cap - PWOS_HOST_RPC_PREFIX_LEN;
    writer.pos = 0u;
    if (write_type_value(&writer, 5u, HOST_RPC_FIELD_COUNT) != 0 ||
        write_uint(&writer, HOST_RPC_KEY_VERSION) != 0 ||
        write_uint(&writer, PWOS_HOST_RPC_VERSION) != 0 ||
        write_uint(&writer, HOST_RPC_KEY_CALL_ID) != 0 ||
        write_uint(&writer, call_id) != 0 ||
        write_uint(&writer, HOST_RPC_KEY_KIND) != 0 ||
        write_uint(&writer, kind) != 0 ||
        write_uint(&writer, HOST_RPC_KEY_SERVICE) != 0 ||
        write_bytes(&writer, 3u, (const uint8_t *)service, service_len) != 0 ||
        write_uint(&writer, HOST_RPC_KEY_METHOD) != 0 ||
        write_bytes(&writer, 3u, (const uint8_t *)method, method_len) != 0 ||
        write_uint(&writer, HOST_RPC_KEY_DEADLINE) != 0 ||
        write_uint(&writer, deadline_ms) != 0 ||
        write_uint(&writer, HOST_RPC_KEY_STATUS) != 0 ||
        write_uint(&writer, status) != 0 ||
        write_uint(&writer, HOST_RPC_KEY_PAYLOAD) != 0 ||
        write_bytes(&writer, 2u, payload, payload_len) != 0) {
        return -1;
    }
    if (writer.pos == 0u || writer.pos > PWOS_HOST_RPC_MAX_FRAME_LEN - PWOS_HOST_RPC_PREFIX_LEN) {
        return -1;
    }
    put_be32(out, (uint32_t)writer.pos);
    *out_len = PWOS_HOST_RPC_PREFIX_LEN + writer.pos;
    return 0;
}

int pwos_host_rpc_decode(
    const uint8_t *frame,
    size_t frame_len,
    pwos_host_rpc_frame_view_t *out_view)
{
    cbor_reader_t reader;
    uint32_t body_len;
    uint32_t field_count;
    uint32_t seen = 0u;
    uint32_t i;

    if (frame == NULL || out_view == NULL || frame_len < PWOS_HOST_RPC_PREFIX_LEN ||
        frame_len > PWOS_HOST_RPC_MAX_FRAME_LEN) {
        return -1;
    }
    body_len = pwos_host_rpc_body_len(frame);
    if (body_len == 0u || body_len != frame_len - PWOS_HOST_RPC_PREFIX_LEN) {
        return -1;
    }
    memset(out_view, 0, sizeof(*out_view));
    reader.data = frame + PWOS_HOST_RPC_PREFIX_LEN;
    reader.len = body_len;
    reader.pos = 0u;
    if (read_type_value(&reader, 5u, &field_count) != 0 ||
        field_count != HOST_RPC_FIELD_COUNT) {
        return -1;
    }
    for (i = 0u; i < field_count; ++i) {
        const uint8_t *bytes;
        uint32_t key;
        uint32_t value;
        uint32_t len;
        uint32_t bit;

        if (read_type_value(&reader, 0u, &key) != 0 ||
            key < HOST_RPC_KEY_VERSION || key > HOST_RPC_KEY_PAYLOAD) {
            return -1;
        }
        bit = 1u << key;
        if ((seen & bit) != 0u) {
            return -1;
        }
        seen |= bit;
        switch (key) {
        case HOST_RPC_KEY_VERSION:
            if (read_type_value(&reader, 0u, &value) != 0 ||
                value != PWOS_HOST_RPC_VERSION) {
                return -1;
            }
            break;
        case HOST_RPC_KEY_CALL_ID:
            if (read_type_value(&reader, 0u, &out_view->call_id) != 0) {
                return -1;
            }
            break;
        case HOST_RPC_KEY_KIND:
            if (read_type_value(&reader, 0u, &value) != 0 || value > UINT8_MAX) {
                return -1;
            }
            out_view->kind = (uint8_t)value;
            break;
        case HOST_RPC_KEY_SERVICE:
            if (read_bytes(&reader, 3u, &bytes, &len) != 0 || len == 0u ||
                len > PWOS_HOST_RPC_MAX_SERVICE_LEN) {
                return -1;
            }
            out_view->service = (const char *)bytes;
            out_view->service_len = (uint8_t)len;
            break;
        case HOST_RPC_KEY_METHOD:
            if (read_bytes(&reader, 3u, &bytes, &len) != 0 || len == 0u ||
                len > PWOS_HOST_RPC_MAX_METHOD_LEN) {
                return -1;
            }
            out_view->method = (const char *)bytes;
            out_view->method_len = (uint8_t)len;
            break;
        case HOST_RPC_KEY_DEADLINE:
            if (read_type_value(&reader, 0u, &out_view->deadline_ms) != 0) {
                return -1;
            }
            break;
        case HOST_RPC_KEY_STATUS:
            if (read_type_value(&reader, 0u, &value) != 0 || value > UINT16_MAX) {
                return -1;
            }
            out_view->status = (uint16_t)value;
            break;
        case HOST_RPC_KEY_PAYLOAD:
            if (read_bytes(&reader, 2u, &bytes, &len) != 0 ||
                len > PWOS_HOST_RPC_MAX_PAYLOAD_LEN) {
                return -1;
            }
            out_view->payload = bytes;
            out_view->payload_len = (uint16_t)len;
            break;
        default:
            return -1;
        }
    }
    if (reader.pos != reader.len || seen != 0x1FEu || out_view->call_id == 0u ||
        !kind_valid(out_view->kind) ||
        out_view->status > PWOS_HOST_RPC_STATUS_CANCELLED) {
        return -1;
    }
    return 0;
}

const char *pwos_host_rpc_status_name(uint16_t status)
{
    switch (status) {
    case PWOS_HOST_RPC_STATUS_OK: return "ok";
    case PWOS_HOST_RPC_STATUS_BAD_REQUEST: return "bad_request";
    case PWOS_HOST_RPC_STATUS_NOT_FOUND: return "not_found";
    case PWOS_HOST_RPC_STATUS_DEADLINE: return "deadline";
    case PWOS_HOST_RPC_STATUS_BUSY: return "busy";
    case PWOS_HOST_RPC_STATUS_INTERNAL: return "internal";
    case PWOS_HOST_RPC_STATUS_NOT_LEADER: return "not_leader";
    case PWOS_HOST_RPC_STATUS_NO_ROUTE: return "no_route";
    case PWOS_HOST_RPC_STATUS_CANCELLED: return "cancelled";
    default: return "unknown";
    }
}

static int method_writer_init(
    cbor_writer_t *writer,
    uint8_t *out,
    size_t out_cap)
{
    if (writer == NULL || out == NULL || out_cap == 0u) {
        return -1;
    }
    writer->data = out;
    writer->cap = out_cap;
    writer->pos = 0u;
    return 0;
}

static int method_reader_init(
    cbor_reader_t *reader,
    const uint8_t *payload,
    uint16_t payload_len,
    uint32_t expected_fields)
{
    uint32_t fields;

    if (reader == NULL || payload == NULL || payload_len == 0u) {
        return -1;
    }
    reader->data = payload;
    reader->len = payload_len;
    reader->pos = 0u;
    return read_type_value(reader, 5u, &fields) == 0 && fields == expected_fields ?
        0 : -1;
}

static int write_uid(cbor_writer_t *writer, const uint32_t uid[3])
{
    uint8_t bytes[12];

    put_be32(bytes, uid[0]);
    put_be32(bytes + 4u, uid[1]);
    put_be32(bytes + 8u, uid[2]);
    return write_bytes(writer, 2u, bytes, sizeof(bytes));
}

static int read_uid(cbor_reader_t *reader, uint32_t uid[3])
{
    const uint8_t *bytes;
    uint32_t len;

    if (read_bytes(reader, 2u, &bytes, &len) != 0 || len != 12u) {
        return -1;
    }
    uid[0] = pwos_host_rpc_body_len(bytes);
    uid[1] = pwos_host_rpc_body_len(bytes + 4u);
    uid[2] = pwos_host_rpc_body_len(bytes + 8u);
    return 0;
}

static int advertise_valid(const pwos_host_rpc_advertise_t *advertise)
{
    return advertise != NULL &&
        (advertise->uid[0] != 0u || advertise->uid[1] != 0u ||
            advertise->uid[2] != 0u) &&
        advertise->role <= PWOS_HOST_ROLE_LEADER &&
        advertise->rpc_port != 0u;
}

int pwos_host_rpc_encode_advertise(
    const pwos_host_rpc_advertise_t *advertise,
    uint8_t *out,
    size_t out_cap,
    uint16_t *out_len)
{
    cbor_writer_t writer;
    size_t hostname_len;

    if (!advertise_valid(advertise) || out_len == NULL ||
        method_writer_init(&writer, out, out_cap) != 0) {
        return -1;
    }
    hostname_len = strlen(advertise->hostname);
    if (hostname_len == 0u || hostname_len >= PWOS_HOST_RPC_HOSTNAME_CAP) {
        return -1;
    }
    if (write_type_value(&writer, 5u, 6u) != 0 ||
        write_uint(&writer, 1u) != 0 || write_uid(&writer, advertise->uid) != 0 ||
        write_uint(&writer, 2u) != 0 || write_uint(&writer, advertise->epoch) != 0 ||
        write_uint(&writer, 3u) != 0 || write_uint(&writer, advertise->priority) != 0 ||
        write_uint(&writer, 4u) != 0 || write_uint(&writer, advertise->role) != 0 ||
        write_uint(&writer, 5u) != 0 || write_uint(&writer, advertise->rpc_port) != 0 ||
        write_uint(&writer, 6u) != 0 ||
        write_bytes(&writer, 3u, (const uint8_t *)advertise->hostname, hostname_len) != 0 ||
        writer.pos > UINT16_MAX) {
        return -1;
    }
    *out_len = (uint16_t)writer.pos;
    return 0;
}

int pwos_host_rpc_decode_advertise(
    const uint8_t *payload,
    uint16_t payload_len,
    pwos_host_rpc_advertise_t *out_advertise)
{
    cbor_reader_t reader;
    uint32_t seen = 0u;
    uint32_t i;

    if (out_advertise == NULL ||
        method_reader_init(&reader, payload, payload_len, 6u) != 0) {
        return -1;
    }
    memset(out_advertise, 0, sizeof(*out_advertise));
    for (i = 0u; i < 6u; ++i) {
        const uint8_t *text;
        uint32_t key;
        uint32_t value;
        uint32_t len;

        if (read_type_value(&reader, 0u, &key) != 0 || key < 1u || key > 6u ||
            (seen & (1u << key)) != 0u) {
            return -1;
        }
        seen |= 1u << key;
        switch (key) {
        case 1u:
            if (read_uid(&reader, out_advertise->uid) != 0) return -1;
            break;
        case 2u:
            if (read_type_value(&reader, 0u, &out_advertise->epoch) != 0) return -1;
            break;
        case 3u:
            if (read_type_value(&reader, 0u, &value) != 0 || value > UINT16_MAX) return -1;
            out_advertise->priority = (uint16_t)value;
            break;
        case 4u:
            if (read_type_value(&reader, 0u, &value) != 0 || value > UINT8_MAX) return -1;
            out_advertise->role = (uint8_t)value;
            break;
        case 5u:
            if (read_type_value(&reader, 0u, &value) != 0 || value > UINT16_MAX) return -1;
            out_advertise->rpc_port = (uint16_t)value;
            break;
        case 6u:
            if (read_bytes(&reader, 3u, &text, &len) != 0 || len == 0u ||
                len >= PWOS_HOST_RPC_HOSTNAME_CAP) return -1;
            memcpy(out_advertise->hostname, text, len);
            out_advertise->hostname[len] = '\0';
            break;
        default:
            return -1;
        }
    }
    return reader.pos == reader.len && seen == 0x7Eu &&
        advertise_valid(out_advertise) ? 0 : -1;
}

int pwos_host_rpc_encode_read_node(
    const char *target,
    const char *path,
    uint16_t max_bytes,
    uint8_t *out,
    size_t out_cap,
    uint16_t *out_len)
{
    cbor_writer_t writer;
    size_t target_len;
    size_t path_len;

    if (target == NULL || path == NULL || out_len == NULL || max_bytes == 0u ||
        method_writer_init(&writer, out, out_cap) != 0) {
        return -1;
    }
    target_len = strlen(target);
    path_len = strlen(path);
    if (target_len == 0u || target_len >= PWOS_HOST_RPC_TARGET_CAP ||
        path_len == 0u || path_len >= PWOS_HOST_RPC_PATH_CAP || path[0] != '/') {
        return -1;
    }
    if (write_type_value(&writer, 5u, 3u) != 0 ||
        write_uint(&writer, 1u) != 0 ||
        write_bytes(&writer, 3u, (const uint8_t *)target, target_len) != 0 ||
        write_uint(&writer, 2u) != 0 ||
        write_bytes(&writer, 3u, (const uint8_t *)path, path_len) != 0 ||
        write_uint(&writer, 3u) != 0 || write_uint(&writer, max_bytes) != 0 ||
        writer.pos > UINT16_MAX) {
        return -1;
    }
    *out_len = (uint16_t)writer.pos;
    return 0;
}

static int decode_target_path(
    cbor_reader_t *reader,
    uint32_t expected_fields,
    const char **out_target,
    uint8_t *out_target_len,
    const char **out_path,
    uint8_t *out_path_len,
    const uint8_t **out_data,
    uint16_t *out_data_len,
    uint16_t *out_max_bytes)
{
    uint32_t seen = 0u;
    uint32_t i;

    for (i = 0u; i < expected_fields; ++i) {
        const uint8_t *bytes;
        uint32_t key;
        uint32_t len;
        uint32_t value;

        if (read_type_value(reader, 0u, &key) != 0 || key < 1u ||
            key > expected_fields || (seen & (1u << key)) != 0u) {
            return -1;
        }
        seen |= 1u << key;
        if (key == 1u) {
            if (read_bytes(reader, 3u, &bytes, &len) != 0 || len == 0u ||
                len >= PWOS_HOST_RPC_TARGET_CAP) return -1;
            *out_target = (const char *)bytes;
            *out_target_len = (uint8_t)len;
        } else if (key == 2u) {
            if (read_bytes(reader, 3u, &bytes, &len) != 0 || len == 0u ||
                len >= PWOS_HOST_RPC_PATH_CAP || bytes[0] != '/') return -1;
            *out_path = (const char *)bytes;
            *out_path_len = (uint8_t)len;
        } else if (out_max_bytes != NULL) {
            if (read_type_value(reader, 0u, &value) != 0 || value == 0u ||
                value > UINT16_MAX) return -1;
            *out_max_bytes = (uint16_t)value;
        } else {
            if (read_bytes(reader, 2u, &bytes, &len) != 0 || len > UINT16_MAX) return -1;
            *out_data = bytes;
            *out_data_len = (uint16_t)len;
        }
    }
    return seen == ((1u << (expected_fields + 1u)) - 2u) ? 0 : -1;
}

int pwos_host_rpc_decode_read_node(
    const uint8_t *payload,
    uint16_t payload_len,
    pwos_host_rpc_read_node_view_t *out_view)
{
    cbor_reader_t reader;

    if (out_view == NULL || method_reader_init(&reader, payload, payload_len, 3u) != 0) {
        return -1;
    }
    memset(out_view, 0, sizeof(*out_view));
    if (decode_target_path(
            &reader, 3u, &out_view->target, &out_view->target_len,
            &out_view->path, &out_view->path_len,
            NULL, NULL, &out_view->max_bytes) != 0 || reader.pos != reader.len) {
        return -1;
    }
    return 0;
}

int pwos_host_rpc_encode_write_node(
    const char *target,
    const char *path,
    const uint8_t *data,
    uint16_t data_len,
    uint8_t *out,
    size_t out_cap,
    uint16_t *out_len)
{
    cbor_writer_t writer;
    size_t target_len;
    size_t path_len;

    if (target == NULL || path == NULL || out_len == NULL ||
        (data_len > 0u && data == NULL) ||
        method_writer_init(&writer, out, out_cap) != 0) {
        return -1;
    }
    target_len = strlen(target);
    path_len = strlen(path);
    if (target_len == 0u || target_len >= PWOS_HOST_RPC_TARGET_CAP ||
        path_len == 0u || path_len >= PWOS_HOST_RPC_PATH_CAP || path[0] != '/') {
        return -1;
    }
    if (write_type_value(&writer, 5u, 3u) != 0 ||
        write_uint(&writer, 1u) != 0 ||
        write_bytes(&writer, 3u, (const uint8_t *)target, target_len) != 0 ||
        write_uint(&writer, 2u) != 0 ||
        write_bytes(&writer, 3u, (const uint8_t *)path, path_len) != 0 ||
        write_uint(&writer, 3u) != 0 || write_bytes(&writer, 2u, data, data_len) != 0 ||
        writer.pos > UINT16_MAX) {
        return -1;
    }
    *out_len = (uint16_t)writer.pos;
    return 0;
}

int pwos_host_rpc_decode_write_node(
    const uint8_t *payload,
    uint16_t payload_len,
    pwos_host_rpc_write_node_view_t *out_view)
{
    cbor_reader_t reader;

    if (out_view == NULL || method_reader_init(&reader, payload, payload_len, 3u) != 0) {
        return -1;
    }
    memset(out_view, 0, sizeof(*out_view));
    if (decode_target_path(
            &reader, 3u, &out_view->target, &out_view->target_len,
            &out_view->path, &out_view->path_len,
            &out_view->data, &out_view->data_len, NULL) != 0 ||
        reader.pos != reader.len) {
        return -1;
    }
    return 0;
}

int pwos_host_rpc_encode_blob(
    const uint8_t *data,
    uint16_t data_len,
    uint8_t *out,
    size_t out_cap,
    uint16_t *out_len)
{
    cbor_writer_t writer;

    if (out_len == NULL || (data_len > 0u && data == NULL) ||
        method_writer_init(&writer, out, out_cap) != 0 ||
        write_bytes(&writer, 2u, data, data_len) != 0 || writer.pos > UINT16_MAX) {
        return -1;
    }
    *out_len = (uint16_t)writer.pos;
    return 0;
}

int pwos_host_rpc_decode_blob(
    const uint8_t *payload,
    uint16_t payload_len,
    const uint8_t **out_data,
    uint16_t *out_data_len)
{
    cbor_reader_t reader;
    uint32_t len;

    if (payload == NULL || payload_len == 0u || out_data == NULL ||
        out_data_len == NULL) {
        return -1;
    }
    reader.data = payload;
    reader.len = payload_len;
    reader.pos = 0u;
    if (read_bytes(&reader, 2u, out_data, &len) != 0 ||
        len > UINT16_MAX || reader.pos != reader.len) {
        return -1;
    }
    *out_data_len = (uint16_t)len;
    return 0;
}

int pwos_host_rpc_encode_text(
    const char *text,
    uint8_t *out,
    size_t out_cap,
    uint16_t *out_len)
{
    cbor_writer_t writer;
    size_t len;

    if (text == NULL || out_len == NULL ||
        method_writer_init(&writer, out, out_cap) != 0) {
        return -1;
    }
    len = strlen(text);
    if (len == 0u || len > UINT8_MAX ||
        write_bytes(&writer, 3u, (const uint8_t *)text, len) != 0) {
        return -1;
    }
    *out_len = (uint16_t)writer.pos;
    return 0;
}

int pwos_host_rpc_decode_text(
    const uint8_t *payload,
    uint16_t payload_len,
    const char **out_text,
    uint8_t *out_text_len)
{
    cbor_reader_t reader;
    const uint8_t *text;
    uint32_t len;

    if (payload == NULL || payload_len == 0u || out_text == NULL ||
        out_text_len == NULL) {
        return -1;
    }
    reader.data = payload;
    reader.len = payload_len;
    reader.pos = 0u;
    if (read_bytes(&reader, 3u, &text, &len) != 0 || len == 0u ||
        len > UINT8_MAX || reader.pos != reader.len) {
        return -1;
    }
    *out_text = (const char *)text;
    *out_text_len = (uint8_t)len;
    return 0;
}

static int write_topology_node(
    cbor_writer_t *writer,
    const pwos_host_rpc_topology_node_t *node)
{
    size_t global_len = strlen(node->global_target);
    size_t owner_len = strlen(node->owner_target);

    if (global_len == 0u || global_len >= PWOS_HOST_RPC_TARGET_CAP ||
        owner_len == 0u || owner_len >= PWOS_HOST_RPC_TARGET_CAP) {
        return -1;
    }
    return write_type_value(writer, 5u, 5u) == 0 &&
        write_uint(writer, 1u) == 0 &&
        write_bytes(writer, 3u, (const uint8_t *)node->global_target, global_len) == 0 &&
        write_uint(writer, 2u) == 0 &&
        write_bytes(writer, 3u, (const uint8_t *)node->owner_target, owner_len) == 0 &&
        write_uint(writer, 3u) == 0 && write_uid(writer, node->owner_uid) == 0 &&
        write_uint(writer, 4u) == 0 && write_uid(writer, node->node_uid) == 0 &&
        write_uint(writer, 5u) == 0 && write_uint(writer, node->boot_id) == 0 ? 0 : -1;
}

int pwos_host_rpc_encode_topology(
    const pwos_host_rpc_topology_t *topology,
    uint8_t *out,
    size_t out_cap,
    uint16_t *out_len)
{
    cbor_writer_t writer;
    size_t i;

    if (topology == NULL || out_len == NULL ||
        topology->node_count > PWOS_HOST_RPC_TOPOLOGY_MAX_NODES ||
        method_writer_init(&writer, out, out_cap) != 0 ||
        write_type_value(&writer, 5u, 2u) != 0 ||
        write_uint(&writer, 1u) != 0 || write_uint(&writer, topology->generation) != 0 ||
        write_uint(&writer, 2u) != 0 ||
        write_type_value(&writer, 4u, topology->node_count) != 0) {
        return -1;
    }
    for (i = 0u; i < topology->node_count; ++i) {
        if (write_topology_node(&writer, &topology->nodes[i]) != 0) {
            return -1;
        }
    }
    if (writer.pos > UINT16_MAX) {
        return -1;
    }
    *out_len = (uint16_t)writer.pos;
    return 0;
}

static int read_text_copy(
    cbor_reader_t *reader,
    char *out,
    size_t out_cap)
{
    const uint8_t *text;
    uint32_t len;

    if (read_bytes(reader, 3u, &text, &len) != 0 || len == 0u || len >= out_cap) {
        return -1;
    }
    memcpy(out, text, len);
    out[len] = '\0';
    return 0;
}

static int read_topology_node(
    cbor_reader_t *reader,
    pwos_host_rpc_topology_node_t *node)
{
    uint32_t fields;
    uint32_t seen = 0u;
    uint32_t i;

    if (read_type_value(reader, 5u, &fields) != 0 || fields != 5u) {
        return -1;
    }
    memset(node, 0, sizeof(*node));
    for (i = 0u; i < fields; ++i) {
        uint32_t key;

        if (read_type_value(reader, 0u, &key) != 0 || key < 1u || key > 5u ||
            (seen & (1u << key)) != 0u) {
            return -1;
        }
        seen |= 1u << key;
        switch (key) {
        case 1u:
            if (read_text_copy(reader, node->global_target,
                    sizeof(node->global_target)) != 0) return -1;
            break;
        case 2u:
            if (read_text_copy(reader, node->owner_target,
                    sizeof(node->owner_target)) != 0) return -1;
            break;
        case 3u:
            if (read_uid(reader, node->owner_uid) != 0) return -1;
            break;
        case 4u:
            if (read_uid(reader, node->node_uid) != 0) return -1;
            break;
        case 5u:
            if (read_type_value(reader, 0u, &node->boot_id) != 0) return -1;
            break;
        default:
            return -1;
        }
    }
    return seen == 0x3Eu ? 0 : -1;
}

int pwos_host_rpc_decode_topology(
    const uint8_t *payload,
    uint16_t payload_len,
    pwos_host_rpc_topology_t *out_topology)
{
    cbor_reader_t reader;
    uint32_t fields;
    uint32_t seen = 0u;
    uint32_t i;

    if (payload == NULL || payload_len == 0u || out_topology == NULL) {
        return -1;
    }
    memset(out_topology, 0, sizeof(*out_topology));
    reader.data = payload;
    reader.len = payload_len;
    reader.pos = 0u;
    if (read_type_value(&reader, 5u, &fields) != 0 || fields != 2u) {
        return -1;
    }
    for (i = 0u; i < fields; ++i) {
        uint32_t key;
        uint32_t count;
        uint32_t j;

        if (read_type_value(&reader, 0u, &key) != 0 || key < 1u || key > 2u ||
            (seen & (1u << key)) != 0u) {
            return -1;
        }
        seen |= 1u << key;
        if (key == 1u) {
            if (read_type_value(&reader, 0u, &out_topology->generation) != 0) {
                return -1;
            }
        } else {
            if (read_type_value(&reader, 4u, &count) != 0 ||
                count > PWOS_HOST_RPC_TOPOLOGY_MAX_NODES) {
                return -1;
            }
            out_topology->node_count = (uint8_t)count;
            for (j = 0u; j < count; ++j) {
                if (read_topology_node(&reader, &out_topology->nodes[j]) != 0) {
                    return -1;
                }
            }
        }
    }
    return seen == 0x06u && reader.pos == reader.len ? 0 : -1;
}
