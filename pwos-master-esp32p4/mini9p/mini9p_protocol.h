#ifndef MINI9P_PROTOCOL_H
#define MINI9P_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define M9P_VERSION 0x01u
#define M9P_MAX_PATH_LEN 255u
#define M9P_MAX_NAME_LEN 64u
#define M9P_MAX_ERROR_TEXT 64u
#define M9P_FRAME_OVERHEAD 10u

// QID
#define M9P_QID_DIR 0x80u       // 目录
#define M9P_QID_VIRTUAL 0x08u   // 虚拟文件
#define M9P_QID_DEVICE 0x04u    // 设备文件
#define M9P_QID_COMPUTE 0x02u   // 计算文件
#define M9P_QID_READONLY 0x01u  // 只读文件

// STAT
#define M9P_STAT_DIR 0x01u
#define M9P_STAT_VIRTUAL 0x02u
#define M9P_STAT_DEVICE 0x04u
#define M9P_STAT_COMPUTE 0x08u

#define M9P_FEATURE_DIRECTORY_READ 0x00000001u
#define M9P_FEATURE_MULTI_TAG 0x00000002u
#define M9P_FEATURE_RESPONSE_CACHE 0x00000004u
#define M9P_FEATURE_ATTACH_FLAGS 0x00000008u

enum m9p_type {
    M9P_TATTACH = 0x01,
    M9P_RWALK = 0x82,
    M9P_TWALK = 0x02,
    M9P_RATTACH = 0x81,
    M9P_TOPEN = 0x03,
    M9P_ROPEN = 0x83,
    M9P_TREAD = 0x04,
    M9P_RREAD = 0x84,
    M9P_TWRITE = 0x05,
    M9P_RWRITE = 0x85,
    M9P_TSTAT = 0x06,
    M9P_RSTAT = 0x86,
    M9P_TCLUNK = 0x07,
    M9P_RCLUNK = 0x87,
    M9P_RERROR = 0xFF
};

enum m9p_open_mode {
    M9P_OREAD = 0x00,
    M9P_OWRITE = 0x01,
    M9P_ORDWR = 0x02,
    M9P_OTRUNC = 0x10
};

enum m9p_error_code {
    M9P_ERR_EINVAL = 0x0001,
    M9P_ERR_ENOENT = 0x0002,
    M9P_ERR_EPERM = 0x0003,
    M9P_ERR_EBUSY = 0x0004,
    M9P_ERR_ENOTDIR = 0x0005,
    M9P_ERR_EISDIR = 0x0006,
    M9P_ERR_EOFFS = 0x0007,
    M9P_ERR_EMSIZE = 0x0008,
    M9P_ERR_EIO = 0x0009,
    M9P_ERR_ENOTSUP = 0x000A,
    M9P_ERR_ETAG = 0x000B,
    M9P_ERR_EFID = 0x000C,
    M9P_ERR_EAGAIN = 0x000D
};

// qid 数据结构，每个对象都有一个唯一的 qid 来标识它
struct m9p_qid {
    uint8_t type;
    uint8_t reserved;
    uint16_t version;
    uint32_t object_id;
};

// 解码后帧的只读视图
struct m9p_frame_view {
    uint8_t version;
    uint8_t type;
    uint16_t tag;
    const uint8_t *payload;
    uint16_t payload_len;
};

// attach 响应的结果
struct m9p_attach_result {
    uint16_t negotiated_msize;
    uint8_t max_fids;
    uint8_t max_inflight;
    uint32_t feature_bits;
    struct m9p_qid root_qid;
};

// open 响应的结果
struct m9p_open_result {
    struct m9p_qid qid;
    uint16_t iounit;
};

// stat 响应的结果
struct m9p_stat {
    struct m9p_qid qid;
    uint8_t perm;       // 权限
    uint8_t flags;      // 标志
    uint32_t size;      // 文件大小
    uint32_t mtime;     // 最后修改时间
    char name[M9P_MAX_NAME_LEN + 1u];
};

// 错误响应的结果
struct m9p_error {
    uint16_t code;
    char msg[M9P_MAX_ERROR_TEXT + 1u];
};

// 目录项
struct m9p_dirent {
    struct m9p_qid qid;
    uint8_t perm;       // 权限
    uint8_t flags;      // 标志
    char name[M9P_MAX_NAME_LEN + 1u];
};

// 编解码
uint16_t m9p_crc16_ccitt_false(const uint8_t *data, size_t len);
bool m9p_encode_frame(
    uint8_t type,
    uint16_t tag,
    const uint8_t *payload,
    uint16_t payload_len,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len);
bool m9p_decode_frame(const uint8_t *frame, size_t frame_len, struct m9p_frame_view *out_view);

// 请求构造
bool m9p_build_tattach(
    uint16_t tag,
    uint16_t fid,
    uint16_t requested_msize,
    uint8_t requested_inflight,
    uint8_t attach_flags,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len);
bool m9p_build_twalk(
    uint16_t tag,
    uint16_t fid,
    uint16_t newfid,
    const char *path,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len);
bool m9p_build_topen(
    uint16_t tag,
    uint16_t fid,
    uint8_t mode,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len);
bool m9p_build_tread(
    uint16_t tag,
    uint16_t fid,
    uint32_t offset,
    uint16_t count,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len);
bool m9p_build_twrite(
    uint16_t tag,
    uint16_t fid,
    uint32_t offset,
    const uint8_t *data,
    uint16_t count,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len);
bool m9p_build_tstat(
    uint16_t tag,
    uint16_t fid,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len);
bool m9p_build_tclunk(
    uint16_t tag,
    uint16_t fid,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len);

    // 响应解析
bool m9p_parse_rattach(const struct m9p_frame_view *frame, struct m9p_attach_result *out_result);
bool m9p_parse_rwalk(const struct m9p_frame_view *frame, struct m9p_qid *out_qid);
bool m9p_parse_ropen(const struct m9p_frame_view *frame, struct m9p_open_result *out_result);
bool m9p_parse_rread(
    const struct m9p_frame_view *frame,
    uint8_t *out_data,
    uint16_t out_cap,
    uint16_t *out_count);
bool m9p_parse_rwrite(const struct m9p_frame_view *frame, uint16_t *out_count);
bool m9p_parse_rstat(const struct m9p_frame_view *frame, struct m9p_stat *out_stat);
bool m9p_parse_rerror(const struct m9p_frame_view *frame, struct m9p_error *out_error);
size_t m9p_parse_dirents(
    const uint8_t *data,
    size_t data_len,
    struct m9p_dirent *entries,
    size_t max_entries);
const char *m9p_error_name(uint16_t code);

#endif