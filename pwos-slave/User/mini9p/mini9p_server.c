#include "mini9p_server.h"

#include <stdio.h>
#include <string.h>

#define M9P_SERVER_IOUNIT 256u
#define M9P_SERVER_DIRBUF_CAP 128u
#define M9P_SERVER_FILEBUF_CAP 96u

#define M9P_NODE_PERM_READ 0x01u
#define M9P_NODE_PERM_WRITE 0x02u

enum m9p_server_node_index {
	M9P_NODE_ROOT = 0,
	M9P_NODE_SYS,
	M9P_NODE_SYS_VERSION,
	M9P_NODE_SYS_HEALTH,
	M9P_NODE_DEV,
	M9P_NODE_DEV_LED,
	M9P_NODE_DEV_TEMPERATURE,
	M9P_NODE_COUNT
};

struct m9p_server_node {
	const char *name;
	uint8_t qid_type;
	uint8_t perm;
	uint8_t stat_flags;
	uint8_t parent_index;
	const uint8_t *children;
	uint8_t child_count;
	bool readable;
	bool writable;
};

static const uint8_t root_children[] = { M9P_NODE_SYS, M9P_NODE_DEV };
static const uint8_t sys_children[] = { M9P_NODE_SYS_VERSION, M9P_NODE_SYS_HEALTH };
static const uint8_t dev_children[] = { M9P_NODE_DEV_LED, M9P_NODE_DEV_TEMPERATURE };

static const struct m9p_server_node server_nodes[M9P_NODE_COUNT] = {
	[M9P_NODE_ROOT] = {
		.name = "",
		.qid_type = M9P_QID_DIR,
		.perm = M9P_NODE_PERM_READ,
		.stat_flags = M9P_STAT_DIR,
		.parent_index = M9P_NODE_ROOT,
		.children = root_children,
		.child_count = (uint8_t)(sizeof(root_children) / sizeof(root_children[0])),
		.readable = true,
		.writable = false,
	},
	[M9P_NODE_SYS] = {
		.name = "sys",
		.qid_type = M9P_QID_DIR,
		.perm = M9P_NODE_PERM_READ,
		.stat_flags = M9P_STAT_DIR,
		.parent_index = M9P_NODE_ROOT,
		.children = sys_children,
		.child_count = (uint8_t)(sizeof(sys_children) / sizeof(sys_children[0])),
		.readable = true,
		.writable = false,
	},
	[M9P_NODE_SYS_VERSION] = {
		.name = "version",
		.qid_type = (uint8_t)(M9P_QID_VIRTUAL | M9P_QID_READONLY),
		.perm = M9P_NODE_PERM_READ,
		.stat_flags = M9P_STAT_VIRTUAL,
		.parent_index = M9P_NODE_SYS,
		.children = NULL,
		.child_count = 0u,
		.readable = true,
		.writable = false,
	},
	[M9P_NODE_SYS_HEALTH] = {
		.name = "health",
		.qid_type = (uint8_t)(M9P_QID_VIRTUAL | M9P_QID_READONLY),
		.perm = M9P_NODE_PERM_READ,
		.stat_flags = M9P_STAT_VIRTUAL,
		.parent_index = M9P_NODE_SYS,
		.children = NULL,
		.child_count = 0u,
		.readable = true,
		.writable = false,
	},
	[M9P_NODE_DEV] = {
		.name = "dev",
		.qid_type = M9P_QID_DIR,
		.perm = M9P_NODE_PERM_READ,
		.stat_flags = M9P_STAT_DIR,
		.parent_index = M9P_NODE_ROOT,
		.children = dev_children,
		.child_count = (uint8_t)(sizeof(dev_children) / sizeof(dev_children[0])),
		.readable = true,
		.writable = false,
	},
	[M9P_NODE_DEV_LED] = {
		.name = "led",
		.qid_type = M9P_QID_DEVICE,
		.perm = (uint8_t)(M9P_NODE_PERM_READ | M9P_NODE_PERM_WRITE),
		.stat_flags = M9P_STAT_DEVICE,
		.parent_index = M9P_NODE_DEV,
		.children = NULL,
		.child_count = 0u,
		.readable = true,
		.writable = true,
	},
	[M9P_NODE_DEV_TEMPERATURE] = {
		.name = "temperature",
		.qid_type = (uint8_t)(M9P_QID_DEVICE | M9P_QID_READONLY),
		.perm = M9P_NODE_PERM_READ,
		.stat_flags = M9P_STAT_DEVICE,
		.parent_index = M9P_NODE_DEV,
		.children = NULL,
		.child_count = 0u,
		.readable = true,
		.writable = false,
	},
};

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

static bool server_node_is_dir(uint8_t node_index)
{
	return (server_nodes[node_index].qid_type & M9P_QID_DIR) != 0u;
}

static struct m9p_qid server_node_qid(const struct m9p_server *server, uint8_t node_index)
{
	struct m9p_qid qid;

	qid.type = server_nodes[node_index].qid_type;
	qid.reserved = 0u;
	qid.version = (uint16_t)(node_index == M9P_NODE_DEV_LED ? server->led_version : 0u);
	qid.object_id = (uint32_t)node_index;
	return qid;
}

static uint8_t find_child(uint8_t parent_index, const char *name, size_t name_len)
{
	uint8_t idx;
	const struct m9p_server_node *parent = &server_nodes[parent_index];

	if (!server_node_is_dir(parent_index)) {
		return M9P_NODE_COUNT;
	}

	for (idx = 0u; idx < parent->child_count; ++idx) {
		uint8_t child_index = parent->children[idx];
		const char *child_name = server_nodes[child_index].name;

		if (strlen(child_name) == name_len && memcmp(child_name, name, name_len) == 0) {
			return child_index;
		}
	}

	return M9P_NODE_COUNT;
}

static uint8_t walk_path(uint8_t start_index, const char *path, size_t path_len, uint16_t *out_error)
{
	uint8_t current = start_index;
	size_t offset = 0u;

	if (path_len == 0u) {
		return current;
	}
	if (path[0] == '/') {
		current = M9P_NODE_ROOT;
	}

	while (offset < path_len) {
		size_t segment_start;
		size_t segment_len;
		uint8_t child_index;

		while (offset < path_len && path[offset] == '/') {
			++offset;
		}
		if (offset >= path_len) {
			break;
		}
		if (!server_node_is_dir(current)) {
			*out_error = M9P_ERR_ENOTDIR;
			return M9P_NODE_COUNT;
		}

		segment_start = offset;
		while (offset < path_len && path[offset] != '/') {
			++offset;
		}
		segment_len = offset - segment_start;
		if (segment_len == 0u || (segment_len == 1u && path[segment_start] == '.')) {
			continue;
		}
		if (segment_len == 2u && path[segment_start] == '.' && path[segment_start + 1u] == '.') {
			current = server_nodes[current].parent_index;
			continue;
		}

		child_index = find_child(current, path + segment_start, segment_len);
		if (child_index >= M9P_NODE_COUNT) {
			*out_error = M9P_ERR_ENOENT;
			return M9P_NODE_COUNT;
		}
		current = child_index;
	}

	return current;
}

static size_t append_dirent(uint8_t *buffer, size_t offset, size_t cap, uint8_t node_index, const struct m9p_server *server)
{
	const struct m9p_server_node *node = &server_nodes[node_index];
	const size_t name_len = strlen(node->name);
	const size_t entry_len = 11u + name_len;

	if (buffer != NULL) {
		struct m9p_qid qid;

		if (offset + entry_len > cap) {
			return 0u;
		}
		qid = server_node_qid(server, node_index);
		encode_qid(buffer + offset, &qid);
		buffer[offset + 8u] = node->perm;
		buffer[offset + 9u] = node->stat_flags;
		buffer[offset + 10u] = (uint8_t)name_len;
		if (name_len > 0u) {
			memcpy(buffer + offset + 11u, node->name, name_len);
		}
	}

	return entry_len;
}

static size_t serialize_directory(uint8_t node_index, uint8_t *buffer, size_t cap, const struct m9p_server *server)
{
	size_t offset = 0u;
	uint8_t idx;
	const struct m9p_server_node *node = &server_nodes[node_index];

	for (idx = 0u; idx < node->child_count; ++idx) {
		size_t entry_len = append_dirent(buffer, offset, cap, node->children[idx], server);

		if (entry_len == 0u) {
			return 0u;
		}
		offset += entry_len;
	}

	return offset;
}

static size_t build_file_text(const struct m9p_server *server, uint8_t node_index, char *buffer, size_t cap)
{
	int written;

	if (buffer == NULL || cap == 0u) {
		return 0u;
	}

	switch (node_index) {
	case M9P_NODE_SYS_VERSION:
		written = snprintf(buffer, cap, "9P-Walkers slave v1\n");
		break;
	case M9P_NODE_SYS_HEALTH:
		written = snprintf(
			buffer,
			cap,
			"status=ok led=%u temp=%d.%dC\n",
			(unsigned)server->led_state,
			(int)(server->temperature_deci_celsius / 10),
			(int)(server->temperature_deci_celsius % 10));
		break;
	case M9P_NODE_DEV_LED:
		written = snprintf(buffer, cap, "%u\n", (unsigned)server->led_state);
		break;
	case M9P_NODE_DEV_TEMPERATURE:
		written = snprintf(
			buffer,
			cap,
			"%d.%d\n",
			(int)(server->temperature_deci_celsius / 10),
			(int)(server->temperature_deci_celsius % 10));
		break;
	default:
		written = 0;
		break;
	}

	if (written < 0) {
		return 0u;
	}
	if ((size_t)written >= cap) {
		return cap - 1u;
	}

	return (size_t)written;
}

static uint32_t node_size(const struct m9p_server *server, uint8_t node_index)
{
	if (server_node_is_dir(node_index)) {
		return (uint32_t)serialize_directory(node_index, NULL, 0u, server);
	}

	{
		char buffer[M9P_SERVER_FILEBUF_CAP];
		return (uint32_t)build_file_text(server, node_index, buffer, sizeof(buffer));
	}
}

static bool fid_valid(const struct m9p_server *server, uint16_t fid)
{
	return fid < server->max_fids && server->fids[fid].in_use;
}

static bool mode_allows_read(uint8_t mode)
{
	uint8_t base_mode = (uint8_t)(mode & 0x03u);
	return base_mode == M9P_OREAD || base_mode == M9P_ORDWR;
}

static bool mode_allows_write(uint8_t mode)
{
	uint8_t base_mode = (uint8_t)(mode & 0x03u);
	return base_mode == M9P_OWRITE || base_mode == M9P_ORDWR;
}

static bool node_mode_supported(uint8_t node_index, uint8_t mode)
{
	const struct m9p_server_node *node = &server_nodes[node_index];

	if (server_node_is_dir(node_index)) {
		return (mode & 0x03u) == M9P_OREAD;
	}
	if (mode_allows_read(mode) && !node->readable) {
		return false;
	}
	if (mode_allows_write(mode) && !node->writable) {
		return false;
	}
	return true;
}

static bool build_response(
	uint8_t type,
	uint16_t tag,
	const uint8_t *payload,
	uint16_t payload_len,
	uint8_t *response_frame,
	size_t response_cap,
	size_t *response_len)
{
	return m9p_encode_frame(type, tag, payload, payload_len, response_frame, response_cap, response_len);
}

static bool build_error_response(
	uint16_t tag,
	uint16_t code,
	const char *message,
	uint8_t *response_frame,
	size_t response_cap,
	size_t *response_len)
{
	uint8_t payload[3u + M9P_MAX_ERROR_TEXT];
	size_t message_len = strlen(message);

	if (message_len > M9P_MAX_ERROR_TEXT) {
		message_len = M9P_MAX_ERROR_TEXT;
	}
	put_le16(payload, code);
	payload[2] = (uint8_t)message_len;
	if (message_len > 0u) {
		memcpy(payload + 3, message, message_len);
	}
	return build_response(M9P_RERROR, tag, payload, (uint16_t)(3u + message_len), response_frame, response_cap, response_len);
}

static bool handle_attach(
	struct m9p_server *server,
	const struct m9p_frame_view *frame,
	uint8_t *response_frame,
	size_t response_cap,
	size_t *response_len)
{
	uint8_t payload[16];
	uint16_t fid;
	uint16_t requested_msize;
	uint8_t requested_inflight;

	if (frame->payload_len != 6u) {
		return build_error_response(frame->tag, M9P_ERR_EINVAL, "invalid attach", response_frame, response_cap, response_len);
	}

	fid = get_le16(frame->payload);
	requested_msize = get_le16(frame->payload + 2u);
	requested_inflight = frame->payload[4];
	if (fid >= M9P_SERVER_MAX_FIDS || requested_msize < M9P_FRAME_OVERHEAD) {
		return build_error_response(frame->tag, M9P_ERR_EINVAL, "bad attach args", response_frame, response_cap, response_len);
	}

	m9p_server_reset_session(server);
	server->attached = true;
	server->negotiated_msize = requested_msize > M9P_SERVER_FRAME_CAP ? M9P_SERVER_FRAME_CAP : requested_msize;
	server->max_fids = M9P_SERVER_MAX_FIDS;
	server->max_inflight = requested_inflight == 0u ? 1u : requested_inflight;
	if (server->max_inflight > 4u) {
		server->max_inflight = 4u;
	}
	server->feature_bits = M9P_FEATURE_DIRECTORY_READ;
	server->fids[fid].in_use = true;
	server->fids[fid].opened = false;
	server->fids[fid].open_mode = M9P_OREAD;
	server->fids[fid].node_index = M9P_NODE_ROOT;

	put_le16(payload, server->negotiated_msize);
	payload[2] = server->max_fids;
	payload[3] = server->max_inflight;
	put_le32(payload + 4u, server->feature_bits);
	{
		struct m9p_qid root_qid = server_node_qid(server, M9P_NODE_ROOT);
		encode_qid(payload + 8u, &root_qid);
	}

	return build_response(M9P_RATTACH, frame->tag, payload, sizeof(payload), response_frame, response_cap, response_len);
}

static bool handle_walk(
	struct m9p_server *server,
	const struct m9p_frame_view *frame,
	uint8_t *response_frame,
	size_t response_cap,
	size_t *response_len)
{
	uint16_t fid;
	uint16_t newfid;
	uint8_t path_len;
	uint16_t error_code = M9P_ERR_EINVAL;
	uint8_t node_index;
	uint8_t payload[8];

	if (!server->attached) {
		return build_error_response(frame->tag, M9P_ERR_EPERM, "not attached", response_frame, response_cap, response_len);
	}
	if (frame->payload_len < 5u) {
		return build_error_response(frame->tag, M9P_ERR_EINVAL, "invalid walk", response_frame, response_cap, response_len);
	}

	fid = get_le16(frame->payload);
	newfid = get_le16(frame->payload + 2u);
	path_len = frame->payload[4];
	if ((uint16_t)(5u + path_len) != frame->payload_len) {
		return build_error_response(frame->tag, M9P_ERR_EINVAL, "walk len mismatch", response_frame, response_cap, response_len);
	}
	if (!fid_valid(server, fid) || newfid >= server->max_fids) {
		return build_error_response(frame->tag, M9P_ERR_EFID, "unknown fid", response_frame, response_cap, response_len);
	}
	if (server->fids[newfid].in_use && newfid != fid) {
		return build_error_response(frame->tag, M9P_ERR_EFID, "newfid busy", response_frame, response_cap, response_len);
	}

	node_index = walk_path(server->fids[fid].node_index, (const char *)(frame->payload + 5u), path_len, &error_code);
	if (node_index >= M9P_NODE_COUNT) {
		return build_error_response(frame->tag, error_code, m9p_error_name(error_code), response_frame, response_cap, response_len);
	}

	server->fids[newfid].in_use = true;
	server->fids[newfid].opened = false;
	server->fids[newfid].open_mode = M9P_OREAD;
	server->fids[newfid].node_index = node_index;

	{
		struct m9p_qid qid = server_node_qid(server, node_index);
		encode_qid(payload, &qid);
	}
	return build_response(M9P_RWALK, frame->tag, payload, sizeof(payload), response_frame, response_cap, response_len);
}

static bool handle_open(
	struct m9p_server *server,
	const struct m9p_frame_view *frame,
	uint8_t *response_frame,
	size_t response_cap,
	size_t *response_len)
{
	uint16_t fid;
	uint8_t mode;
	uint8_t payload[10];
	uint8_t node_index;

	if (frame->payload_len != 3u) {
		return build_error_response(frame->tag, M9P_ERR_EINVAL, "invalid open", response_frame, response_cap, response_len);
	}

	fid = get_le16(frame->payload);
	mode = frame->payload[2];
	if (!fid_valid(server, fid)) {
		return build_error_response(frame->tag, M9P_ERR_EFID, "unknown fid", response_frame, response_cap, response_len);
	}

	node_index = server->fids[fid].node_index;
	if (!node_mode_supported(node_index, mode)) {
		return build_error_response(frame->tag, M9P_ERR_EPERM, "mode denied", response_frame, response_cap, response_len);
	}

	server->fids[fid].opened = true;
	server->fids[fid].open_mode = mode;

	{
		struct m9p_qid qid = server_node_qid(server, node_index);
		encode_qid(payload, &qid);
	}
	put_le16(payload + 8u, M9P_SERVER_IOUNIT);
	return build_response(M9P_ROPEN, frame->tag, payload, sizeof(payload), response_frame, response_cap, response_len);
}

static bool handle_read(
	struct m9p_server *server,
	const struct m9p_frame_view *frame,
	uint8_t *response_frame,
	size_t response_cap,
	size_t *response_len)
{
	uint16_t fid;
	uint32_t offset;
	uint16_t count;
	uint8_t node_index;
	uint16_t actual_count;
	uint8_t payload[2u + M9P_SERVER_IOUNIT];
	size_t data_len = 0u;

	if (frame->payload_len != 8u) {
		return build_error_response(frame->tag, M9P_ERR_EINVAL, "invalid read", response_frame, response_cap, response_len);
	}

	fid = get_le16(frame->payload);
	offset = get_le32(frame->payload + 2u);
	count = get_le16(frame->payload + 6u);
	if (!fid_valid(server, fid)) {
		return build_error_response(frame->tag, M9P_ERR_EFID, "unknown fid", response_frame, response_cap, response_len);
	}

	node_index = server->fids[fid].node_index;
	if (!server->fids[fid].opened || !mode_allows_read(server->fids[fid].open_mode)) {
		return build_error_response(frame->tag, M9P_ERR_EPERM, "fid not readable", response_frame, response_cap, response_len);
	}

	if (server_node_is_dir(node_index)) {
		uint8_t directory_data[M9P_SERVER_DIRBUF_CAP];

		data_len = serialize_directory(node_index, directory_data, sizeof(directory_data), server);
		if (data_len == 0u && server_nodes[node_index].child_count > 0u) {
			return build_error_response(frame->tag, M9P_ERR_EMSIZE, "dir too large", response_frame, response_cap, response_len);
		}
		if (offset > data_len) {
			actual_count = 0u;
		} else {
			size_t remaining = data_len - offset;
			actual_count = (uint16_t)(remaining < count ? remaining : count);
			memcpy(payload + 2u, directory_data + offset, actual_count);
		}
	} else {
		char file_data[M9P_SERVER_FILEBUF_CAP];

		data_len = build_file_text(server, node_index, file_data, sizeof(file_data));
		if (offset > data_len) {
			actual_count = 0u;
		} else {
			size_t remaining = data_len - offset;
			actual_count = (uint16_t)(remaining < count ? remaining : count);
			memcpy(payload + 2u, file_data + offset, actual_count);
		}
	}

	put_le16(payload, actual_count);
	return build_response(M9P_RREAD, frame->tag, payload, (uint16_t)(2u + actual_count), response_frame, response_cap, response_len);
}

static bool parse_led_state(const uint8_t *data, uint16_t count, uint8_t *out_led_state)
{
	uint16_t offset = 0u;

	while (offset < count && (data[offset] == ' ' || data[offset] == '\t' || data[offset] == '\r' || data[offset] == '\n')) {
		++offset;
	}
	if (offset >= count) {
		return false;
	}
	if (data[offset] == '0') {
		*out_led_state = 0u;
		return true;
	}
	if (data[offset] == '1') {
		*out_led_state = 1u;
		return true;
	}
	return false;
}

static bool handle_write(
	struct m9p_server *server,
	const struct m9p_frame_view *frame,
	uint8_t *response_frame,
	size_t response_cap,
	size_t *response_len)
{
	uint16_t fid;
	uint32_t offset;
	uint16_t count;
	uint8_t node_index;
	uint8_t led_state;
	uint8_t payload[2];

	if (frame->payload_len < 8u) {
		return build_error_response(frame->tag, M9P_ERR_EINVAL, "invalid write", response_frame, response_cap, response_len);
	}

	fid = get_le16(frame->payload);
	offset = get_le32(frame->payload + 2u);
	count = get_le16(frame->payload + 6u);
	if ((uint16_t)(8u + count) != frame->payload_len) {
		return build_error_response(frame->tag, M9P_ERR_EINVAL, "write len mismatch", response_frame, response_cap, response_len);
	}
	if (!fid_valid(server, fid)) {
		return build_error_response(frame->tag, M9P_ERR_EFID, "unknown fid", response_frame, response_cap, response_len);
	}

	node_index = server->fids[fid].node_index;
	if (!server->fids[fid].opened || !mode_allows_write(server->fids[fid].open_mode)) {
		return build_error_response(frame->tag, M9P_ERR_EPERM, "fid not writable", response_frame, response_cap, response_len);
	}
	if (server_node_is_dir(node_index)) {
		return build_error_response(frame->tag, M9P_ERR_EISDIR, "cannot write dir", response_frame, response_cap, response_len);
	}
	if (node_index != M9P_NODE_DEV_LED) {
		return build_error_response(frame->tag, M9P_ERR_EPERM, "readonly node", response_frame, response_cap, response_len);
	}
	if (offset != 0u) {
		return build_error_response(frame->tag, M9P_ERR_EOFFS, "bad offset", response_frame, response_cap, response_len);
	}
	if (!parse_led_state(frame->payload + 8u, count, &led_state)) {
		return build_error_response(frame->tag, M9P_ERR_EINVAL, "invalid led value", response_frame, response_cap, response_len);
	}

	if (server->led_state != led_state) {
		server->led_state = led_state;
		++server->led_version;
	}

	put_le16(payload, count);
	return build_response(M9P_RWRITE, frame->tag, payload, sizeof(payload), response_frame, response_cap, response_len);
}

static bool handle_stat(
	struct m9p_server *server,
	const struct m9p_frame_view *frame,
	uint8_t *response_frame,
	size_t response_cap,
	size_t *response_len)
{
	uint16_t fid;
	uint8_t node_index;
	const struct m9p_server_node *node;
	uint8_t payload[19u + M9P_MAX_NAME_LEN];
	size_t name_len;

	if (frame->payload_len != 2u) {
		return build_error_response(frame->tag, M9P_ERR_EINVAL, "invalid stat", response_frame, response_cap, response_len);
	}

	fid = get_le16(frame->payload);
	if (!fid_valid(server, fid)) {
		return build_error_response(frame->tag, M9P_ERR_EFID, "unknown fid", response_frame, response_cap, response_len);
	}

	node_index = server->fids[fid].node_index;
	node = &server_nodes[node_index];
	{
		struct m9p_qid qid = server_node_qid(server, node_index);
		encode_qid(payload, &qid);
	}
	payload[8] = node->perm;
	payload[9] = node->stat_flags;
	put_le32(payload + 10u, node_size(server, node_index));
	put_le32(payload + 14u, (uint32_t)(node_index == M9P_NODE_DEV_LED ? server->led_version : 0u));
	name_len = strlen(node->name);
	if (name_len > M9P_MAX_NAME_LEN) {
		name_len = M9P_MAX_NAME_LEN;
	}
	payload[18] = (uint8_t)name_len;
	if (name_len > 0u) {
		memcpy(payload + 19u, node->name, name_len);
	}

	return build_response(M9P_RSTAT, frame->tag, payload, (uint16_t)(19u + name_len), response_frame, response_cap, response_len);
}

static bool handle_clunk(
	struct m9p_server *server,
	const struct m9p_frame_view *frame,
	uint8_t *response_frame,
	size_t response_cap,
	size_t *response_len)
{
	uint16_t fid;

	if (frame->payload_len != 2u) {
		return build_error_response(frame->tag, M9P_ERR_EINVAL, "invalid clunk", response_frame, response_cap, response_len);
	}

	fid = get_le16(frame->payload);
	if (!fid_valid(server, fid)) {
		return build_error_response(frame->tag, M9P_ERR_EFID, "unknown fid", response_frame, response_cap, response_len);
	}

	server->fids[fid].in_use = false;
	server->fids[fid].opened = false;
	server->fids[fid].open_mode = 0u;
	server->fids[fid].node_index = M9P_NODE_ROOT;

	return build_response(M9P_RCLUNK, frame->tag, NULL, 0u, response_frame, response_cap, response_len);
}

void m9p_server_init(struct m9p_server *server)
{
	if (server == NULL) {
		return;
	}

	memset(server, 0, sizeof(*server));
	server->negotiated_msize = M9P_SERVER_FRAME_CAP;
	server->max_fids = M9P_SERVER_MAX_FIDS;
	server->max_inflight = 1u;
	server->feature_bits = M9P_FEATURE_DIRECTORY_READ;
	server->temperature_deci_celsius = 253;
}

void m9p_server_reset_session(struct m9p_server *server)
{
	if (server == NULL) {
		return;
	}

	memset(server->fids, 0, sizeof(server->fids));
}

void m9p_server_set_temperature(struct m9p_server *server, int16_t temperature_deci_celsius)
{
	if (server == NULL) {
		return;
	}

	server->temperature_deci_celsius = temperature_deci_celsius;
}

uint8_t m9p_server_get_led_state(const struct m9p_server *server)
{
	if (server == NULL) {
		return 0u;
	}

	return server->led_state;
}

bool m9p_server_handle_frame(
	struct m9p_server *server,
	const uint8_t *request_frame,
	size_t request_len,
	uint8_t *response_frame,
	size_t response_cap,
	size_t *response_len)
{
	struct m9p_frame_view frame;

	if (response_len != NULL) {
		*response_len = 0u;
	}
	if (server == NULL || request_frame == NULL || response_frame == NULL || response_len == NULL) {
		return false;
	}
	if (!m9p_decode_frame(request_frame, request_len, &frame)) {
		return false;
	}
	if (frame.version != M9P_VERSION) {
		return build_error_response(frame.tag, M9P_ERR_EINVAL, "bad version", response_frame, response_cap, response_len);
	}

	switch (frame.type) {
	case M9P_TATTACH:
		return handle_attach(server, &frame, response_frame, response_cap, response_len);
	case M9P_TWALK:
		return handle_walk(server, &frame, response_frame, response_cap, response_len);
	case M9P_TOPEN:
		return handle_open(server, &frame, response_frame, response_cap, response_len);
	case M9P_TREAD:
		return handle_read(server, &frame, response_frame, response_cap, response_len);
	case M9P_TWRITE:
		return handle_write(server, &frame, response_frame, response_cap, response_len);
	case M9P_TSTAT:
		return handle_stat(server, &frame, response_frame, response_cap, response_len);
	case M9P_TCLUNK:
		return handle_clunk(server, &frame, response_frame, response_cap, response_len);
	default:
		return build_error_response(frame.tag, M9P_ERR_ENOTSUP, "unsupported type", response_frame, response_cap, response_len);
	}
}

