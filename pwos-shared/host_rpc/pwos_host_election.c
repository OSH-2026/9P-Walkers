#include "pwos_host_election.h"

#include <string.h>

static int uid_equal(const uint32_t lhs[3], const uint32_t rhs[3])
{
    return lhs[0] == rhs[0] && lhs[1] == rhs[1] && lhs[2] == rhs[2];
}

int pwos_host_candidate_compare(
    const pwos_host_candidate_t *lhs,
    const pwos_host_candidate_t *rhs)
{
    size_t i;

    if (lhs == NULL || rhs == NULL) {
        return 0;
    }
    if (lhs->epoch != rhs->epoch) {
        return lhs->epoch > rhs->epoch ? 1 : -1;
    }
    if (lhs->priority != rhs->priority) {
        return lhs->priority > rhs->priority ? 1 : -1;
    }
    /* 最后一项用稳定 UID 打破平局，保证所有主机独立算出相同结果。 */
    for (i = 0u; i < 3u; ++i) {
        if (lhs->uid[i] != rhs->uid[i]) {
            return lhs->uid[i] > rhs->uid[i] ? 1 : -1;
        }
    }
    return 0;
}

static void elect(pwos_host_election_t *election)
{
    pwos_host_candidate_t previous = election->leader;
    size_t i;

    election->leader = election->local;
    for (i = 0u; i < PWOS_HOST_ELECTION_MAX_PEERS; ++i) {
        if (election->peers[i].used != 0u &&
            pwos_host_candidate_compare(
                &election->peers[i], &election->leader) > 0) {
            election->leader = election->peers[i];
        }
    }
    election->local_role = uid_equal(
        election->leader.uid, election->local.uid) ?
        PWOS_HOST_ROLE_LEADER : PWOS_HOST_ROLE_FOLLOWER;
    if (previous.used != 0u &&
        !uid_equal(previous.uid, election->leader.uid)) {
        ++election->stats.leader_changes;
    }
}

int pwos_host_election_init(
    pwos_host_election_t *election,
    const uint32_t local_uid[3],
    uint32_t local_epoch,
    uint16_t local_priority,
    uint32_t timeout_ms)
{
    if (election == NULL || local_uid == NULL ||
        (local_uid[0] == 0u && local_uid[1] == 0u && local_uid[2] == 0u)) {
        return -1;
    }
    memset(election, 0, sizeof(*election));
    election->local.used = 1u;
    memcpy(election->local.uid, local_uid, sizeof(election->local.uid));
    election->local.epoch = local_epoch;
    election->local.priority = local_priority;
    election->timeout_ms = timeout_ms == 0u ?
        PWOS_HOST_ELECTION_DEFAULT_TIMEOUT_MS : timeout_ms;
    elect(election);
    return 0;
}

int pwos_host_election_update_peer(
    pwos_host_election_t *election,
    const uint32_t uid[3],
    uint32_t epoch,
    uint16_t priority,
    uint32_t now_ms)
{
    pwos_host_candidate_t *free_slot = NULL;
    pwos_host_candidate_t *peer = NULL;
    size_t i;

    if (election == NULL || uid == NULL || uid_equal(uid, election->local.uid)) {
        return -1;
    }
    for (i = 0u; i < PWOS_HOST_ELECTION_MAX_PEERS; ++i) {
        if (election->peers[i].used != 0u && uid_equal(election->peers[i].uid, uid)) {
            peer = &election->peers[i];
            break;
        }
        if (free_slot == NULL && election->peers[i].used == 0u) {
            free_slot = &election->peers[i];
        }
    }
    if (peer == NULL) {
        peer = free_slot;
        if (peer == NULL) {
            ++election->stats.peer_rejected;
            return -1;
        }
        memset(peer, 0, sizeof(*peer));
        peer->used = 1u;
        memcpy(peer->uid, uid, sizeof(peer->uid));
        ++election->stats.peer_added;
    }
    peer->epoch = epoch;
    peer->priority = priority;
    peer->last_seen_ms = now_ms;
    ++election->stats.peer_updates;
    elect(election);
    return 0;
}

size_t pwos_host_election_expire(
    pwos_host_election_t *election,
    uint32_t now_ms)
{
    size_t expired = 0u;
    size_t i;

    if (election == NULL) {
        return 0u;
    }
    for (i = 0u; i < PWOS_HOST_ELECTION_MAX_PEERS; ++i) {
        if (election->peers[i].used != 0u &&
            (uint32_t)(now_ms - election->peers[i].last_seen_ms) >=
                election->timeout_ms) {
            memset(&election->peers[i], 0, sizeof(election->peers[i]));
            ++expired;
        }
    }
    if (expired > 0u) {
        election->stats.peer_expired += (uint32_t)expired;
        elect(election);
    }
    return expired;
}

int pwos_host_election_get_peer(
    const pwos_host_election_t *election,
    size_t index,
    pwos_host_candidate_t *out_peer)
{
    if (election == NULL || out_peer == NULL ||
        index >= PWOS_HOST_ELECTION_MAX_PEERS ||
        election->peers[index].used == 0u) {
        return -1;
    }
    *out_peer = election->peers[index];
    return 0;
}

const char *pwos_host_role_name(uint8_t role)
{
    switch (role) {
    case PWOS_HOST_ROLE_OBSERVER: return "observer";
    case PWOS_HOST_ROLE_FOLLOWER: return "follower";
    case PWOS_HOST_ROLE_LEADER: return "leader";
    default: return "unknown";
    }
}
