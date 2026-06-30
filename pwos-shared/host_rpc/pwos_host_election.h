#ifndef PWOS_HOST_ELECTION_H
#define PWOS_HOST_ELECTION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_HOST_ELECTION_MAX_PEERS 8u
#define PWOS_HOST_ELECTION_DEFAULT_TIMEOUT_MS 15000u

typedef enum {
    PWOS_HOST_ROLE_OBSERVER = 0,
    PWOS_HOST_ROLE_FOLLOWER = 1,
    PWOS_HOST_ROLE_LEADER = 2,
} pwos_host_role_t;

typedef struct {
    uint8_t used;
    uint32_t uid[3];
    uint32_t epoch;
    uint16_t priority;
    uint32_t last_seen_ms;
} pwos_host_candidate_t;

typedef struct {
    uint32_t peer_updates;
    uint32_t peer_added;
    uint32_t peer_expired;
    uint32_t peer_rejected;
    uint32_t leader_changes;
} pwos_host_election_stats_t;

typedef struct {
    pwos_host_candidate_t local;
    pwos_host_candidate_t peers[PWOS_HOST_ELECTION_MAX_PEERS];
    pwos_host_candidate_t leader;
    uint8_t local_role;
    uint32_t timeout_ms;
    pwos_host_election_stats_t stats;
} pwos_host_election_t;

int pwos_host_election_init(
    pwos_host_election_t *election,
    const uint32_t local_uid[3],
    uint32_t local_epoch,
    uint16_t local_priority,
    uint32_t timeout_ms);

int pwos_host_election_update_peer(
    pwos_host_election_t *election,
    const uint32_t uid[3],
    uint32_t epoch,
    uint16_t priority,
    uint32_t now_ms);

size_t pwos_host_election_expire(
    pwos_host_election_t *election,
    uint32_t now_ms);

int pwos_host_election_get_peer(
    const pwos_host_election_t *election,
    size_t index,
    pwos_host_candidate_t *out_peer);

/* 返回 >0 表示 lhs 更适合作为 leader，<0 表示 rhs 更适合。 */
int pwos_host_candidate_compare(
    const pwos_host_candidate_t *lhs,
    const pwos_host_candidate_t *rhs);

const char *pwos_host_role_name(uint8_t role);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_HOST_ELECTION_H */
