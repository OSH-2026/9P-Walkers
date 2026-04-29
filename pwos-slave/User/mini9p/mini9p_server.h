#ifndef MINI9P_SERVER_H
#define MINI9P_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mini9p_protocol.h"

#define M9P_SERVER_MAX_FIDS 16u
#define M9P_SERVER_FRAME_CAP 512u

struct m9p_server_fid_state {
	bool in_use;
	bool opened;
	uint8_t open_mode;
	uint8_t node_index;
};

struct m9p_server {
	bool attached;
	uint16_t negotiated_msize;
	uint8_t max_fids;
	uint8_t max_inflight;
	uint32_t feature_bits;
	uint8_t led_state;
	int16_t temperature_deci_celsius;
	uint16_t led_version;
	struct m9p_server_fid_state fids[M9P_SERVER_MAX_FIDS];
};

void m9p_server_init(struct m9p_server *server);
void m9p_server_reset_session(struct m9p_server *server);
void m9p_server_set_temperature(struct m9p_server *server, int16_t temperature_deci_celsius);
uint8_t m9p_server_get_led_state(const struct m9p_server *server);
bool m9p_server_handle_frame(
	struct m9p_server *server,
	const uint8_t *request_frame,
	size_t request_len,
	uint8_t *response_frame,
	size_t response_cap,
	size_t *response_len);

#endif