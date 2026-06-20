#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pwos_link_parser.h"

#ifndef PWOS_LINK_GOLDEN_DIR
#define PWOS_LINK_GOLDEN_DIR "golden"
#endif

static int g_failures;

static void failf(const char *label, const char *detail)
{
    ++g_failures;
    printf("FAIL %s: %s\n", label, detail);
}

static void expect_int(const char *label, int actual, int expected)
{
    if (actual != expected) {
        char detail[160];

        snprintf(detail, sizeof(detail), "actual=%d expected=%d", actual, expected);
        failf(label, detail);
    }
}

static void expect_size(const char *label, size_t actual, size_t expected)
{
    if (actual != expected) {
        char detail[160];

        snprintf(detail, sizeof(detail), "actual=%lu expected=%lu",
                 (unsigned long)actual,
                 (unsigned long)expected);
        failf(label, detail);
    }
}

static void expect_bytes(const char *label, const uint8_t *actual, const uint8_t *expected, size_t len)
{
    if (memcmp(actual, expected, len) != 0) {
        failf(label, "bytes differ");
    }
}

static void put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static uint8_t test_payload_byte(size_t i)
{
    return (uint8_t)((i * 37u + 11u) & 0xFFu);
}

static size_t load_hex_file(const char *path, uint8_t *out, size_t cap)
{
    FILE *file = fopen(path, "r");
    size_t len = 0u;
    unsigned int value;

    if (file == NULL) {
        return 0u;
    }

    while (fscanf(file, "%2x", &value) == 1) {
        if (len >= cap) {
            fclose(file);
            return 0u;
        }
        out[len++] = (uint8_t)value;
    }

    fclose(file);
    return len;
}

static int build_frame(const uint8_t *payload, uint16_t payload_len, uint8_t *out, size_t cap, size_t *out_len)
{
    return pwos_link_encode(
        (uint8_t)PWOS_LINK_TYPE_DATA_MINI9P,
        0u,
        1u,
        2u,
        8u,
        0x1234u,
        0u,
        payload,
        payload_len,
        out,
        cap,
        out_len);
}

static void test_crc_known_vector(void)
{
    static const uint8_t vector[] = "123456789";

    expect_int("crc known vector",
               (int)pwos_link_crc16_ccitt_false(vector, sizeof(vector) - 1u),
               0x29B1);
}

static void test_golden_frame(void)
{
    const uint8_t payload[] = {0x01u, 0x02u, 0x4Du, 0x48u, 0x03u};
    uint8_t frame[PWOS_LINK_MAX_FRAME_LEN];
    uint8_t golden[PWOS_LINK_MAX_FRAME_LEN];
    char path[256];
    size_t frame_len = 0u;
    size_t golden_len;

    snprintf(path, sizeof(path), "%s/link_frame_v2_basic.hex", PWOS_LINK_GOLDEN_DIR);
    golden_len = load_hex_file(path, golden, sizeof(golden));

    expect_int("golden encode status",
               build_frame(payload, (uint16_t)sizeof(payload), frame, sizeof(frame), &frame_len),
               PWOS_OK);
    expect_size("golden file length", golden_len, frame_len);
    if (golden_len == frame_len) {
        expect_bytes("golden frame bytes", frame, golden, frame_len);
    }
}

static void test_decode_roundtrip(void)
{
    const uint8_t payload[] = {0x10u, 0x20u, 0x30u};
    uint8_t frame[PWOS_LINK_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    pwos_link_frame_view_t view;

    expect_int("roundtrip encode",
               build_frame(payload, (uint16_t)sizeof(payload), frame, sizeof(frame), &frame_len),
               PWOS_OK);
    expect_int("roundtrip decode",
               pwos_link_decode(frame, frame_len, &view),
               PWOS_OK);
    expect_int("roundtrip type", view.type, PWOS_LINK_TYPE_DATA_MINI9P);
    expect_int("roundtrip src", view.src, 1);
    expect_int("roundtrip dst", view.dst, 2);
    expect_int("roundtrip ttl", view.ttl, 8);
    expect_int("roundtrip seq", view.seq, 0x1234);
    expect_size("roundtrip payload len", view.payload_len, sizeof(payload));
    expect_bytes("roundtrip payload", view.payload, payload, sizeof(payload));
}

static void test_empty_and_single_byte_input(void)
{
    pwos_link_parser_t parser;
    pwos_link_parse_event_t event;
    size_t consumed = 99u;

    pwos_link_parser_init(&parser);

    expect_int("empty feed",
               pwos_link_parser_feed(&parser, NULL, 0u, &event, &consumed),
               PWOS_LINK_PARSE_NONE);
    expect_size("empty consumed", consumed, 0u);

    expect_int("single magic byte",
               pwos_link_parser_feed_byte(&parser, PWOS_LINK_MAGIC0, &event),
               PWOS_LINK_PARSE_NONE);
    expect_size("single magic parser len", parser.len, 1u);

    expect_int("noise after single magic",
               pwos_link_parser_feed_byte(&parser, 0x00u, &event),
               PWOS_LINK_PARSE_NONE);
    expect_size("single magic reset", parser.len, 0u);
}

static void test_invalid_feed_byte_reports_error(void)
{
    pwos_link_parse_event_t event;

    memset(&event, 0xA5, sizeof(event));
    expect_int("null parser feed byte",
               pwos_link_parser_feed_byte(NULL, 0x00u, &event),
               PWOS_LINK_PARSE_ERROR);
    expect_int("null parser feed byte kind", event.kind, PWOS_LINK_PARSE_ERROR);
    expect_int("null parser feed byte status", event.status, PWOS_E_NO_MEMORY);
}

static void expect_parser_frame(
    const char *label,
    const uint8_t *input,
    size_t input_len,
    const uint8_t *payload,
    size_t payload_len,
    size_t expected_consumed)
{
    pwos_link_parser_t parser;
    pwos_link_parse_event_t event;
    size_t consumed = 0u;

    pwos_link_parser_init(&parser);
    expect_int(label,
               pwos_link_parser_feed(&parser, input, input_len, &event, &consumed),
               PWOS_LINK_PARSE_FRAME);
    expect_size("parser consumed", consumed, expected_consumed);
    expect_int("parser event status", event.status, PWOS_OK);
    expect_size("parser payload len", event.frame.payload_len, payload_len);
    if (payload_len > 0u) {
        expect_bytes("parser payload", event.frame.payload, payload, payload_len);
    }
}

static void test_whole_frame_once(void)
{
    const uint8_t payload[] = {0xAAu, 0xBBu};
    uint8_t frame[PWOS_LINK_MAX_FRAME_LEN];
    size_t frame_len = 0u;

    expect_int("whole encode",
               build_frame(payload, (uint16_t)sizeof(payload), frame, sizeof(frame), &frame_len),
               PWOS_OK);
    expect_parser_frame("whole frame once", frame, frame_len, payload, sizeof(payload), frame_len);
}

static void test_two_frames_in_one_buffer(void)
{
    const uint8_t payload1[] = {0x01u};
    const uint8_t payload2[] = {0x02u, 0x03u};
    uint8_t frame1[PWOS_LINK_MAX_FRAME_LEN];
    uint8_t frame2[PWOS_LINK_MAX_FRAME_LEN];
    uint8_t both[PWOS_LINK_MAX_FRAME_LEN * 2u];
    size_t frame1_len = 0u;
    size_t frame2_len = 0u;
    size_t consumed = 0u;
    pwos_link_parser_t parser;
    pwos_link_parse_event_t event;

    expect_int("two encode 1", build_frame(payload1, sizeof(payload1), frame1, sizeof(frame1), &frame1_len), PWOS_OK);
    expect_int("two encode 2", build_frame(payload2, sizeof(payload2), frame2, sizeof(frame2), &frame2_len), PWOS_OK);
    memcpy(both, frame1, frame1_len);
    memcpy(both + frame1_len, frame2, frame2_len);

    pwos_link_parser_init(&parser);
    expect_int("two first",
               pwos_link_parser_feed(&parser, both, frame1_len + frame2_len, &event, &consumed),
               PWOS_LINK_PARSE_FRAME);
    expect_size("two first consumed", consumed, frame1_len);
    expect_bytes("two first payload", event.frame.payload, payload1, sizeof(payload1));

    expect_int("two second",
               pwos_link_parser_feed(&parser, both + consumed, frame2_len, &event, &consumed),
               PWOS_LINK_PARSE_FRAME);
    expect_size("two second consumed", consumed, frame2_len);
    expect_bytes("two second payload", event.frame.payload, payload2, sizeof(payload2));
}

static void test_noise_and_trailing_noise(void)
{
    const uint8_t payload[] = {0x7Au};
    const uint8_t prefix[] = {0x00u, 0x4Du, 0x00u, 0x55u};
    const uint8_t suffix[] = {0x99u, 0x88u, 0x77u};
    uint8_t frame[PWOS_LINK_MAX_FRAME_LEN];
    uint8_t mixed[sizeof(prefix) + PWOS_LINK_MAX_FRAME_LEN + sizeof(suffix)];
    size_t frame_len = 0u;
    size_t consumed = 0u;
    pwos_link_parser_t parser;
    pwos_link_parse_event_t event;

    expect_int("noise encode", build_frame(payload, sizeof(payload), frame, sizeof(frame), &frame_len), PWOS_OK);
    memcpy(mixed, prefix, sizeof(prefix));
    memcpy(mixed + sizeof(prefix), frame, frame_len);
    memcpy(mixed + sizeof(prefix) + frame_len, suffix, sizeof(suffix));

    pwos_link_parser_init(&parser);
    expect_int("noise plus frame",
               pwos_link_parser_feed(&parser, mixed, sizeof(prefix) + frame_len + sizeof(suffix), &event, &consumed),
               PWOS_LINK_PARSE_FRAME);
    expect_size("noise consumed", consumed, sizeof(prefix) + frame_len);
    expect_bytes("noise payload", event.frame.payload, payload, sizeof(payload));

    expect_int("trailing noise",
               pwos_link_parser_feed(&parser, mixed + consumed, sizeof(suffix), &event, &consumed),
               PWOS_LINK_PARSE_NONE);
    expect_size("trailing noise consumed", consumed, sizeof(suffix));
}

static void test_half_frame_two_feeds(void)
{
    const uint8_t payload[] = {0x01u, 0x02u, 0x03u, 0x04u};
    uint8_t frame[PWOS_LINK_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    size_t consumed = 0u;
    size_t split;
    pwos_link_parser_t parser;
    pwos_link_parse_event_t event;

    expect_int("half encode", build_frame(payload, sizeof(payload), frame, sizeof(frame), &frame_len), PWOS_OK);
    split = frame_len / 2u;

    pwos_link_parser_init(&parser);
    expect_int("half first",
               pwos_link_parser_feed(&parser, frame, split, &event, &consumed),
               PWOS_LINK_PARSE_NONE);
    expect_size("half first consumed", consumed, split);

    expect_int("half second",
               pwos_link_parser_feed(&parser, frame + split, frame_len - split, &event, &consumed),
               PWOS_LINK_PARSE_FRAME);
    expect_size("half second consumed", consumed, frame_len - split);
    expect_bytes("half payload", event.frame.payload, payload, sizeof(payload));
}

static void test_payload_zero_and_max(void)
{
    uint8_t frame[PWOS_LINK_MAX_FRAME_LEN];
    uint8_t payload[PWOS_LINK_MAX_PAYLOAD_LEN];
    size_t frame_len = 0u;
    size_t i;
    pwos_link_parser_t parser;
    pwos_link_parse_event_t event;
    size_t consumed = 0u;

    expect_int("zero payload encode", build_frame(NULL, 0u, frame, sizeof(frame), &frame_len), PWOS_OK);
    expect_parser_frame("zero payload parse", frame, frame_len, NULL, 0u, frame_len);

    for (i = 0u; i < sizeof(payload); ++i) {
        payload[i] = test_payload_byte(i);
    }

    expect_int("max payload encode",
               build_frame(payload, (uint16_t)sizeof(payload), frame, sizeof(frame), &frame_len),
               PWOS_OK);

    pwos_link_parser_init(&parser);
    expect_int("max payload parse",
               pwos_link_parser_feed(&parser, frame, frame_len, &event, &consumed),
               PWOS_LINK_PARSE_FRAME);
    expect_size("max consumed", consumed, frame_len);
    expect_size("max payload len", event.frame.payload_len, sizeof(payload));
    expect_bytes("max payload bytes", event.frame.payload, payload, sizeof(payload));
}

static void rewrite_header_crc(uint8_t *frame)
{
    uint16_t hdr_crc = pwos_link_crc16_ccitt_false(frame, PWOS_LINK_OFF_HDR_CRC);

    put_le16(frame + PWOS_LINK_OFF_HDR_CRC, hdr_crc);
}

static void test_bad_lengths_and_crc(void)
{
    const uint8_t payload[] = {0x01u, 0x02u, 0x03u, 0x04u, 0x05u};
    uint8_t frame[PWOS_LINK_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    pwos_link_parser_t parser;
    pwos_link_parse_event_t event;
    size_t consumed = 0u;

    expect_int("bad base encode", build_frame(payload, sizeof(payload), frame, sizeof(frame), &frame_len), PWOS_OK);

    frame[PWOS_LINK_OFF_HDR_CRC] ^= 0x01u;
    pwos_link_parser_init(&parser);
    expect_int("bad header crc",
               pwos_link_parser_feed(&parser, frame, PWOS_LINK_HDR_LEN, &event, &consumed),
               PWOS_LINK_PARSE_ERROR);
    expect_int("bad header crc status", event.status, PWOS_E_BAD_CRC);

    expect_int("bad body base encode", build_frame(payload, sizeof(payload), frame, sizeof(frame), &frame_len), PWOS_OK);
    frame[PWOS_LINK_OFF_PAYLOAD] ^= 0x80u;
    pwos_link_parser_init(&parser);
    expect_int("bad body crc",
               pwos_link_parser_feed(&parser, frame, frame_len, &event, &consumed),
               PWOS_LINK_PARSE_ERROR);
    expect_int("bad body crc status", event.status, PWOS_E_BAD_CRC);

    expect_int("big len base encode", build_frame(payload, sizeof(payload), frame, sizeof(frame), &frame_len), PWOS_OK);
    put_le16(frame + PWOS_LINK_OFF_PAYLOAD_LEN, (uint16_t)(PWOS_LINK_MAX_PAYLOAD_LEN + 1u));
    rewrite_header_crc(frame);
    pwos_link_parser_init(&parser);
    expect_int("payload len too big",
               pwos_link_parser_feed(&parser, frame, PWOS_LINK_HDR_LEN, &event, &consumed),
               PWOS_LINK_PARSE_ERROR);
    expect_int("payload len too big status", event.status, PWOS_E_BAD_LENGTH);

    expect_int("small len base encode", build_frame(payload, sizeof(payload), frame, sizeof(frame), &frame_len), PWOS_OK);
    put_le16(frame + PWOS_LINK_OFF_PAYLOAD_LEN, 3u);
    rewrite_header_crc(frame);
    pwos_link_parser_init(&parser);
    expect_int("payload len too small",
               pwos_link_parser_feed(&parser, frame, frame_len, &event, &consumed),
               PWOS_LINK_PARSE_ERROR);
    expect_int("payload len too small status", event.status, PWOS_E_BAD_CRC);
}

static void test_bad_header_then_good_frame_recovers(void)
{
    const uint8_t bad_payload[] = {0x11u};
    const uint8_t good_payload[] = {0x22u, 0x33u};
    uint8_t bad_frame[PWOS_LINK_MAX_FRAME_LEN];
    uint8_t good_frame[PWOS_LINK_MAX_FRAME_LEN];
    uint8_t stream[PWOS_LINK_MAX_FRAME_LEN * 2u];
    size_t bad_len = 0u;
    size_t good_len = 0u;
    size_t consumed = 0u;
    pwos_link_parser_t parser;
    pwos_link_parse_event_t event;

    expect_int("recover bad header encode",
               build_frame(bad_payload, sizeof(bad_payload), bad_frame, sizeof(bad_frame), &bad_len),
               PWOS_OK);
    expect_int("recover good encode",
               build_frame(good_payload, sizeof(good_payload), good_frame, sizeof(good_frame), &good_len),
               PWOS_OK);

    bad_frame[PWOS_LINK_OFF_HDR_CRC] ^= 0x01u;
    memcpy(stream, bad_frame, PWOS_LINK_HDR_LEN);
    memcpy(stream + PWOS_LINK_HDR_LEN, good_frame, good_len);

    pwos_link_parser_init(&parser);
    expect_int("bad header emits error",
               pwos_link_parser_feed(&parser, stream, PWOS_LINK_HDR_LEN + good_len, &event, &consumed),
               PWOS_LINK_PARSE_ERROR);
    expect_size("bad header consumed", consumed, PWOS_LINK_HDR_LEN);
    expect_int("bad header status", event.status, PWOS_E_BAD_CRC);

    expect_int("good after bad header",
               pwos_link_parser_feed(&parser, stream + consumed, good_len, &event, &consumed),
               PWOS_LINK_PARSE_FRAME);
    expect_size("good after bad header consumed", consumed, good_len);
    expect_bytes("good after bad header payload", event.frame.payload, good_payload, sizeof(good_payload));
}

static void test_bad_payload_then_good_frame_recovers(void)
{
    const uint8_t bad_payload[] = {0x44u, 0x55u};
    const uint8_t good_payload[] = {0x66u};
    uint8_t bad_frame[PWOS_LINK_MAX_FRAME_LEN];
    uint8_t good_frame[PWOS_LINK_MAX_FRAME_LEN];
    uint8_t stream[PWOS_LINK_MAX_FRAME_LEN * 2u];
    size_t bad_len = 0u;
    size_t good_len = 0u;
    size_t consumed = 0u;
    pwos_link_parser_t parser;
    pwos_link_parse_event_t event;

    expect_int("recover bad payload encode",
               build_frame(bad_payload, sizeof(bad_payload), bad_frame, sizeof(bad_frame), &bad_len),
               PWOS_OK);
    expect_int("recover good payload encode",
               build_frame(good_payload, sizeof(good_payload), good_frame, sizeof(good_frame), &good_len),
               PWOS_OK);

    bad_frame[PWOS_LINK_OFF_PAYLOAD] ^= 0x80u;
    memcpy(stream, bad_frame, bad_len);
    memcpy(stream + bad_len, good_frame, good_len);

    pwos_link_parser_init(&parser);
    expect_int("bad payload emits error",
               pwos_link_parser_feed(&parser, stream, bad_len + good_len, &event, &consumed),
               PWOS_LINK_PARSE_ERROR);
    expect_size("bad payload consumed", consumed, bad_len);
    expect_int("bad payload status", event.status, PWOS_E_BAD_CRC);

    expect_int("good after bad payload",
               pwos_link_parser_feed(&parser, stream + consumed, good_len, &event, &consumed),
               PWOS_LINK_PARSE_FRAME);
    expect_size("good after bad payload consumed", consumed, good_len);
    expect_bytes("good after bad payload bytes", event.frame.payload, good_payload, sizeof(good_payload));
}

static void test_bad_version(void)
{
    const uint8_t payload[] = {0x01u};
    uint8_t frame[PWOS_LINK_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    pwos_link_parser_t parser;
    pwos_link_parse_event_t event;
    size_t consumed = 0u;

    expect_int("bad version encode", build_frame(payload, sizeof(payload), frame, sizeof(frame), &frame_len), PWOS_OK);
    frame[PWOS_LINK_OFF_VERSION] = 3u;

    expect_int("bad version decode", pwos_link_decode(frame, frame_len, &event.frame), PWOS_E_BAD_VERSION);

    pwos_link_parser_init(&parser);
    expect_int("bad version parser",
               pwos_link_parser_feed(&parser, frame, frame_len, &event, &consumed),
               PWOS_LINK_PARSE_ERROR);
    expect_int("bad version status", event.status, PWOS_E_BAD_VERSION);
}

static void test_payload_magic_does_not_resync(void)
{
    const uint8_t payload[] = {0x01u, PWOS_LINK_MAGIC0, PWOS_LINK_MAGIC1, 0x02u};
    uint8_t frame[PWOS_LINK_MAX_FRAME_LEN];
    size_t frame_len = 0u;

    expect_int("payload magic encode", build_frame(payload, sizeof(payload), frame, sizeof(frame), &frame_len), PWOS_OK);
    expect_parser_frame("payload magic parse", frame, frame_len, payload, sizeof(payload), frame_len);
}

static void test_random_noise_1mb(void)
{
    pwos_link_parser_t parser;
    pwos_link_parse_event_t event;
    uint32_t x = 0xC001D00Du;
    size_t i;

    pwos_link_parser_init(&parser);

    for (i = 0u; i < (1024u * 1024u); ++i) {
        pwos_link_parse_kind_t kind;

        x = x * 1664525u + 1013904223u;
        kind = pwos_link_parser_feed_byte(&parser, (uint8_t)(x >> 24), &event);
        if (kind == PWOS_LINK_PARSE_FRAME) {
            if (event.frame.payload_len > PWOS_LINK_MAX_PAYLOAD_LEN) {
                failf("random frame payload len", "overflow");
            }
        } else if (kind == PWOS_LINK_PARSE_ERROR) {
            if (event.status == PWOS_OK) {
                failf("random error status", "unexpected PWOS_OK");
            }
        }
        if (parser.len > PWOS_LINK_MAX_FRAME_LEN) {
            failf("random parser len", "parser length overflow");
            break;
        }
    }
}

int main(void)
{
    test_crc_known_vector();
    test_golden_frame();
    test_decode_roundtrip();
    test_empty_and_single_byte_input();
    test_invalid_feed_byte_reports_error();
    test_whole_frame_once();
    test_two_frames_in_one_buffer();
    test_noise_and_trailing_noise();
    test_half_frame_two_feeds();
    test_payload_zero_and_max();
    test_bad_lengths_and_crc();
    test_bad_header_then_good_frame_recovers();
    test_bad_payload_then_good_frame_recovers();
    test_bad_version();
    test_payload_magic_does_not_resync();
    test_random_noise_1mb();

    if (g_failures != 0) {
        printf("pwos link tests failed: %d\n", g_failures);
        return 1;
    }

    printf("pwos link tests passed\n");
    return 0;
}
