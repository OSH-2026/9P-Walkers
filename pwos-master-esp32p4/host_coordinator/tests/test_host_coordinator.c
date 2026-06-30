#include "host_coordinator.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        ++g_failures; \
        printf("CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

static pwos_mesh2_node_register_t make_register(
    uint32_t uid0,
    uint32_t boot_id,
    uint8_t upstream_port)
{
    pwos_mesh2_node_register_t reg;

    memset(&reg, 0, sizeof(reg));
    reg.uid[0] = uid0;
    reg.uid[1] = uid0 + 1u;
    reg.uid[2] = uid0 + 2u;
    reg.boot_id = boot_id;
    reg.caps = PWOS_MESH2_NODE_CAP_RELAY;
    reg.upstream_port = upstream_port;
    return reg;
}

static void test_register_and_duplicate(void)
{
    pwos_host_coordinator_t coordinator;
    pwos_mesh2_node_register_t reg = make_register(100u, 10u, 0u);
    pwos_mesh2_addr_assign_t first;
    pwos_mesh2_addr_assign_t second;

    pwos_host_coordinator_init(&coordinator);

    CHECK(pwos_host_coordinator_handle_register(&coordinator, &reg, &first) == 0);
    CHECK(first.addr == 1u);
    CHECK(first.lease_epoch == 1u);
    CHECK(coordinator.register_rx == 1u);

    CHECK(pwos_host_coordinator_handle_register(&coordinator, &reg, &second) == 0);
    CHECK(second.addr == first.addr);
    CHECK(second.lease_epoch == first.lease_epoch);
    CHECK(coordinator.duplicate_register_rx == 1u);
}

static void test_reboot_keeps_addr_bumps_epoch(void)
{
    pwos_host_coordinator_t coordinator;
    pwos_mesh2_node_register_t reg = make_register(200u, 10u, 0u);
    pwos_mesh2_addr_assign_t first;
    pwos_mesh2_addr_assign_t after_reboot;

    pwos_host_coordinator_init(&coordinator);
    CHECK(pwos_host_coordinator_handle_register(&coordinator, &reg, &first) == 0);

    reg.boot_id = 11u;
    CHECK(pwos_host_coordinator_handle_register(&coordinator, &reg, &after_reboot) == 0);
    CHECK(after_reboot.addr == first.addr);
    CHECK(after_reboot.lease_epoch > first.lease_epoch);
    CHECK(coordinator.reboot_rx == 1u);
}

static void test_lease_renew_resends_current_lease(void)
{
    pwos_host_coordinator_t coordinator;
    pwos_mesh2_node_register_t reg = make_register(250u, 20u, 0u);
    pwos_mesh2_addr_assign_t assign;
    pwos_mesh2_lease_renew_t renew;
    pwos_mesh2_lease_ack_t ack;

    pwos_host_coordinator_init(&coordinator);
    CHECK(pwos_host_coordinator_handle_register(&coordinator, &reg, &assign) == 0);

    memset(&renew, 0, sizeof(renew));
    renew.uid[0] = reg.uid[0];
    renew.uid[1] = reg.uid[1];
    renew.uid[2] = reg.uid[2];
    renew.boot_id = reg.boot_id;
    renew.addr = assign.addr;
    renew.lease_epoch = assign.lease_epoch;

    CHECK(pwos_host_coordinator_handle_lease_renew(&coordinator, &renew, &ack) == 0);
    CHECK(ack.addr == assign.addr);
    CHECK(ack.lease_epoch == assign.lease_epoch);
    CHECK(ack.lease_ms == assign.lease_ms);
    CHECK(coordinator.lease_renew_rx == 1u);
    CHECK(coordinator.lease_ack_tx == 1u);
}

static void test_link_state_derives_route(void)
{
    pwos_host_coordinator_t coordinator;
    pwos_mesh2_node_register_t mcu1 = make_register(300u, 30u, 0u);
    pwos_mesh2_node_register_t mcu2 = make_register(400u, 40u, 1u);
    pwos_mesh2_addr_assign_t assign1;
    pwos_mesh2_addr_assign_t assign2;
    pwos_mesh2_link_state_t link;
    pwos_mesh2_route_update_t route;
    uint8_t owner = 0u;

    pwos_host_coordinator_init(&coordinator);
    CHECK(pwos_host_coordinator_handle_register(&coordinator, &mcu1, &assign1) == 0);
    CHECK(pwos_host_coordinator_handle_register(&coordinator, &mcu2, &assign2) == 0);

    memset(&link, 0, sizeof(link));
    link.local_addr = assign1.addr;
    link.local_port = 2u;
    link.peer_addr = assign2.addr;
    link.peer_port = 0u;
    link.flags = PWOS_MESH2_LINK_FLAG_UP;
    link.metric = 1u;
    link.peer_boot_id = mcu2.boot_id;
    link.peer_uid[0] = mcu2.uid[0];
    link.peer_uid[1] = mcu2.uid[1];
    link.peer_uid[2] = mcu2.uid[2];

    pwos_mesh2_route_update_t reverse_route;
    uint8_t reverse_owner = 0u;
    CHECK(pwos_host_coordinator_handle_link_state(&coordinator, &link, &route, &owner, &reverse_route, &reverse_owner) == 1);
    CHECK(owner == assign1.addr);
    CHECK(route.dst == assign2.addr);
    CHECK(route.next_hop == assign2.addr);
    CHECK(route.action == PWOS_MESH2_ROUTE_SET);
    CHECK(route.route_version == 1u);
    CHECK(reverse_owner == assign2.addr);
    CHECK(reverse_route.dst == assign1.addr);
    CHECK(reverse_route.next_hop == assign1.addr);
    CHECK(reverse_route.action == PWOS_MESH2_ROUTE_SET);
    CHECK(reverse_route.route_version == 2u);
    CHECK(coordinator.route_updates_tx == 2u);
}

static void test_triangle_derives_both_directions_for_every_edge(void)
{
    pwos_host_coordinator_t coordinator;
    pwos_mesh2_node_register_t regs[3];
    pwos_mesh2_addr_assign_t assigns[3];
    const uint8_t edges[3][2] = {{0u, 1u}, {1u, 2u}, {2u, 0u}};
    size_t i;

    pwos_host_coordinator_init(&coordinator);
    for (i = 0u; i < 3u; ++i) {
        regs[i] = make_register(500u + (uint32_t)i * 100u,
            50u + (uint32_t)i, (uint8_t)i);
        CHECK(pwos_host_coordinator_handle_register(
            &coordinator, &regs[i], &assigns[i]) == 0);
    }

    for (i = 0u; i < 3u; ++i) {
        const uint8_t local_index = edges[i][0];
        const uint8_t peer_index = edges[i][1];
        pwos_mesh2_link_state_t link;
        pwos_mesh2_route_update_t route;
        pwos_mesh2_route_update_t reverse;
        uint8_t owner = 0u;
        uint8_t reverse_owner = 0u;

        memset(&link, 0, sizeof(link));
        link.local_addr = assigns[local_index].addr;
        link.local_port = (uint8_t)i;
        link.peer_addr = assigns[peer_index].addr;
        link.flags = PWOS_MESH2_LINK_FLAG_UP;
        link.metric = 1u;
        link.peer_boot_id = regs[peer_index].boot_id;
        memcpy(link.peer_uid, regs[peer_index].uid, sizeof(link.peer_uid));

        CHECK(pwos_host_coordinator_handle_link_state(
            &coordinator, &link, &route, &owner,
            &reverse, &reverse_owner) == 1);
        CHECK(owner == assigns[local_index].addr);
        CHECK(route.dst == assigns[peer_index].addr);
        CHECK(reverse_owner == assigns[peer_index].addr);
        CHECK(reverse.dst == assigns[local_index].addr);
    }
    CHECK(coordinator.route_updates_tx == 6u);
}

int main(void)
{
    test_register_and_duplicate();
    test_reboot_keeps_addr_bumps_epoch();
    test_lease_renew_resends_current_lease();
    test_link_state_derives_route();
    test_triangle_derives_both_directions_for_every_edge();

    if (g_failures != 0) {
        printf("pwos host coordinator tests failed: %d\n", g_failures);
        return 1;
    }
    printf("pwos host coordinator tests passed\n");
    return 0;
}
