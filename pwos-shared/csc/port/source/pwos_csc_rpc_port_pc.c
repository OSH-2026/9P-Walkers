#include "rpc_port.h"
#include "schedule.h"

#include <stdlib.h>

struct rpc_waiter {
    int woken;
};

struct rpc_waiter *rpc_waiter_create(void)
{
    struct rpc_waiter *waiter = (struct rpc_waiter *)malloc(sizeof(*waiter));

    if (waiter != NULL) {
        waiter->woken = 0;
    }
    return waiter;
}

void rpc_waiter_destroy(struct rpc_waiter *waiter)
{
    free(waiter);
}

int rpc_waiter_wait(struct rpc_waiter *waiter, uint32_t timeout_ms)
{
    uint32_t start;

    if (waiter == NULL) {
        return -1;
    }

    start = rpc_now_ms();
    while (!waiter->woken) {
        if (timeout_ms == 0u) {
            return -1;
        }
        if ((uint32_t)(rpc_now_ms() - start) >= timeout_ms) {
            return -1;
        }
    }
    return 0;
}

void rpc_waiter_wake(struct rpc_waiter *waiter)
{
    if (waiter != NULL) {
        waiter->woken = 1;
    }
}

uint32_t rpc_now_ms(void)
{
    return rtos_now_time();
}

void rpc_port_lock(void)
{
}

void rpc_port_unlock(void)
{
}
