#include "mini9p_client.h"

#include <string.h>

/**
 * @brief 发送请求帧并接收响应帧
 *
 * @param client 客户端
 * @param tx_len 发送长度
 * @param expected_tag 期望的响应标签
 * @param expected_type 期望的响应类型
 * @param out_frame 输出的响应帧
 * @return int 0表示成功，非0表示错误码
 */
static int request_with_frame(
    struct m9p_client *client,
    size_t tx_len,
    uint16_t expected_tag,
    uint8_t expected_type,
    struct m9p_frame_view *out_frame)
{
    struct m9p_frame_view frame;
    struct m9p_error error;
    uint8_t attempt;

    if (client == NULL || client->transport == NULL || out_frame == NULL) {
        return -(int)M9P_ERR_ENOTSUP;
    }

    for (attempt = 0u; attempt < M9P_CLIENT_TIMEOUT_RETRY_ATTEMPTS; ++attempt) {
        size_t rx_len = 0u;
        int rc;
        /*
         * transport 是协议层和数据面之间的唯一接口：
         * 在 ESP32 主机上它就是 session_transport()，负责 pending/tag/deadline。
         * mini9P client 本身只关心请求帧、响应帧、tag/type 校验。
         */
        rc = client->transport(
            client->transport_ctx,
            client->tx_buffer,
            tx_len,
            client->rx_buffer,
            sizeof(client->rx_buffer),
            &rx_len);
        if ((rc == -(int)M9P_ERR_EAGAIN || rc == -(int)M9P_ERR_EBUSY) &&
            (attempt + 1u) < M9P_CLIENT_TIMEOUT_RETRY_ATTEMPTS) {
            /* 只对协议级暂时不可用重试；deadline/stale_boot 这类会直接返回。 */
            continue;
        }
        if (rc != 0) {
            return rc;
        }
        if (!m9p_decode_frame(client->rx_buffer, rx_len, &frame)) {
            return -(int)M9P_ERR_EIO;
        }
        if (frame.version != M9P_VERSION) {
            return -(int)M9P_ERR_EINVAL;
        }
        if (frame.tag != expected_tag) {
            /* session_transport 返回前会把 wire_tag 改回本地 tag，所以这里仍能严格校验。 */
            return -(int)M9P_ERR_ETAG;
        }
        if (frame.type == M9P_RERROR) {
            if (m9p_parse_rerror(&frame, &error)) {
                return -(int)error.code;
            }
            return -(int)M9P_ERR_EIO;
        }
        if (frame.type != expected_type) {
            return -(int)M9P_ERR_EIO;
        }

        *out_frame = frame;
        return 0;
    }

    return -(int)M9P_ERR_EAGAIN;
}

/**
 * @brief 客户端初始化
 * 
 * @param client 客户端实例指针
 * @param transport 传输函数指针
 * @param transport_ctx 传输层上下文指针
 */
void m9p_client_init(struct m9p_client *client, m9p_transport_fn transport, void *transport_ctx)
{
    if (client == NULL) {
        return;
    }

    memset(client, 0, sizeof(*client));
    client->transport = transport;
    client->transport_ctx = transport_ctx;
    client->next_tag = 1u;
    client->next_fid = M9P_FIRST_DYNAMIC_FID;
    client->negotiated_msize = 256u;
}

void m9p_client_reset_session(struct m9p_client *client)
{
    if (client == NULL) {
        return;
    }

    /* 节点重启或 session reset 后，清空协商状态与 root qid。 */
    client->next_tag = 1u;
    client->next_fid = M9P_FIRST_DYNAMIC_FID;
    client->negotiated_msize = 256u;
    client->max_fids = 0u;
    client->max_inflight = 0u;
    client->feature_bits = 0u;
    client->attached = false;
    memset(&client->root_qid, 0, sizeof(client->root_qid));
}

/**
 * @brief 客户端 fid 分配函数
 * 
 * @param client 客户端实例指针
 * @return uint16_t 分配的 fid
 */
uint16_t m9p_client_alloc_fid(struct m9p_client *client)
{
    uint16_t fid;

    if (client == NULL) {
        return 0u;
    }

    /* fid 只在单个 mini9P session 内有效，节点重启后会由 reset_session 归零。 */
    fid = client->next_fid;
    ++client->next_fid;
    if (client->next_fid < M9P_FIRST_DYNAMIC_FID || client->next_fid == M9P_ROOT_FID) {
        client->next_fid = M9P_FIRST_DYNAMIC_FID;
    }
    return fid;
}

/**
 * @brief 客户端 ATTACH 操作
 * 
 * @param client 客户端实例指针
 * @param requested_msize 请求的最大消息大小
 * @param requested_inflight 请求的最大未完成请求数
 * @param attach_flags ATTACH 标志
 * @return int 0表示成功，非0表示错误码
 */
int m9p_client_attach(
    struct m9p_client *client,
    uint16_t requested_msize,
    uint8_t requested_inflight,
    uint8_t attach_flags)
{
    struct m9p_frame_view frame;
    struct m9p_attach_result attach_result;
    size_t tx_len = 0u;
    uint16_t tag;

    if (client == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    tag = client->next_tag++;
    if (!m9p_build_tattach(
            tag,
            M9P_ROOT_FID,
            requested_msize,
            requested_inflight,
            attach_flags,
            client->tx_buffer,
            sizeof(client->tx_buffer),
            &tx_len)) {
        return -(int)M9P_ERR_EMSIZE;
    }

    {
        int rc = request_with_frame(client, tx_len, tag, M9P_RATTACH, &frame);

        if (rc != 0) {
            return rc;
        }
    }
    if (!m9p_parse_rattach(&frame, &attach_result)) {
        return -(int)M9P_ERR_EIO;
    }

    client->negotiated_msize = attach_result.negotiated_msize;
    client->max_fids = attach_result.max_fids;
    client->max_inflight = attach_result.max_inflight;
    client->feature_bits = attach_result.feature_bits;
    client->root_qid = attach_result.root_qid;
    client->attached = true;
    return 0;
}

/**
 * @brief 客户端 WALK 操作
 * 
 * @param client 客户端实例指针
 * @param fid 当前 fid
 * @param newfid 新 fid
 * @param path 路径
 * @param out_qid 输出 qid
 * @return int 0表示成功，非0表示错误码
 */
int m9p_client_walk(struct m9p_client *client, uint16_t fid, uint16_t newfid, const char *path, struct m9p_qid *out_qid)
{
    struct m9p_frame_view frame;
    size_t tx_len = 0u;
    uint16_t tag;
    int rc;

    if (client == NULL || out_qid == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    tag = client->next_tag++;
    if (!m9p_build_twalk(tag, fid, newfid, path, client->tx_buffer, sizeof(client->tx_buffer), &tx_len)) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = request_with_frame(client, tx_len, tag, M9P_RWALK, &frame);
    if (rc != 0) {
        return rc;
    }
    if (!m9p_parse_rwalk(&frame, out_qid)) {
        return -(int)M9P_ERR_EIO;
    }

    return 0;
}

/**
 * @brief 客户端 OPEN 操作
 * 
 * @param client 客户端实例指针
 * @param fid fid
 * @param mode 打开模式
 * @param out_result 输出结果
 * @return int 0表示成功，非0表示错误码
 */
int m9p_client_open(struct m9p_client *client, uint16_t fid, uint8_t mode, struct m9p_open_result *out_result)
{
    struct m9p_frame_view frame;
    size_t tx_len = 0u;
    uint16_t tag;
    int rc;

    if (client == NULL || out_result == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    tag = client->next_tag++;
    if (!m9p_build_topen(tag, fid, mode, client->tx_buffer, sizeof(client->tx_buffer), &tx_len)) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = request_with_frame(client, tx_len, tag, M9P_ROPEN, &frame);
    if (rc != 0) {
        return rc;
    }
    if (!m9p_parse_ropen(&frame, out_result)) {
        return -(int)M9P_ERR_EIO;
    }

    return 0;
}

/**
 * @brief 客户端 READ 操作
 * 
 * @param client 客户端实例指针
 * @param fid fid
 * @param offset 偏移量
 * @param data 数据缓冲区
 * @param in_out_count 输入为请求的字节数，输出为实际读取的字节数
 * @return int 0表示成功，非0表示错误码
 */
int m9p_client_read(
    struct m9p_client *client,
    uint16_t fid,
    uint32_t offset,
    uint8_t *data,
    uint16_t *in_out_count)
{
    struct m9p_frame_view frame;
    size_t tx_len = 0u;
    uint16_t tag;
    int rc;

    if (client == NULL || in_out_count == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    tag = client->next_tag++;
    if (!m9p_build_tread(
            tag,
            fid,
            offset,
            *in_out_count,
            client->tx_buffer,
            sizeof(client->tx_buffer),
            &tx_len)) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = request_with_frame(client, tx_len, tag, M9P_RREAD, &frame);
    if (rc != 0) {
        return rc;
    }
    if (!m9p_parse_rread(&frame, data, *in_out_count, in_out_count)) {
        return -(int)M9P_ERR_EIO;
    }

    return 0;
}

/**
 * @brief 客户端 WRITE 操作
 * 
 * @param client 客户端实例指针
 * @param fid fid
 * @param offset 偏移量
 * @param data 数据缓冲区
 * @param count 写入的字节数
 * @param out_written 实际写入的字节数
 * @return int 0表示成功，非0表示错误码
 */
int m9p_client_write(
    struct m9p_client *client,
    uint16_t fid,
    uint32_t offset,
    const uint8_t *data,
    uint16_t count,
    uint16_t *out_written)
{
    struct m9p_frame_view frame;
    size_t tx_len = 0u;
    uint16_t tag;
    int rc;

    if (client == NULL || out_written == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    tag = client->next_tag++;
    if (!m9p_build_twrite(
            tag,
            fid,
            offset,
            data,
            count,
            client->tx_buffer,
            sizeof(client->tx_buffer),
            &tx_len)) {
        return -(int)M9P_ERR_EMSIZE;
    }

    rc = request_with_frame(client, tx_len, tag, M9P_RWRITE, &frame);
    if (rc != 0) {
        return rc;
    }
    if (!m9p_parse_rwrite(&frame, out_written)) {
        return -(int)M9P_ERR_EIO;
    }

    return 0;
}

/**
 * @brief 客户端 STAT 操作
 * 
 * @param client 客户端实例指针
 * @param fid fid
 * @param out_stat 输出的文件状态信息
 * @return int 0表示成功，非0表示错误码
 */
int m9p_client_stat(struct m9p_client *client, uint16_t fid, struct m9p_stat *out_stat)
{
    struct m9p_frame_view frame;
    size_t tx_len = 0u;
    uint16_t tag;
    int rc;

    if (client == NULL || out_stat == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    tag = client->next_tag++;
    if (!m9p_build_tstat(tag, fid, client->tx_buffer, sizeof(client->tx_buffer), &tx_len)) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = request_with_frame(client, tx_len, tag, M9P_RSTAT, &frame);
    if (rc != 0) {
        return rc;
    }
    if (!m9p_parse_rstat(&frame, out_stat)) {
        return -(int)M9P_ERR_EIO;
    }

    return 0;
}

/**
 * @brief 客户端 CLUNK 操作
 * 
 * @param client 客户端实例指针
 * @param fid fid
 * @return int 0表示成功，非0表示错误码
 */
int m9p_client_clunk(struct m9p_client *client, uint16_t fid)
{
    struct m9p_frame_view frame;
    size_t tx_len = 0u;
    uint16_t tag;
    int rc;

    if (client == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    tag = client->next_tag++;
    if (!m9p_build_tclunk(tag, fid, client->tx_buffer, sizeof(client->tx_buffer), &tx_len)) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = request_with_frame(client, tx_len, tag, M9P_RCLUNK, &frame);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

/**
 * @brief 客户端 WALK 路径操作
 * 
 * @param client 客户端实例指针
 * @param path 路径
 * @param out_fid 输出的 fid
 * @param out_qid 输出的 qid
 * @return int 0表示成功，非0表示错误码
 */
int m9p_client_walk_path(struct m9p_client *client, const char *path, uint16_t *out_fid, struct m9p_qid *out_qid)
{
    uint16_t fid;

    if (client == NULL || path == NULL || out_fid == NULL || out_qid == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    if (!client->attached) {
        return -(int)M9P_ERR_EPERM;
    }

    /* cluster_vfs open/stat 使用这个 helper：从 root fid walk 到远端绝对路径。 */
    fid = m9p_client_alloc_fid(client);
    if (fid < M9P_FIRST_DYNAMIC_FID) {
        return -(int)M9P_ERR_EFID;
    }
    {
        int rc = m9p_client_walk(client, M9P_ROOT_FID, fid, path, out_qid);

        if (rc != 0) {
            return rc;
        }
    }

    *out_fid = fid;
    return 0;
}

/**
 * @brief 客户端 OPEN 路径操作
 * 
 * @param client 客户端实例指针
 * @param path 路径
 * @param mode 打开模式
 * @param out_fid 输出的 fid
 * @param out_result 输出的打开结果
 * @return int 0表示成功，非0表示错误码
 */
int m9p_client_open_path(
    struct m9p_client *client,
    const char *path,
    uint8_t mode,
    uint16_t *out_fid,
    struct m9p_open_result *out_result)
{
    struct m9p_qid qid;
    uint16_t fid;
    int rc;

    if (client == NULL || path == NULL || out_fid == NULL || out_result == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    /* open_path = walk 分配 fid + open；open 失败时必须 clunk 释放 fid。 */
    rc = m9p_client_walk_path(client, path, &fid, &qid);
    if (rc != 0) {
        return rc;
    }

    rc = m9p_client_open(client, fid, mode, out_result);
    if (rc != 0) {
        (void)m9p_client_clunk(client, fid);
        return rc;
    }

    *out_fid = fid;
    return 0;
}
