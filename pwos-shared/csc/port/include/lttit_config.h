#ifndef PWOS_CSC_LTTIT_CONFIG_H
#define PWOS_CSC_LTTIT_CONFIG_H

/*
 * lttit CSC 的最小配置切片。
 *
 * 当前编译 ccnet 路由层和 scp 可靠传输层；ccrpc 接入时再补
 * pending/waiter 配置。这里不引入 lttit 的 RTOS 配置。
 */
#define CCNET_DEBUG 0

#define RPC_DEBUG 0
#define RPC_BUF_DEFAULT 1024u

#define SCP_DEBUG 0
#define SCP_DUMP 0
#define SCP_RUN_DEBUG 0

#define RETRANS_COUNT_MAX 12
#define SCP_RTO_MIN 100
#define SCP_RTO_MAX 1000
#define RETRANS_RECO_MAX 16
#define RETRANS_GAP_MAX 8
#define SCP_RECV_LIMIT 0xFFFFu
#define MTU 200u
#define SEND_WIN_INIT (2u * MTU)
#define RECV_WIN_INIT (2u * MTU)
#define SSTHRESH_INIT (2u * MTU)
#define PERSIST_INTERVAL 200u
#define MAX_IDLE_FAIL 3u
#define IDLE_TIMEOUT 100000u

#endif
