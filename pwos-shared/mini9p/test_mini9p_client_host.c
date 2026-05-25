#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mini9p_client.h"

static int g_failures;

struct fake_transport {
    uint8_t timeouts_before_success;
    int fail_code;
    uint8_t call_count;
    bool saw_tag;
    uint16_t expected_tag;
};

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

static int fake_transport_request(
    void *transport_ctx,
    const uint8_t *tx_data,
    size_t tx_len,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    struct fake_transport *transport = (struct fake_transport *)transport_ctx;
    struct m9p_frame_view request;

    if (transport == NULL || tx_data == NULL || rx_data == NULL || rx_len == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    if (!m9p_decode_frame(tx_data, tx_len, &request)) {
        return -(int)M9P_ERR_EIO;
    }

    ++transport->call_count;
    if (!transport->saw_tag) {
        transport->expected_tag = request.tag;
        transport->saw_tag = true;
    } else if (request.tag != transport->expected_tag) {
        return -(int)M9P_ERR_ETAG;
    }

    if (transport->fail_code != 0) {
        return transport->fail_code;
    }
    if (transport->call_count <= transport->timeouts_before_success) {
        return -(int)M9P_ERR_EAGAIN;
    }

    {
        struct m9p_qid root_qid;

        memset(&root_qid, 0, sizeof(root_qid));
        root_qid.type = M9P_QID_DIR;
        root_qid.version = 1u;
        root_qid.object_id = 1u;
        if (!m9p_build_rattach(
                request.tag,
                M9P_DEFAULT_MSIZE,
                16u,
                M9P_DEFAULT_INFLIGHT,
                M9P_FEATURE_DIRECTORY_READ,
                &root_qid,
                rx_data,
                rx_cap,
                rx_len)) {
            return -(int)M9P_ERR_EMSIZE;
        }
    }

    return 0;
}

static void test_timeout_retries_then_succeeds(void)
{
    struct m9p_client client;
    struct fake_transport transport;

    memset(&transport, 0, sizeof(transport));
    transport.timeouts_before_success = (uint8_t)(M9P_CLIENT_TIMEOUT_RETRY_ATTEMPTS - 1u);

    m9p_client_init(&client, fake_transport_request, &transport);

    expect_int("attach retries then succeeds",
               m9p_client_attach(&client, M9P_DEFAULT_MSIZE, M9P_DEFAULT_INFLIGHT, 0u),
               0);
    expect_int("attach retry call count", transport.call_count, M9P_CLIENT_TIMEOUT_RETRY_ATTEMPTS);
}

static void test_timeout_returns_after_retry_budget(void)
{
    struct m9p_client client;
    struct fake_transport transport;

    memset(&transport, 0, sizeof(transport));
    transport.timeouts_before_success = M9P_CLIENT_TIMEOUT_RETRY_ATTEMPTS;

    m9p_client_init(&client, fake_transport_request, &transport);

    expect_int("attach timeout after retries",
               m9p_client_attach(&client, M9P_DEFAULT_MSIZE, M9P_DEFAULT_INFLIGHT, 0u),
               -(int)M9P_ERR_EAGAIN);
    expect_int("attach timeout call count", transport.call_count, M9P_CLIENT_TIMEOUT_RETRY_ATTEMPTS);
}

static void test_non_timeout_error_is_not_retried(void)
{
    struct m9p_client client;
    struct fake_transport transport;

    memset(&transport, 0, sizeof(transport));
    transport.fail_code = -(int)M9P_ERR_EIO;

    m9p_client_init(&client, fake_transport_request, &transport);

    expect_int("attach non-timeout error",
               m9p_client_attach(&client, M9P_DEFAULT_MSIZE, M9P_DEFAULT_INFLIGHT, 0u),
               -(int)M9P_ERR_EIO);
    expect_int("attach non-timeout call count", transport.call_count, 1);
}

int main(void)
{
    test_timeout_retries_then_succeeds();
    test_timeout_returns_after_retry_budget();
    test_non_timeout_error_is_not_retried();

    if (g_failures != 0) {
        printf("mini9p client host tests failed: %d\n", g_failures);
        return 1;
    }

    printf("mini9p client host tests passed\n");
    return 0;
}
