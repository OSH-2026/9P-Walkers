#include "mini9p_peer_link.h"

#include <string.h>

/*
 * 当前协议类型仍是固定枚举，因此这里直接按已知 T* / R* 集合分类。
 * 这样 peer_link 不必依赖“奇偶”或其他脆弱规则，也不需要改动协议帧格式。
 */
static bool type_is_request(uint8_t type)
{
    switch (type) {
    case M9P_TATTACH:
    case M9P_TWALK:
    case M9P_TOPEN:
    case M9P_TREAD:
    case M9P_TWRITE:
    case M9P_TSTAT:
    case M9P_TCLUNK:
        return true;
    default:
        return false;
    }
}

static bool type_is_response(uint8_t type)
{
    switch (type) {
    case M9P_RATTACH:
    case M9P_RWALK:
    case M9P_ROPEN:
    case M9P_RREAD:
    case M9P_RWRITE:
    case M9P_RSTAT:
    case M9P_RCLUNK:
    case M9P_RERROR:
        return true;
    default:
        return false;
    }
}

/*
 * 上层 handler 失败时，peer_link 会尽量把它转成协议级 Rerror，而不是让链路静默断在半路。
 * 若 rc 不是标准的负 Mini9P 错误码，则退化为 EIO。
 */
static uint16_t protocol_error_from_rc(int rc)
{
    uint16_t code;

    if (rc >= 0) {
        return M9P_ERR_EIO;
    }

    code = (uint16_t)(-rc);
    switch (code) {
    case M9P_ERR_EINVAL:
    case M9P_ERR_ENOENT:
    case M9P_ERR_EPERM:
    case M9P_ERR_EBUSY:
    case M9P_ERR_ENOTDIR:
    case M9P_ERR_EISDIR:
    case M9P_ERR_EOFFS:
    case M9P_ERR_EMSIZE:
    case M9P_ERR_EIO:
    case M9P_ERR_ENOTSUP:
    case M9P_ERR_ETAG:
    case M9P_ERR_EFID:
    case M9P_ERR_EAGAIN:
        return code;
    default:
        return M9P_ERR_EIO;
    }
}

static int claim_dispatch(struct m9p_peer_link *link)
{
    if (link->dispatch_busy) {
        return -(int)M9P_ERR_EBUSY;
    }

    link->dispatch_busy = true;
    return 0;
}

static void release_dispatch(struct m9p_peer_link *link)
{
    link->dispatch_busy = false;
}

static int decode_and_validate_frame(
    const uint8_t *frame_data,
    size_t frame_len,
    struct m9p_frame_view *out_frame)
{
    if (!m9p_decode_frame(frame_data, frame_len, out_frame)) {
        return -(int)M9P_ERR_EIO;
    }
    if (out_frame->version != M9P_VERSION) {
        return -(int)M9P_ERR_EINVAL;
    }

    return 0;
}

static int observe_foreign_frame(
    struct m9p_peer_link *link,
    const uint8_t *frame_data,
    size_t frame_len,
    const struct m9p_frame_view *frame)
{
    if (link->config.foreign_frame_handler == NULL) {
        return 0;
    }

    return link->config.foreign_frame_handler(
        link->config.foreign_frame_handler_ctx,
        frame_data,
        frame_len,
        frame);
}

/*
 * 这是 peer_link 的核心能力：在等待本端响应时，收到对端 T* 请求就当场处理，
 * 这样双方都能在同一条 UART 上主动发起请求，而不必等“对方完全空闲”再说。
 */
static int service_foreign_request(
    struct m9p_peer_link *link,
    const uint8_t *request_data,
    size_t request_len,
    const struct m9p_frame_view *request_frame)
{
    size_t response_len = 0u;
    int rc;

    if (link->config.request_handler != NULL) {
        rc = link->config.request_handler(
            link->config.request_handler_ctx,
            request_data,
            request_len,
            link->config.dispatch_tx_buffer,
            link->config.dispatch_tx_cap,
            &response_len);
    } else {
        rc = -(int)M9P_ERR_ENOTSUP;
    }

    if (rc < 0 || response_len == 0u) {
        uint16_t code = protocol_error_from_rc(rc == 0 ? -(int)M9P_ERR_EIO : rc);

        if (!m9p_build_rerror(
                request_frame->tag,
                code,
                m9p_error_name(code),
                link->config.dispatch_tx_buffer,
                link->config.dispatch_tx_cap,
                &response_len)) {
            return -(int)M9P_ERR_EMSIZE;
        }
    }

    return link->config.send_frame(
        link->config.transport_ctx,
        link->config.dispatch_tx_buffer,
        response_len);
}

void m9p_peer_link_get_default_config(struct m9p_peer_link_config *out_config)
{
    if (out_config == NULL) {
        return;
    }

    memset(out_config, 0, sizeof(*out_config));
}

int m9p_peer_link_init(struct m9p_peer_link *link, const struct m9p_peer_link_config *config)
{
    if (link == NULL || config == NULL || config->send_frame == NULL ||
        config->receive_frame == NULL || config->dispatch_rx_buffer == NULL ||
        config->dispatch_tx_buffer == NULL || config->dispatch_rx_cap < M9P_FRAME_OVERHEAD ||
        config->dispatch_tx_cap < M9P_FRAME_OVERHEAD) {
        return -(int)M9P_ERR_EINVAL;
    }

    link->config = *config;
    link->initialized = true;
    link->dispatch_busy = false;
    return 0;
}

void m9p_peer_link_deinit(struct m9p_peer_link *link)
{
    if (link == NULL) {
        return;
    }

    memset(link, 0, sizeof(*link));
}

int m9p_peer_link_request(
    void *link_ctx,
    const uint8_t *tx_data,
    size_t tx_len,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    struct m9p_peer_link *link = (struct m9p_peer_link *)link_ctx;
    struct m9p_frame_view outbound_frame;
    struct m9p_frame_view inbound_frame;
    size_t inbound_len = 0u;
    int rc;

    if (link == NULL || !link->initialized || tx_data == NULL || tx_len == 0u ||
        rx_data == NULL || rx_len == NULL || rx_cap < M9P_FRAME_OVERHEAD) {
        return -(int)M9P_ERR_EINVAL;
    }

    *rx_len = 0u;

    rc = decode_and_validate_frame(tx_data, tx_len, &outbound_frame);
    if (rc < 0) {
        return rc;
    }
    if (!type_is_request(outbound_frame.type)) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = claim_dispatch(link);
    if (rc < 0) {
        return rc;
    }

    rc = link->config.send_frame(link->config.transport_ctx, tx_data, tx_len);
    if (rc < 0) {
        goto out;
    }

    while (1) {
        inbound_len = 0u;
        rc = link->config.receive_frame(
            link->config.transport_ctx,
            link->config.dispatch_rx_buffer,
            link->config.dispatch_rx_cap,
            &inbound_len);
        if (rc < 0) {
            goto out;
        }

        rc = decode_and_validate_frame(link->config.dispatch_rx_buffer, inbound_len, &inbound_frame);
        if (rc < 0) {
            goto out;
        }

        if (type_is_request(inbound_frame.type)) {
            rc = service_foreign_request(
                link,
                link->config.dispatch_rx_buffer,
                inbound_len,
                &inbound_frame);
            if (rc < 0) {
                goto out;
            }
            continue;
        }

        if (type_is_response(inbound_frame.type)) {
            if (inbound_frame.tag == outbound_frame.tag) {
                if (inbound_len > rx_cap) {
                    rc = -(int)M9P_ERR_EMSIZE;
                    goto out;
                }

                memcpy(rx_data, link->config.dispatch_rx_buffer, inbound_len);
                *rx_len = inbound_len;
                rc = 0;
                goto out;
            }

            rc = observe_foreign_frame(
                link,
                link->config.dispatch_rx_buffer,
                inbound_len,
                &inbound_frame);
            if (rc < 0) {
                goto out;
            }
            continue;
        }

        rc = -(int)M9P_ERR_EIO;
        goto out;
    }

out:
    release_dispatch(link);
    return rc;
}

int m9p_peer_link_poll_once(struct m9p_peer_link *link)
{
    struct m9p_frame_view frame;
    size_t rx_len = 0u;
    int rc;

    if (link == NULL || !link->initialized) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = claim_dispatch(link);
    if (rc < 0) {
        return rc;
    }

    rc = link->config.receive_frame(
        link->config.transport_ctx,
        link->config.dispatch_rx_buffer,
        link->config.dispatch_rx_cap,
        &rx_len);
    if (rc < 0) {
        goto out;
    }

    rc = decode_and_validate_frame(link->config.dispatch_rx_buffer, rx_len, &frame);
    if (rc < 0) {
        goto out;
    }

    if (type_is_request(frame.type)) {
        rc = service_foreign_request(link, link->config.dispatch_rx_buffer, rx_len, &frame);
        goto out;
    }

    if (type_is_response(frame.type)) {
        rc = observe_foreign_frame(link, link->config.dispatch_rx_buffer, rx_len, &frame);
        if (rc == 0) {
            rc = -(int)M9P_ERR_ETAG;
        }
        goto out;
    }

    rc = -(int)M9P_ERR_EIO;

out:
    release_dispatch(link);
    return rc;
}