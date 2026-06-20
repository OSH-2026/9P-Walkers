#include <stdint.h>
#include <stdio.h>

#include "scp.h"

static int g_failures;

static int fake_send(void *user, const void *buf, size_t len)
{
    size_t *sent = (size_t *)user;

    if (sent != NULL) {
        *sent += len;
    }
    (void)buf;
    return (int)len;
}

static void expect_int(const char *label, int actual, int expected)
{
    if (actual != expected) {
        ++g_failures;
        printf("FAIL %s: actual=%d expected=%d\n", label, actual, expected);
    }
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
    struct scp_transport_class transport = {
        .send = fake_send,
        .recv = NULL,
        .close = NULL,
        .user = &sent,
    };
    struct scp_stream *stream;

    expect_int("scp init", scp_init(4u), 0);

    stream = scp_stream_alloc(&transport, 1, 2);
    expect_ptr_not_null("stream alloc", stream);
    if (stream != NULL) {
        expect_int("stream initial state", stream->state, SCP_CLOSED);
        expect_int("stream src fd", (int)stream->src_fd, 1);
        expect_int("stream dst fd", (int)stream->dst_fd, 2);
        expect_int("stream free", scp_stream_free(stream), 0);
    }

    scp_timer_process();

    if (g_failures != 0) {
        printf("pwos csc scp smoke failed: %d\n", g_failures);
        return 1;
    }

    printf("pwos csc scp smoke passed\n");
    return 0;
}
