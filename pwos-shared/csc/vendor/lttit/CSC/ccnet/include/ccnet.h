#ifndef CCNET_H
#define CCNET_H

#include <stdint.h>
#include <stddef.h>

#define CCNET_MAX_NODES 4
#define CCNET_INF 0x3fff
#define CCNET_TTL_DEFAULT 16
#define CCNET_MAX_PACKET 256

struct ccnet_graph {
    int cost[CCNET_MAX_NODES][CCNET_MAX_NODES];
    int node_count;
};

struct ccnet_dijkstra {
    int dist[CCNET_MAX_NODES];
    int prev[CCNET_MAX_NODES];
    int visited[CCNET_MAX_NODES];
};

enum {
    CCNET_TYPE_DATA = 0,
};

struct ccnet_hdr {
    uint16_t src;
    uint16_t dst;
    uint8_t  ttl;
    uint8_t  type;
    uint16_t len;
    uint16_t checksum;
} __attribute__((packed));

struct ccnet_send_parameter {
    uint16_t dst;
    uint8_t ttl;
    uint8_t type;
};

typedef int (*ccnet_link_t)(void *ctx, void *data, size_t len);

int ccnet_init(uint16_t src, int node_count);
void ccnet_graph_set_edge(int u, int v, int w);
void ccnet_build_routing_table(void);
int ccnet_register_node_link(uint16_t node_id, ccnet_link_t fun);

int ccnet_output(void *ctx, void *data, int len);
int ccnet_input(void *ctx, void *data, int len);

#endif
