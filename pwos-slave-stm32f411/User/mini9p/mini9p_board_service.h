/**
 * @file mini9p_board_service.h
 * @brief Board-level Mini9P service bootstrap for pwos-slave.
 */

#ifndef MINI9P_BOARD_SERVICE_H
#define MINI9P_BOARD_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

int mini9p_board_service_init(void);
int mini9p_board_service_poll_once(void);

#ifdef __cplusplus
}
#endif

#endif
