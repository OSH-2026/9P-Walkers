/**
 * @file mini9p_server.c
 * @author hb (huobin92@gmail.com)
 * @brief 
 * @version 0.1
 * @date 2026-04-23
 * 
 * @copyright Copyright (c) 2026
 * 
 */

#include "mini9p_server.h"

#include <string.h>

#define M9P_SERVER_MIN_MSIZE 128u
#define M9P_SERVER_DEFAULT_IOUNIT 256u

static const struct m9p_qid k_default_root_qid = {
    M9P_QID_DIR,
    0u,
    1u,
    1u,
};

/* ==================================================================
 *  1. 基础工具函数
 *     取较小值、backend 错误码归一化、协议错误码提取、qid 判空。
 *  ================================================================== */

/**
 * @brief 返回两个 uint16_t 中的较小值。
 * @param[in] a 第一个值。
 * @param[in] b 第二个值。
 * @return a 与 b 中的较小者。
 */
static uint16_t min_u16(uint16_t a, uint16_t b)
{
    return a < b ? a : b;
}

/**
 * @brief 返回两个 uint8_t 中的较小值。
 * @param[in] a 第一个值。
 * @param[in] b 第二个值。
 * @return a 与 b 中的较小者。
 */
static uint8_t min_u8(uint8_t a, uint8_t b)
{
    return a < b ? a : b;
}

/**
 * @brief 将 backend 返回值统一为负错误码。
 * @param[in] rc backend 原始返回值。
 * @return 若 rc 为正则返回 -rc，否则直接返回 rc。
 */
static int normalize_backend_rc(int rc)
{
    if (rc > 0) {
        return -rc;
    }
    return rc;
}

/**
 * @brief 将内部错误码提取为协议级错误码，用于构造 RERROR 响应。
 * @param[in] rc 内部错误码（可为负数）。
 * @return 对应的 uint16_t 协议错误码；无法识别时返回 M9P_ERR_EIO。
 */
static uint16_t error_code_from_rc(int rc)
{
    int code = rc < 0 ? -rc : rc;

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
        return (uint16_t)code;
    default:
        return M9P_ERR_EIO;
    }
}

/**
 * @brief 判断 qid 是否为零值（表示未传入有效的 root qid）。
 * @param[in] qid 指向 m9p_qid 的指针。
 * @return true 表示所有字段均为零；false 表示至少一个字段非零。
 */
static bool qid_is_zero(const struct m9p_qid *qid)
{
    return qid->type == 0u && qid->reserved == 0u && qid->version == 0u && qid->object_id == 0u;
}

/* ==================================================================
 *  2. mode、fid、路径管理
 *     打开模式校验、fid 表查找与分配、路径安全拼接与规范化。
 *  ================================================================== */

/**
 * @brief 检查 TOPEN 请求的 mode 是否合法。
 * @param[in] mode 打开模式（含 access 位与可选的 OTRUNC）。
 * @return true 表示 mode 合法；false 表示非法。
 */
static bool mode_is_valid(uint8_t mode)
{
    const uint8_t access = (uint8_t)(mode & 0x03u);

    if ((mode & (uint8_t)~(M9P_OTRUNC | 0x03u)) != 0u) {
        return false;
    }
    if (access > M9P_ORDWR) {
        return false;
    }
    if ((mode & M9P_OTRUNC) != 0u && access == M9P_OREAD) {
        return false;
    }
    return true;
}

/**
 * @brief 判断当前打开模式是否允许读。
 * @param[in] mode 打开模式。
 * @return true 允许读；false 不允许。
 */
static bool mode_allows_read(uint8_t mode)
{
    const uint8_t access = (uint8_t)(mode & 0x03u);

    return access == M9P_OREAD || access == M9P_ORDWR;
}

/**
 * @brief 判断当前打开模式是否允许写。
 * @param[in] mode 打开模式。
 * @return true 允许写；false 不允许。
 */
static bool mode_allows_write(uint8_t mode)
{
    const uint8_t access = (uint8_t)(mode & 0x03u);

    return access == M9P_OWRITE || access == M9P_ORDWR;
}

/**
 * @brief 清空一个 fid 表项。
 * @param[in,out] entry 指向 fid 表项的指针；为 NULL 时不做任何操作。
 */
static void clear_fid(struct m9p_server_fid *entry)
{
    if (entry != NULL) {
        memset(entry, 0, sizeof(*entry));
    }
}

/**
 * @brief 按 fid 编号查找已使用的表项。
 * @param[in] server Mini9P server 实例。
 * @param[in] fid 要查找的 fid 编号。
 * @return 找到则返回对应表项指针；未找到返回 NULL。
 */
static struct m9p_server_fid *find_fid(struct m9p_server *server, uint16_t fid)
{
    uint8_t i;

    for (i = 0u; i < server->max_fids; ++i) {
        if (server->fids[i].in_use && server->fids[i].fid == fid) {
            return &server->fids[i];
        }
    }
    return NULL;
}

/**
 * @brief 查找第一个空闲的 fid 表项。
 * @param[in] server Mini9P server 实例。
 * @return 找到则返回空闲表项指针；无空闲返回 NULL。
 */
static struct m9p_server_fid *find_free_fid(struct m9p_server *server)
{
    uint8_t i;

    for (i = 0u; i < server->max_fids; ++i) {
        if (!server->fids[i].in_use) {
            return &server->fids[i];
        }
    }
    return NULL;
}

/**
 * @brief 判断 session 中是否还有活跃的 fid。
 * @param[in] server Mini9P server 实例。
 * @return true 表示至少有一个 fid 处于使用中；false 表示全部空闲。
 */
static bool has_live_fids(const struct m9p_server *server)
{
    uint8_t i;

    for (i = 0u; i < server->max_fids; ++i) {
        if (server->fids[i].in_use) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 安全复制路径字符串，确保不会溢出目标缓冲区。
 * @param[out] dst 目标缓冲区。
 * @param[in] dst_cap 目标缓冲区容量（字节）。
 * @param[in] src 源路径字符串。
 * @return 成功返回 0；参数非法或路径过长返回 -M9P_ERR_EINVAL。
 */
static int copy_path(char *dst, size_t dst_cap, const char *src)
{
    size_t len;

    if (dst == NULL || dst_cap == 0u || src == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    len = strlen(src);
    if (len >= dst_cap) {
        return -(int)M9P_ERR_EINVAL;
    }

    memcpy(dst, src, len + 1u);
    return 0;
}

/**
 * @brief 检查路径中是否包含 ".." 上级目录引用。防御函数。
 * @param[in] path 要检查的路径字符串。
 * @return true 包含 ".."；false 不包含。
 */
static bool path_has_parent_ref(const char *path)
{
    const char *p = path;

    while (p != NULL && *p != '\0') {
        const char *segment;
        size_t len = 0u;

        while (*p == '/') {
            ++p;
        }
        segment = p;
        while (*p != '\0' && *p != '/') {
            ++p;
            ++len;
        }
        if (len == 2u && segment[0] == '.' && segment[1] == '.') {
            return true;
        }
    }

    return false;
}

/**
 * @brief 去除路径末尾的多余斜杠（保留根目录 "/"）。
 * @param[in,out] path 要处理的路径字符串；为 NULL 时不做任何操作。
 */
static void trim_trailing_slash(char *path)
{
    size_t len;

    if (path == NULL) {
        return;
    }

    len = strlen(path);
    while (len > 1u && path[len - 1u] == '/') {
        path[len - 1u] = '\0';
        --len;
    }
}

/**
 * @brief 将 fid 当前路径与 TWALK 的 path 合成为最终绝对路径。
 * @param[in] base_path 基础路径（通常为 fid->path）。
 * @param[in] walk_path TWALK 请求中的相对或绝对路径。
 * @param[out] out_path 输出缓冲区，存放合成后的绝对路径。
 * @param[in] out_cap 输出缓冲区容量。
 * @return 成功返回 0；参数非法、路径过长或包含 ".." 返回 -M9P_ERR_EINVAL。
 */
static int resolve_path(const char *base_path, const char *walk_path, char *out_path, size_t out_cap)
{
    size_t base_len;
    size_t walk_len;

    if (base_path == NULL || out_path == NULL || out_cap < 2u) {
        return -(int)M9P_ERR_EINVAL;
    }

    if (walk_path == NULL || walk_path[0] == '\0') {
        return copy_path(out_path, out_cap, base_path);
    }

    walk_len = strlen(walk_path);
    if (walk_len == 0u || walk_len > M9P_MAX_PATH_LEN) {
        return -(int)M9P_ERR_EINVAL;
    }

    if (walk_path[0] == '/') {
        if (walk_len >= out_cap) {
            return -(int)M9P_ERR_EINVAL;
        }
        memcpy(out_path, walk_path, walk_len + 1u);
    } else {
        base_len = strlen(base_path);
        if (base_len == 0u || base_path[0] != '/') {
            return -(int)M9P_ERR_EINVAL;
        }
        if (base_len == 1u) {
            if (1u + walk_len >= out_cap) {
                return -(int)M9P_ERR_EINVAL;
            }
            out_path[0] = '/';
            memcpy(out_path + 1u, walk_path, walk_len + 1u);
        } else {
            if (base_len + 1u + walk_len >= out_cap) {
                return -(int)M9P_ERR_EINVAL;
            }
            memcpy(out_path, base_path, base_len);
            out_path[base_len] = '/';
            memcpy(out_path + base_len + 1u, walk_path, walk_len + 1u);
        }
    }

    trim_trailing_slash(out_path);
    if (path_has_parent_ref(out_path)) {
        return -(int)M9P_ERR_EINVAL;
    }
    return 0;
}

/* ==================================================================
 *  3. backend 和响应辅助
 *     backend stat/open/read/write/clunk 封装、权限校验、iounit 限制、
 *     RERROR 响应构造。
 *  ================================================================== */

/**
 * @brief 调用 backend 的 stat() 操作，用于 walk / open / stat 的基础查询。
 * @param[in] server Mini9P server 实例。
 * @param[in] path 要查询的路径。
 * @param[out] out_stat 输出 stat 信息。
 * @return 成功返回 0；未实现返回 -M9P_ERR_ENOTSUP；其他返回 backend 错误码。
 */
static int backend_stat(struct m9p_server *server, const char *path, struct m9p_stat *out_stat)
{
    if (server->ops == NULL || server->ops->stat == NULL) {
        return -(int)M9P_ERR_ENOTSUP;
    }
    return normalize_backend_rc(server->ops->stat(server->ops_ctx, path, out_stat));
}

/**
 * @brief 根据 stat 的 perm 与 flags 校验打开模式是否被允许。
 * @param[in] stat 目标路径的 stat 信息。
 * @param[in] mode 请求的打开模式。
 * @return 成功返回 0；目录不允许写返回 -M9P_ERR_EISDIR；权限不足返回 -M9P_ERR_EPERM。
 */
static int validate_open_stat(const struct m9p_stat *stat, uint8_t mode)
{
    if ((stat->flags & M9P_STAT_DIR) != 0u && mode_allows_write(mode)) {
        return -(int)M9P_ERR_EISDIR;
    }
    if (mode_allows_read(mode) && (stat->perm & M9P_SERVER_PERM_READ) == 0u) {
        return -(int)M9P_ERR_EPERM;
    }
    if (mode_allows_write(mode) && (stat->perm & M9P_SERVER_PERM_WRITE) == 0u) {
        return -(int)M9P_ERR_EPERM;
    }
    return 0;
}

/**
 * @brief 将单次 I/O 单元大小限制在协商 msize、内部 buffer、backend 建议值的交集内。
 * @param[in] server Mini9P server 实例。
 * @param[in] iounit backend 建议的 iounit；为 0 时使用 default_iounit。
 * @return 限制后的 iounit。
 */
static uint16_t clamp_iounit(struct m9p_server *server, uint16_t iounit)
{
    uint16_t max_payload;
    uint16_t limit;

    if (iounit == 0u) {
        iounit = server->default_iounit;
    }

    max_payload = server->negotiated_msize > (M9P_FRAME_OVERHEAD + 2u)
        ? (uint16_t)(server->negotiated_msize - M9P_FRAME_OVERHEAD - 2u)
        : 0u;
    limit = min_u16(max_payload, M9P_SERVER_IO_BUFFER_CAP);
    return min_u16(iounit, limit);
}

/**
 * @brief 构造 RERROR 响应帧。
 * @param[in] tag 请求 tag。
 * @param[in] code 协议错误码。
 * @param[out] response_data 响应数据缓冲区。
 * @param[in] response_cap 响应缓冲区容量。
 * @param[out] response_len 实际写入的响应长度。
 * @return 成功返回 0；缓冲区不足返回 -M9P_ERR_EMSIZE。
 */
static int build_error_response(uint16_t tag,
                                uint16_t code,
                                uint8_t *response_data,
                                size_t response_cap,
                                size_t *response_len)
{
    if (!m9p_build_rerror(tag, code, m9p_error_name(code), response_data, response_cap, response_len)) {
        return -(int)M9P_ERR_EMSIZE;
    }
    return 0;
}

/**
 * @brief 将内部错误码转换为 RERROR 响应帧。
 * @param[in] tag 请求 tag。
 * @param[in] rc 内部错误码（可为负数）。
 * @param[out] response_data 响应数据缓冲区。
 * @param[in] response_cap 响应缓冲区容量。
 * @param[out] response_len 实际写入的响应长度。
 * @return 成功返回 0；缓冲区不足返回 -M9P_ERR_EMSIZE。
 */
static int build_error_from_rc(uint16_t tag,
                               int rc,
                               uint8_t *response_data,
                               size_t response_cap,
                               size_t *response_len)
{
    return build_error_response(tag, error_code_from_rc(rc), response_data, response_cap, response_len);
}

/* ==================================================================
 *  4. 公开初始化函数
 *     生成默认配置、初始化 server 实例、重置 session 状态。
 *  ================================================================== */

/**
 * @brief 生成默认的 server 配置。
 * @param[out] out_config 输出配置结构体；为 NULL 时不做任何操作。
 */
void m9p_server_get_default_config(struct m9p_server_config *out_config)
{
    if (out_config == NULL) {
        return;
    }

    memset(out_config, 0, sizeof(*out_config));
    out_config->max_msize = M9P_SERVER_DEFAULT_MSIZE;
    out_config->max_fids = M9P_SERVER_MAX_FIDS;
    out_config->max_inflight = M9P_SERVER_DEFAULT_MAX_INFLIGHT;
    out_config->default_iounit = M9P_SERVER_DEFAULT_IOUNIT;
    out_config->feature_bits = M9P_FEATURE_DIRECTORY_READ;
    out_config->root_qid = k_default_root_qid;
}

/**
 * @brief 初始化 Mini9P server 实例。
 * @param[in,out] server 要初始化的 server 实例。
 * @param[in] config 用户配置；为 NULL 时使用默认配置。
 * @return 成功返回 0；server 为 NULL 返回 -M9P_ERR_EINVAL。
 */
int m9p_server_init(struct m9p_server *server, const struct m9p_server_config *config)
{
    struct m9p_server_config default_config;
    const struct m9p_server_config *active_config = config;

    if (server == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    if (active_config == NULL) {
        m9p_server_get_default_config(&default_config);
        active_config = &default_config;
    }

    memset(server, 0, sizeof(*server));
    server->ops = active_config->ops;
    server->ops_ctx = active_config->ops_ctx;
    server->max_msize = active_config->max_msize == 0u ? M9P_SERVER_DEFAULT_MSIZE : active_config->max_msize;
    if (server->max_msize < M9P_SERVER_MIN_MSIZE) {
        server->max_msize = M9P_SERVER_MIN_MSIZE;
    }
    server->max_fids = active_config->max_fids == 0u ? M9P_SERVER_MAX_FIDS : active_config->max_fids;
    server->max_fids = min_u8(server->max_fids, M9P_SERVER_MAX_FIDS);
    server->max_inflight = active_config->max_inflight == 0u
        ? M9P_SERVER_DEFAULT_MAX_INFLIGHT
        : active_config->max_inflight;
    server->default_iounit = active_config->default_iounit == 0u
        ? M9P_SERVER_DEFAULT_IOUNIT
        : active_config->default_iounit;
    server->default_iounit = min_u16(server->default_iounit, M9P_SERVER_IO_BUFFER_CAP);
    server->feature_bits = active_config->feature_bits;
    server->root_qid = qid_is_zero(&active_config->root_qid) ? k_default_root_qid : active_config->root_qid;
    server->root_fid = M9P_SERVER_ROOT_FID;
    server->negotiated_msize = server->max_msize;
    return 0;
}

/**
 * @brief 重置 session：隐式 clunk 所有活跃 fid，清空表项并恢复初始状态。
 * @param[in,out] server Mini9P server 实例；为 NULL 时不做任何操作。
 */
void m9p_server_reset_session(struct m9p_server *server)
{
    uint8_t i;

    if (server == NULL) {
        return;
    }

    for (i = 0u; i < server->max_fids; ++i) {
        struct m9p_server_fid *entry = &server->fids[i];

        if (entry->in_use && server->ops != NULL && server->ops->clunk != NULL) {
            (void)server->ops->clunk(server->ops_ctx, entry->path, entry->open);
        }
        clear_fid(entry);
    }
    server->attached = false;
    server->root_fid = M9P_SERVER_ROOT_FID;
    server->negotiated_msize = server->max_msize;
}

/* ==================================================================
 *  5. 请求 handler
 *     TATTACH / TWALK / TOPEN / TREAD / TWRITE / TSTAT / TCLUNK 的
 *     请求处理与响应构造。
 *  ================================================================== */

/**
 * @brief 处理 TATTACH 请求：协商 msize / inflight，建立 root fid，返回 RATTACH。
 * @param[in,out] server Mini9P server 实例。
 * @param[in] frame 解析后的请求帧视图。
 * @param[out] response_data 响应数据缓冲区。
 * @param[in] response_cap 响应缓冲区容量。
 * @param[out] response_len 实际写入的响应长度。
 * @return 成功返回 0；失败返回负错误码或 RERROR 构建错误码。
 */
static int handle_tattach(struct m9p_server *server,
                          const struct m9p_frame_view *frame,
                          uint8_t *response_data,
                          size_t response_cap,
                          size_t *response_len)
{
    struct m9p_attach_request request;
    struct m9p_stat root_stat;
    struct m9p_server_fid *root_entry;
    uint16_t requested_msize;
    uint8_t negotiated_inflight;
    int rc;

    if (!m9p_parse_tattach(frame, &request)) {
        return build_error_response(frame->tag, M9P_ERR_EINVAL, response_data, response_cap, response_len);
    }
    if (request.attach_flags != 0u && (server->feature_bits & M9P_FEATURE_ATTACH_FLAGS) == 0u) {
        return build_error_response(frame->tag, M9P_ERR_ENOTSUP, response_data, response_cap, response_len);
    }

    requested_msize = request.requested_msize == 0u ? server->max_msize : request.requested_msize;
    if (requested_msize < M9P_SERVER_MIN_MSIZE) {
        return build_error_response(frame->tag, M9P_ERR_EMSIZE, response_data, response_cap, response_len);
    }

    negotiated_inflight = request.requested_inflight == 0u
        ? 1u
        : min_u8(request.requested_inflight, server->max_inflight);

    m9p_server_reset_session(server);
    server->negotiated_msize = min_u16(requested_msize, server->max_msize);
    server->root_fid = request.fid;

    if (server->ops != NULL && server->ops->stat != NULL) {
        rc = backend_stat(server, "/", &root_stat);
        if (rc != 0) {
            return build_error_from_rc(frame->tag, rc, response_data, response_cap, response_len);
        }
        server->root_qid = root_stat.qid;
    }

    if (!m9p_build_rattach(
            frame->tag,
            server->negotiated_msize,
            server->max_fids,
            negotiated_inflight,
            server->feature_bits,
            &server->root_qid,
            response_data,
            response_cap,
            response_len)) {
        m9p_server_reset_session(server);
        return -(int)M9P_ERR_EMSIZE;
    }

    root_entry = find_free_fid(server);
    if (root_entry == NULL) {
        m9p_server_reset_session(server);
        return build_error_response(frame->tag, M9P_ERR_EBUSY, response_data, response_cap, response_len);
    }

    root_entry->in_use = true;
    root_entry->fid = request.fid;
    root_entry->qid = server->root_qid;
    root_entry->iounit = clamp_iounit(server, server->default_iounit);
    (void)copy_path(root_entry->path, sizeof(root_entry->path), "/");
    server->attached = true;
    return 0;
}

/**
 * @brief 处理 TWALK 请求：根据旧 fid 与 path 解析目标路径，确认存在后创建 newfid，返回 RWALK。
 * @param[in,out] server Mini9P server 实例。
 * @param[in] frame 解析后的请求帧视图。
 * @param[out] response_data 响应数据缓冲区。
 * @param[in] response_cap 响应缓冲区容量。
 * @param[out] response_len 实际写入的响应长度。
 * @return 成功返回 0；失败返回负错误码或 RERROR 构建错误码。
 */
static int handle_twalk(struct m9p_server *server,
                        const struct m9p_frame_view *frame,
                        uint8_t *response_data,
                        size_t response_cap,
                        size_t *response_len)
{
    struct m9p_walk_request request;
    struct m9p_server_fid *source;
    struct m9p_server_fid *target;
    struct m9p_stat stat;
    char path[M9P_MAX_PATH_LEN + 1u];
    int rc;

    if (!server->attached) {
        return build_error_response(frame->tag, M9P_ERR_EPERM, response_data, response_cap, response_len);
    }
    if (!m9p_parse_twalk(frame, &request)) {
        return build_error_response(frame->tag, M9P_ERR_EINVAL, response_data, response_cap, response_len);
    }
    if (find_fid(server, request.newfid) != NULL) {
        return build_error_response(frame->tag, M9P_ERR_EFID, response_data, response_cap, response_len);
    }

    source = find_fid(server, request.fid);
    if (source == NULL || source->open) {
        return build_error_response(frame->tag, M9P_ERR_EFID, response_data, response_cap, response_len);
    }

    rc = resolve_path(source->path, request.path, path, sizeof(path));
    if (rc != 0) {
        return build_error_from_rc(frame->tag, rc, response_data, response_cap, response_len);
    }

    rc = backend_stat(server, path, &stat);
    if (rc != 0) {
        return build_error_from_rc(frame->tag, rc, response_data, response_cap, response_len);
    }

    target = find_free_fid(server);
    if (target == NULL) {
        return build_error_response(frame->tag, M9P_ERR_EBUSY, response_data, response_cap, response_len);
    }

    target->in_use = true;
    target->fid = request.newfid;
    target->qid = stat.qid;
    target->iounit = clamp_iounit(server, server->default_iounit);
    rc = copy_path(target->path, sizeof(target->path), path);
    if (rc != 0) {
        clear_fid(target);
        return build_error_from_rc(frame->tag, rc, response_data, response_cap, response_len);
    }

    if (!m9p_build_rwalk(frame->tag, &target->qid, response_data, response_cap, response_len)) {
        clear_fid(target);
        return -(int)M9P_ERR_EMSIZE;
    }
    return 0;
}

/**
 * @brief 处理 TOPEN 请求：校验 mode、权限与目录写入限制，调用 backend open，返回 ROPEN。
 * @param[in,out] server Mini9P server 实例。
 * @param[in] frame 解析后的请求帧视图。
 * @param[out] response_data 响应数据缓冲区。
 * @param[in] response_cap 响应缓冲区容量。
 * @param[out] response_len 实际写入的响应长度。
 * @return 成功返回 0；失败返回负错误码或 RERROR 构建错误码。
 */
static int handle_topen(struct m9p_server *server,
                        const struct m9p_frame_view *frame,
                        uint8_t *response_data,
                        size_t response_cap,
                        size_t *response_len)
{
    struct m9p_open_request request;
    struct m9p_server_fid *entry;
    struct m9p_stat stat;
    struct m9p_qid qid;
    uint16_t iounit;
    bool backend_opened = false;
    int rc;

    if (!server->attached) {
        return build_error_response(frame->tag, M9P_ERR_EPERM, response_data, response_cap, response_len);
    }
    if (!m9p_parse_topen(frame, &request) || !mode_is_valid(request.mode)) {
        return build_error_response(frame->tag, M9P_ERR_EINVAL, response_data, response_cap, response_len);
    }

    entry = find_fid(server, request.fid);
    if (entry == NULL) {
        return build_error_response(frame->tag, M9P_ERR_EFID, response_data, response_cap, response_len);
    }
    if (entry->open) {
        return build_error_response(frame->tag, M9P_ERR_EBUSY, response_data, response_cap, response_len);
    }

    rc = backend_stat(server, entry->path, &stat);
    if (rc != 0) {
        return build_error_from_rc(frame->tag, rc, response_data, response_cap, response_len);
    }
    rc = validate_open_stat(&stat, request.mode);
    if (rc != 0) {
        return build_error_from_rc(frame->tag, rc, response_data, response_cap, response_len);
    }

    qid = stat.qid;
    iounit = server->default_iounit;
    if (server->ops != NULL && server->ops->open != NULL) {
        rc = normalize_backend_rc(server->ops->open(server->ops_ctx, entry->path, request.mode, &qid, &iounit));
        if (rc != 0) {
            return build_error_from_rc(frame->tag, rc, response_data, response_cap, response_len);
        }
        backend_opened = true;
    }

    if (!m9p_build_ropen(frame->tag, &qid, clamp_iounit(server, iounit), response_data, response_cap, response_len)) {
        if (backend_opened && server->ops->clunk != NULL) {
            (void)server->ops->clunk(server->ops_ctx, entry->path, true);
        }
        return -(int)M9P_ERR_EMSIZE;
    }

    entry->open = true;
    entry->mode = request.mode;
    entry->qid = qid;
    entry->iounit = clamp_iounit(server, iounit);
    return 0;
}

/**
 * @brief 处理 TREAD 请求：检查 fid 已 open 且允许读，调用 backend read，返回 RREAD。
 * @param[in,out] server Mini9P server 实例。
 * @param[in] frame 解析后的请求帧视图。
 * @param[out] response_data 响应数据缓冲区。
 * @param[in] response_cap 响应缓冲区容量。
 * @param[out] response_len 实际写入的响应长度。
 * @return 成功返回 0；失败返回负错误码或 RERROR 构建错误码。
 */
static int handle_tread(struct m9p_server *server,
                        const struct m9p_frame_view *frame,
                        uint8_t *response_data,
                        size_t response_cap,
                        size_t *response_len)
{
    struct m9p_read_request request;
    struct m9p_server_fid *entry;
    uint16_t cap;
    uint16_t response_data_cap;
    uint16_t count;
    int rc;

    if (!server->attached) {
        return build_error_response(frame->tag, M9P_ERR_EPERM, response_data, response_cap, response_len);
    }
    if (!m9p_parse_tread(frame, &request)) {
        return build_error_response(frame->tag, M9P_ERR_EINVAL, response_data, response_cap, response_len);
    }

    entry = find_fid(server, request.fid);
    if (entry == NULL || !entry->open) {
        return build_error_response(frame->tag, M9P_ERR_EFID, response_data, response_cap, response_len);
    }
    if (!mode_allows_read(entry->mode)) {
        return build_error_response(frame->tag, M9P_ERR_EPERM, response_data, response_cap, response_len);
    }
    if (server->ops == NULL || server->ops->read == NULL) {
        return build_error_response(frame->tag, M9P_ERR_ENOTSUP, response_data, response_cap, response_len);
    }

    response_data_cap = response_cap > (M9P_FRAME_OVERHEAD + 2u)
        ? (uint16_t)(response_cap - M9P_FRAME_OVERHEAD - 2u)
        : 0u;
    cap = min_u16(clamp_iounit(server, entry->iounit), response_data_cap);
    cap = min_u16(cap, request.count);
    count = cap;
    rc = normalize_backend_rc(server->ops->read(
        server->ops_ctx,
        entry->path,
        request.offset,
        entry->mode,
        server->io_buffer,
        cap,
        &count));
    if (rc != 0) {
        return build_error_from_rc(frame->tag, rc, response_data, response_cap, response_len);
    }
    if (count > cap) {
        return build_error_response(frame->tag, M9P_ERR_EIO, response_data, response_cap, response_len);
    }

    if (!m9p_build_rread(frame->tag, server->io_buffer, count, response_data, response_cap, response_len)) {
        return -(int)M9P_ERR_EMSIZE;
    }
    return 0;
}

/**
 * @brief 处理 TWRITE 请求：检查 fid 已 open 且允许写，调用 backend write，返回 RWRITE。
 * @param[in,out] server Mini9P server 实例。
 * @param[in] frame 解析后的请求帧视图。
 * @param[out] response_data 响应数据缓冲区。
 * @param[in] response_cap 响应缓冲区容量。
 * @param[out] response_len 实际写入的响应长度。
 * @return 成功返回 0；失败返回负错误码或 RERROR 构建错误码。
 */
static int handle_twrite(struct m9p_server *server,
                         const struct m9p_frame_view *frame,
                         uint8_t *response_data,
                         size_t response_cap,
                         size_t *response_len)
{
    struct m9p_write_request request;
    struct m9p_server_fid *entry;
    uint16_t written;
    int rc;

    if (!server->attached) {
        return build_error_response(frame->tag, M9P_ERR_EPERM, response_data, response_cap, response_len);
    }
    if (!m9p_parse_twrite(frame, &request)) {
        return build_error_response(frame->tag, M9P_ERR_EINVAL, response_data, response_cap, response_len);
    }

    entry = find_fid(server, request.fid);
    if (entry == NULL || !entry->open) {
        return build_error_response(frame->tag, M9P_ERR_EFID, response_data, response_cap, response_len);
    }
    if (!mode_allows_write(entry->mode)) {
        return build_error_response(frame->tag, M9P_ERR_EPERM, response_data, response_cap, response_len);
    }
    if (request.count > clamp_iounit(server, entry->iounit)) {
        return build_error_response(frame->tag, M9P_ERR_EMSIZE, response_data, response_cap, response_len);
    }
    if (server->ops == NULL || server->ops->write == NULL) {
        return build_error_response(frame->tag, M9P_ERR_ENOTSUP, response_data, response_cap, response_len);
    }

    written = request.count;
    rc = normalize_backend_rc(server->ops->write(
        server->ops_ctx,
        entry->path,
        request.offset,
        entry->mode,
        request.data,
        request.count,
        &written));
    if (rc != 0) {
        return build_error_from_rc(frame->tag, rc, response_data, response_cap, response_len);
    }
    if (written > request.count) {
        return build_error_response(frame->tag, M9P_ERR_EIO, response_data, response_cap, response_len);
    }

    if (!m9p_build_rwrite(frame->tag, written, response_data, response_cap, response_len)) {
        return -(int)M9P_ERR_EMSIZE;
    }
    return 0;
}

/**
 * @brief 处理 TSTAT 请求：查询 fid 对应路径的 stat 信息，返回 RSTAT。
 * @param[in,out] server Mini9P server 实例。
 * @param[in] frame 解析后的请求帧视图。
 * @param[out] response_data 响应数据缓冲区。
 * @param[in] response_cap 响应缓冲区容量。
 * @param[out] response_len 实际写入的响应长度。
 * @return 成功返回 0；失败返回负错误码或 RERROR 构建错误码。
 */
static int handle_tstat(struct m9p_server *server,
                        const struct m9p_frame_view *frame,
                        uint8_t *response_data,
                        size_t response_cap,
                        size_t *response_len)
{
    uint16_t fid;
    struct m9p_server_fid *entry;
    struct m9p_stat stat;
    int rc;

    if (!server->attached) {
        return build_error_response(frame->tag, M9P_ERR_EPERM, response_data, response_cap, response_len);
    }
    if (!m9p_parse_tstat(frame, &fid)) {
        return build_error_response(frame->tag, M9P_ERR_EINVAL, response_data, response_cap, response_len);
    }

    entry = find_fid(server, fid);
    if (entry == NULL) {
        return build_error_response(frame->tag, M9P_ERR_EFID, response_data, response_cap, response_len);
    }

    rc = backend_stat(server, entry->path, &stat);
    if (rc != 0) {
        return build_error_from_rc(frame->tag, rc, response_data, response_cap, response_len);
    }
    entry->qid = stat.qid;

    if (!m9p_build_rstat(frame->tag, &stat, response_data, response_cap, response_len)) {
        return -(int)M9P_ERR_EMSIZE;
    }
    return 0;
}

/**
 * @brief 处理 TCLUNK 请求：释放 fid，必要时调用 backend clunk，返回 RCLUNK。
 * @param[in,out] server Mini9P server 实例。
 * @param[in] frame 解析后的请求帧视图。
 * @param[out] response_data 响应数据缓冲区。
 * @param[in] response_cap 响应缓冲区容量。
 * @param[out] response_len 实际写入的响应长度。
 * @return 成功返回 0；失败返回负错误码或 RERROR 构建错误码。
 */
static int handle_tclunk(struct m9p_server *server,
                         const struct m9p_frame_view *frame,
                         uint8_t *response_data,
                         size_t response_cap,
                         size_t *response_len)
{
    uint16_t fid;
    struct m9p_server_fid *entry;
    int rc = 0;

    if (!m9p_parse_tclunk(frame, &fid)) {
        return build_error_response(frame->tag, M9P_ERR_EINVAL, response_data, response_cap, response_len);
    }

    entry = find_fid(server, fid);
    if (entry == NULL) {
        return build_error_response(frame->tag, M9P_ERR_EFID, response_data, response_cap, response_len);
    }

    if (server->ops != NULL && server->ops->clunk != NULL) {
        rc = normalize_backend_rc(server->ops->clunk(server->ops_ctx, entry->path, entry->open));
    }
    if (rc != 0) {
        return build_error_from_rc(frame->tag, rc, response_data, response_cap, response_len);
    }

    clear_fid(entry);
    if (!has_live_fids(server)) {
        server->attached = false;
    }

    if (!m9p_build_rclunk(frame->tag, response_data, response_cap, response_len)) {
        return -(int)M9P_ERR_EMSIZE;
    }
    return 0;
}

/**
 * @brief Mini9P server 主入口：解码请求帧，校验后分发给对应的请求 handler。
 * @param[in,out] server_ctx server 实例指针（内部转换为 struct m9p_server *）。
 * @param[in] request_data 原始请求数据。
 * @param[in] request_len 请求数据长度。
 * @param[out] response_data 响应数据缓冲区。
 * @param[in] response_cap 响应缓冲区容量。
 * @param[out] response_len 实际写入的响应长度。
 * @return 成功返回 0；参数非法返回 -M9P_ERR_EINVAL；帧解码失败返回 -M9P_ERR_EIO；
 *         或返回各 handler 的返回值。
 */
int m9p_server_handle_frame(void *server_ctx,
                            const uint8_t *request_data,
                            size_t request_len,
                            uint8_t *response_data,
                            size_t response_cap,
                            size_t *response_len)
{
    struct m9p_server *server = (struct m9p_server *)server_ctx;
    struct m9p_frame_view frame;

    if (response_len != NULL) {
        *response_len = 0u;
    }
    if (server == NULL || request_data == NULL || response_data == NULL || response_len == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    if (!m9p_decode_frame(request_data, request_len, &frame)) {
        return -(int)M9P_ERR_EIO;
    }
    if (frame.version != M9P_VERSION) {
        return build_error_response(frame.tag, M9P_ERR_EINVAL, response_data, response_cap, response_len);
    }
    if (server->attached && frame.type != M9P_TATTACH && request_len > server->negotiated_msize) {
        return build_error_response(frame.tag, M9P_ERR_EMSIZE, response_data, response_cap, response_len);
    }

    switch (frame.type) {
    case M9P_TATTACH:
        return handle_tattach(server, &frame, response_data, response_cap, response_len);
    case M9P_TWALK:
        return handle_twalk(server, &frame, response_data, response_cap, response_len);
    case M9P_TOPEN:
        return handle_topen(server, &frame, response_data, response_cap, response_len);
    case M9P_TREAD:
        return handle_tread(server, &frame, response_data, response_cap, response_len);
    case M9P_TWRITE:
        return handle_twrite(server, &frame, response_data, response_cap, response_len);
    case M9P_TSTAT:
        return handle_tstat(server, &frame, response_data, response_cap, response_len);
    case M9P_TCLUNK:
        return handle_tclunk(server, &frame, response_data, response_cap, response_len);
    default:
        return build_error_response(frame.tag, M9P_ERR_ENOTSUP, response_data, response_cap, response_len);
    }
}
