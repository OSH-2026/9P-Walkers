#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "rpc.h"

static int g_failures;

static size_t fake_send(void *user, const uint8_t *buf, size_t len)
{
    size_t *sent = (size_t *)user;

    if (sent != NULL) {
        *sent += len;
    }
    (void)buf;
    return len;
}

static int fake_handler(const struct rpc_request *request, struct rpc_response *response)
{
    (void)request;
    (void)response;
    return 0;
}

static void expect_ptr_not_null(const char *label, const void *ptr)
{
    if (ptr == NULL) {
        ++g_failures;
        printf("FAIL %s: null pointer\n", label);
    }
}

int main(void)
{
    size_t sent = 0u;
    struct rpc_transport_class *transport;

    rpc_init(4u);
    rpc_set_handler(fake_handler);

    transport = rpc_trans_class_create((void *)fake_send, NULL, NULL, &sent);
    expect_ptr_not_null("transport create", transport);

    if (g_failures != 0) {
        printf("pwos csc rpc smoke failed: %d\n", g_failures);
        return 1;
    }

    printf("pwos csc rpc smoke passed\n");
    return 0;
}
