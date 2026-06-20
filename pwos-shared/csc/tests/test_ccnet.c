#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ccnet.h"
#include "common.h"

static int g_failures;

struct capture {
    uint8_t data[CCNET_MAX_PACKET];
    size_t len;
    int calls;
};

static struct capture g_next_hop_capture;
static struct capture g_local_capture;

static void failf(const char *label, const char *detail)
{
    ++g_failures;
    printf("FAIL %s: %s\n", label, detail);
}

static void expect_int(const char *label, int actual, int expected)
{
    if (actual != expected) {
        char detail[128];

        snprintf(detail, sizeof(detail), "actual=%d expected=%d", actual, expected);
        failf(label, detail);
    }
}

static void expect_size(const char *label, size_t actual, size_t expected)
{
    if (actual != expected) {
        char detail[128];

        snprintf(detail, sizeof(detail), "actual=%lu expected=%lu",
                 (unsigned long)actual,
                 (unsigned long)expected);
        failf(label, detail);
    }
}

static int capture_next_hop(void *ctx, void *data, size_t len)
{
    (void)ctx;
    if (len > sizeof(g_next_hop_capture.data)) {
        return -1;
    }
    memcpy(g_next_hop_capture.data, data, len);
    g_next_hop_capture.len = len;
    ++g_next_hop_capture.calls;
    return 0;
}

static int capture_local(void *ctx, void *data, size_t len)
{
    (void)ctx;
    if (len > sizeof(g_local_capture.data)) {
        return -1;
    }
    memcpy(g_local_capture.data, data, len);
    g_local_capture.len = len;
    ++g_local_capture.calls;
    return 0;
}

static void reset_captures(void)
{
    memset(&g_next_hop_capture, 0, sizeof(g_next_hop_capture));
    memset(&g_local_capture, 0, sizeof(g_local_capture));
}

static void setup_node0_topology(void)
{
    expect_int("ccnet init", ccnet_init(0u, 4), 0);

    ccnet_graph_set_edge(0, 1, 1);
    ccnet_graph_set_edge(1, 0, 1);
    ccnet_graph_set_edge(1, 2, 1);
    ccnet_graph_set_edge(2, 1, 1);
    ccnet_graph_set_edge(2, 3, 1);
    ccnet_graph_set_edge(3, 2, 1);
    ccnet_build_routing_table();

    expect_int("register next hop", ccnet_register_node_link(1u, capture_next_hop), 0);
    expect_int("register local", ccnet_register_node_link(0u, capture_local), 0);
}

static void test_output_uses_dijkstra_next_hop(void)
{
    const uint8_t payload[] = {0x10u, 0x20u, 0x30u};
    struct ccnet_send_parameter param;
    const struct ccnet_hdr *hdr;

    reset_captures();
    setup_node0_topology();

    param.dst = 3u;
    param.ttl = 9u;
    param.type = CCNET_TYPE_DATA;

    expect_int("ccnet output", ccnet_output(&param, (void *)payload, sizeof(payload)), 0);
    expect_int("next hop called", g_next_hop_capture.calls, 1);
    expect_size("next hop packet len",
                g_next_hop_capture.len,
                sizeof(struct ccnet_hdr) + sizeof(payload));

    hdr = (const struct ccnet_hdr *)g_next_hop_capture.data;
    expect_int("output src", ntohs(hdr->src), 0);
    expect_int("output dst", ntohs(hdr->dst), 3);
    expect_int("output ttl", hdr->ttl, 9);
    expect_int("output type", hdr->type, CCNET_TYPE_DATA);
    expect_int("output payload len", ntohs(hdr->len), (int)sizeof(payload));
    expect_int("output checksum", in_checksum(g_next_hop_capture.data, (int)g_next_hop_capture.len), 0);

    if (memcmp(g_next_hop_capture.data + sizeof(struct ccnet_hdr), payload, sizeof(payload)) != 0) {
        failf("output payload", "payload differs");
    }
}

static void test_input_delivers_local_payload(void)
{
    uint8_t packet[CCNET_MAX_PACKET];
    const uint8_t payload[] = {0xA1u, 0xB2u};
    struct ccnet_hdr *hdr = (struct ccnet_hdr *)packet;
    size_t packet_len = sizeof(*hdr) + sizeof(payload);

    reset_captures();
    setup_node0_topology();

    memset(packet, 0, sizeof(packet));
    hdr->src = htons(2u);
    hdr->dst = htons(0u);
    hdr->ttl = 4u;
    hdr->type = CCNET_TYPE_DATA;
    hdr->len = htons((uint16_t)sizeof(payload));
    memcpy(packet + sizeof(*hdr), payload, sizeof(payload));
    hdr->checksum = in_checksum(packet, (int)packet_len);

    expect_int("ccnet input local", ccnet_input(NULL, packet, (int)packet_len), 0);
    expect_int("local called", g_local_capture.calls, 1);
    expect_size("local len", g_local_capture.len, sizeof(payload));

    if (memcmp(g_local_capture.data, payload, sizeof(payload)) != 0) {
        failf("local payload", "payload differs");
    }
}

int main(void)
{
    test_output_uses_dijkstra_next_hop();
    test_input_delivers_local_payload();

    if (g_failures != 0) {
        printf("pwos csc ccnet tests failed: %d\n", g_failures);
        return 1;
    }

    printf("pwos csc ccnet tests passed\n");
    return 0;
}
