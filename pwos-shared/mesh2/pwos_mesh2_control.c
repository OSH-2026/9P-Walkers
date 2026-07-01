#include "pwos_mesh2_control.h"

#include <string.h>

static void put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static uint16_t get_le16(const uint8_t *src)
{
    return (uint16_t)src[0] | (uint16_t)((uint16_t)src[1] << 8);
}

static void put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] |
        ((uint32_t)src[1] << 8) |
        ((uint32_t)src[2] << 16) |
        ((uint32_t)src[3] << 24);
}

static void put_le64(uint8_t *dst, uint64_t value)
{
    put_le32(dst, (uint32_t)value);
    put_le32(dst + 4u, (uint32_t)(value >> 32));
}

static uint64_t get_le64(const uint8_t *src)
{
    return (uint64_t)get_le32(src) |
        ((uint64_t)get_le32(src + 4u) << 32);
}

static pwos_status_t check_encode_args(
    const void *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t required_len,
    size_t *out_len)
{
    if (msg == NULL || payload == NULL || out_len == NULL) {
        return PWOS_E_NO_MEMORY;
    }
    if (payload_cap < required_len) {
        return PWOS_E_NO_MEMORY;
    }
    return PWOS_OK;
}

static pwos_status_t check_decode_args(
    const uint8_t *payload,
    size_t payload_len,
    size_t required_len,
    void *out_msg)
{
    if (payload == NULL || out_msg == NULL) {
        return PWOS_E_NO_MEMORY;
    }
    if (payload_len != required_len || payload[0] != PWOS_MESH2_CONTROL_VERSION) {
        return PWOS_E_BAD_LENGTH;
    }
    return PWOS_OK;
}

pwos_status_t pwos_mesh2_encode_node_register(
    const pwos_mesh2_node_register_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len)
{
    pwos_status_t status;

    status = check_encode_args(
        msg, payload, payload_cap, PWOS_MESH2_NODE_REGISTER_PAYLOAD_LEN, out_len);
    if (status != PWOS_OK) {
        return status;
    }

    memset(payload, 0, PWOS_MESH2_NODE_REGISTER_PAYLOAD_LEN);
    payload[0] = PWOS_MESH2_CONTROL_VERSION;
    payload[1] = msg->upstream_port;
    put_le32(payload + 4u, msg->caps);
    put_le32(payload + 8u, msg->boot_id);
    put_le32(payload + 12u, msg->uid[0]);
    put_le32(payload + 16u, msg->uid[1]);
    put_le32(payload + 20u, msg->uid[2]);
    *out_len = PWOS_MESH2_NODE_REGISTER_PAYLOAD_LEN;
    return PWOS_OK;
}

pwos_status_t pwos_mesh2_decode_node_register(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_node_register_t *out_msg)
{
    pwos_status_t status;

    status = check_decode_args(
        payload, payload_len, PWOS_MESH2_NODE_REGISTER_PAYLOAD_LEN, out_msg);
    if (status != PWOS_OK) {
        return status;
    }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->upstream_port = payload[1];
    out_msg->caps = get_le32(payload + 4u);
    out_msg->boot_id = get_le32(payload + 8u);
    out_msg->uid[0] = get_le32(payload + 12u);
    out_msg->uid[1] = get_le32(payload + 16u);
    out_msg->uid[2] = get_le32(payload + 20u);
    return PWOS_OK;
}

pwos_status_t pwos_mesh2_encode_addr_assign(
    const pwos_mesh2_addr_assign_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len)
{
    pwos_status_t status;

    status = check_encode_args(
        msg, payload, payload_cap, PWOS_MESH2_ADDR_ASSIGN_PAYLOAD_LEN, out_len);
    if (status != PWOS_OK) {
        return status;
    }

    memset(payload, 0, PWOS_MESH2_ADDR_ASSIGN_PAYLOAD_LEN);
    payload[0] = PWOS_MESH2_CONTROL_VERSION;
    payload[1] = msg->addr;
    payload[2] = msg->flags;
    put_le32(payload + 4u, msg->lease_epoch);
    put_le32(payload + 8u, msg->lease_ms);
    put_le32(payload + 12u, msg->boot_id);
    put_le32(payload + 16u, msg->uid[0]);
    put_le32(payload + 20u, msg->uid[1]);
    put_le32(payload + 24u, msg->uid[2]);
    *out_len = PWOS_MESH2_ADDR_ASSIGN_PAYLOAD_LEN;
    return PWOS_OK;
}

pwos_status_t pwos_mesh2_decode_addr_assign(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_addr_assign_t *out_msg)
{
    pwos_status_t status;

    status = check_decode_args(
        payload, payload_len, PWOS_MESH2_ADDR_ASSIGN_PAYLOAD_LEN, out_msg);
    if (status != PWOS_OK) {
        return status;
    }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->addr = payload[1];
    out_msg->flags = payload[2];
    out_msg->lease_epoch = get_le32(payload + 4u);
    out_msg->lease_ms = get_le32(payload + 8u);
    out_msg->boot_id = get_le32(payload + 12u);
    out_msg->uid[0] = get_le32(payload + 16u);
    out_msg->uid[1] = get_le32(payload + 20u);
    out_msg->uid[2] = get_le32(payload + 24u);
    return PWOS_OK;
}

pwos_status_t pwos_mesh2_encode_lease_renew(
    const pwos_mesh2_lease_renew_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len)
{
    pwos_status_t status;

    status = check_encode_args(
        msg, payload, payload_cap, PWOS_MESH2_LEASE_RENEW_PAYLOAD_LEN, out_len);
    if (status != PWOS_OK) {
        return status;
    }

    memset(payload, 0, PWOS_MESH2_LEASE_RENEW_PAYLOAD_LEN);
    payload[0] = PWOS_MESH2_CONTROL_VERSION;
    payload[1] = msg->addr;
    put_le32(payload + 4u, msg->lease_epoch);
    put_le32(payload + 8u, msg->boot_id);
    put_le32(payload + 12u, msg->uid[0]);
    put_le32(payload + 16u, msg->uid[1]);
    put_le32(payload + 20u, msg->uid[2]);
    *out_len = PWOS_MESH2_LEASE_RENEW_PAYLOAD_LEN;
    return PWOS_OK;
}

pwos_status_t pwos_mesh2_decode_lease_renew(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_lease_renew_t *out_msg)
{
    pwos_status_t status;

    status = check_decode_args(
        payload, payload_len, PWOS_MESH2_LEASE_RENEW_PAYLOAD_LEN, out_msg);
    if (status != PWOS_OK) {
        return status;
    }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->addr = payload[1];
    out_msg->lease_epoch = get_le32(payload + 4u);
    out_msg->boot_id = get_le32(payload + 8u);
    out_msg->uid[0] = get_le32(payload + 12u);
    out_msg->uid[1] = get_le32(payload + 16u);
    out_msg->uid[2] = get_le32(payload + 20u);
    return PWOS_OK;
}

pwos_status_t pwos_mesh2_encode_lease_ack(
    const pwos_mesh2_lease_ack_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len)
{
    return pwos_mesh2_encode_addr_assign(msg, payload, payload_cap, out_len);
}

pwos_status_t pwos_mesh2_decode_lease_ack(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_lease_ack_t *out_msg)
{
    return pwos_mesh2_decode_addr_assign(payload, payload_len, out_msg);
}

pwos_status_t pwos_mesh2_encode_link_state(
    const pwos_mesh2_link_state_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len)
{
    pwos_status_t status;

    status = check_encode_args(
        msg, payload, payload_cap, PWOS_MESH2_LINK_STATE_PAYLOAD_LEN, out_len);
    if (status != PWOS_OK) {
        return status;
    }

    memset(payload, 0, PWOS_MESH2_LINK_STATE_PAYLOAD_LEN);
    payload[0] = PWOS_MESH2_CONTROL_VERSION;
    payload[1] = msg->local_addr;
    payload[2] = msg->local_port;
    payload[3] = msg->peer_addr;
    payload[4] = msg->peer_port;
    payload[5] = msg->flags;
    put_le16(payload + 6u, msg->metric);
    put_le32(payload + 8u, msg->peer_boot_id);
    put_le32(payload + 12u, msg->peer_uid[0]);
    put_le32(payload + 16u, msg->peer_uid[1]);
    put_le32(payload + 20u, msg->peer_uid[2]);
    *out_len = PWOS_MESH2_LINK_STATE_PAYLOAD_LEN;
    return PWOS_OK;
}

pwos_status_t pwos_mesh2_decode_link_state(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_link_state_t *out_msg)
{
    pwos_status_t status;

    status = check_decode_args(
        payload, payload_len, PWOS_MESH2_LINK_STATE_PAYLOAD_LEN, out_msg);
    if (status != PWOS_OK) {
        return status;
    }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->local_addr = payload[1];
    out_msg->local_port = payload[2];
    out_msg->peer_addr = payload[3];
    out_msg->peer_port = payload[4];
    out_msg->flags = payload[5];
    out_msg->metric = get_le16(payload + 6u);
    out_msg->peer_boot_id = get_le32(payload + 8u);
    out_msg->peer_uid[0] = get_le32(payload + 12u);
    out_msg->peer_uid[1] = get_le32(payload + 16u);
    out_msg->peer_uid[2] = get_le32(payload + 20u);
    return PWOS_OK;
}

pwos_status_t pwos_mesh2_encode_route_update(
    const pwos_mesh2_route_update_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len)
{
    pwos_status_t status;

    status = check_encode_args(
        msg, payload, payload_cap, PWOS_MESH2_ROUTE_UPDATE_PAYLOAD_LEN, out_len);
    if (status != PWOS_OK) {
        return status;
    }
    if (msg->action != PWOS_MESH2_ROUTE_SET && msg->action != PWOS_MESH2_ROUTE_DELETE) {
        return PWOS_E_BAD_LENGTH;
    }

    memset(payload, 0, PWOS_MESH2_ROUTE_UPDATE_PAYLOAD_LEN);
    payload[0] = PWOS_MESH2_CONTROL_VERSION;
    payload[1] = msg->dst;
    payload[2] = msg->next_hop;
    payload[3] = msg->action;
    put_le32(payload + 4u, msg->route_version);
    put_le16(payload + 8u, msg->metric);
    *out_len = PWOS_MESH2_ROUTE_UPDATE_PAYLOAD_LEN;
    return PWOS_OK;
}

pwos_status_t pwos_mesh2_decode_route_update(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_route_update_t *out_msg)
{
    pwos_status_t status;

    status = check_decode_args(
        payload, payload_len, PWOS_MESH2_ROUTE_UPDATE_PAYLOAD_LEN, out_msg);
    if (status != PWOS_OK) {
        return status;
    }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->dst = payload[1];
    out_msg->next_hop = payload[2];
    out_msg->action = payload[3];
    out_msg->route_version = get_le32(payload + 4u);
    out_msg->metric = get_le16(payload + 8u);
    if (out_msg->action != PWOS_MESH2_ROUTE_SET &&
        out_msg->action != PWOS_MESH2_ROUTE_DELETE) {
        return PWOS_E_BAD_LENGTH;
    }
    return PWOS_OK;
}

pwos_status_t pwos_mesh2_encode_host_advertise(
    const pwos_mesh2_host_advertise_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len)
{
    pwos_status_t status = check_encode_args(
        msg, payload, payload_cap,
        PWOS_MESH2_HOST_ADVERTISE_PAYLOAD_LEN, out_len);

    if (status != PWOS_OK) {
        return status;
    }
    if (msg->role > PWOS_MESH2_HOST_ROLE_LEADER) {
        return PWOS_E_BAD_LENGTH;
    }
    memset(payload, 0, PWOS_MESH2_HOST_ADVERTISE_PAYLOAD_LEN);
    payload[0] = PWOS_MESH2_CONTROL_VERSION;
    payload[1] = msg->role;
    payload[2] = msg->flags;
    put_le32(payload + 4u, msg->epoch);
    put_le16(payload + 8u, msg->priority);
    put_le32(payload + 12u, msg->host_uid[0]);
    put_le32(payload + 16u, msg->host_uid[1]);
    put_le32(payload + 20u, msg->host_uid[2]);
    put_le32(payload + 24u, msg->cluster_id);
    *out_len = PWOS_MESH2_HOST_ADVERTISE_PAYLOAD_LEN;
    return PWOS_OK;
}

pwos_status_t pwos_mesh2_decode_host_advertise(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_host_advertise_t *out_msg)
{
    pwos_status_t status = check_decode_args(
        payload, payload_len,
        PWOS_MESH2_HOST_ADVERTISE_PAYLOAD_LEN, out_msg);

    if (status != PWOS_OK) {
        return status;
    }
    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->role = payload[1];
    out_msg->flags = payload[2];
    out_msg->epoch = get_le32(payload + 4u);
    out_msg->priority = get_le16(payload + 8u);
    out_msg->host_uid[0] = get_le32(payload + 12u);
    out_msg->host_uid[1] = get_le32(payload + 16u);
    out_msg->host_uid[2] = get_le32(payload + 20u);
    out_msg->cluster_id = get_le32(payload + 24u);
    if (out_msg->role > PWOS_MESH2_HOST_ROLE_LEADER) {
        return PWOS_E_BAD_LENGTH;
    }
    return PWOS_OK;
}

pwos_status_t pwos_mesh2_encode_time_sync(
    const pwos_mesh2_time_sync_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len)
{
    pwos_status_t status = check_encode_args(
        msg, payload, payload_cap, PWOS_MESH2_TIME_SYNC_PAYLOAD_LEN, out_len);

    if (status != PWOS_OK) {
        return status;
    }
    if ((msg->kind != PWOS_MESH2_TIME_SYNC_REQUEST &&
         msg->kind != PWOS_MESH2_TIME_SYNC_RESPONSE) ||
        msg->sequence == 0u || msg->client_tx_mono_us == 0u ||
        (msg->flags & ~PWOS_MESH2_TIME_SYNC_FLAG_WALL_VALID) != 0u ||
        (msg->kind == PWOS_MESH2_TIME_SYNC_REQUEST &&
         (msg->flags != 0u || msg->server_rx_unix_us != 0u ||
          msg->server_tx_unix_us != 0u)) ||
        (msg->kind == PWOS_MESH2_TIME_SYNC_RESPONSE &&
         (((msg->flags & PWOS_MESH2_TIME_SYNC_FLAG_WALL_VALID) != 0u &&
           (msg->server_rx_unix_us == 0u || msg->server_tx_unix_us == 0u)) ||
          (msg->flags == 0u &&
           (msg->server_rx_unix_us != 0u || msg->server_tx_unix_us != 0u))))) {
        return PWOS_E_BAD_LENGTH;
    }

    memset(payload, 0, PWOS_MESH2_TIME_SYNC_PAYLOAD_LEN);
    payload[0] = PWOS_MESH2_CONTROL_VERSION;
    payload[1] = msg->kind;
    payload[2] = msg->flags;
    put_le32(payload + 4u, msg->sequence);
    put_le64(payload + 8u, msg->client_tx_mono_us);
    put_le64(payload + 16u, msg->server_rx_unix_us);
    put_le64(payload + 24u, msg->server_tx_unix_us);
    *out_len = PWOS_MESH2_TIME_SYNC_PAYLOAD_LEN;
    return PWOS_OK;
}

pwos_status_t pwos_mesh2_decode_time_sync(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_time_sync_t *out_msg)
{
    pwos_status_t status = check_decode_args(
        payload, payload_len, PWOS_MESH2_TIME_SYNC_PAYLOAD_LEN, out_msg);

    if (status != PWOS_OK) {
        return status;
    }
    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->kind = payload[1];
    out_msg->flags = payload[2];
    out_msg->sequence = get_le32(payload + 4u);
    out_msg->client_tx_mono_us = get_le64(payload + 8u);
    out_msg->server_rx_unix_us = get_le64(payload + 16u);
    out_msg->server_tx_unix_us = get_le64(payload + 24u);
    if (payload[3] != 0u ||
        (out_msg->kind != PWOS_MESH2_TIME_SYNC_REQUEST &&
         out_msg->kind != PWOS_MESH2_TIME_SYNC_RESPONSE) ||
        out_msg->sequence == 0u || out_msg->client_tx_mono_us == 0u ||
        (out_msg->flags & ~PWOS_MESH2_TIME_SYNC_FLAG_WALL_VALID) != 0u ||
        (out_msg->kind == PWOS_MESH2_TIME_SYNC_REQUEST &&
         (out_msg->flags != 0u || out_msg->server_rx_unix_us != 0u ||
          out_msg->server_tx_unix_us != 0u)) ||
        (out_msg->kind == PWOS_MESH2_TIME_SYNC_RESPONSE &&
         (((out_msg->flags & PWOS_MESH2_TIME_SYNC_FLAG_WALL_VALID) != 0u &&
           (out_msg->server_rx_unix_us == 0u ||
            out_msg->server_tx_unix_us == 0u)) ||
          (out_msg->flags == 0u &&
           (out_msg->server_rx_unix_us != 0u ||
            out_msg->server_tx_unix_us != 0u))))) {
        return PWOS_E_BAD_LENGTH;
    }
    return PWOS_OK;
}
