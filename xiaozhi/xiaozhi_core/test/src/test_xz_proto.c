/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * xz_proto: encoding and decoding of the xiaozhi protocol.
 *
 * Everything here is a pure function - no network, no hardware - so the tests
 * pin down the component contract: frame shape, escaping, boundary truncation,
 * and graceful handling of malformed input.
 */

#include <string.h>
#include <stdio.h>

#include "unity.h"
#include "cJSON.h"

#include "xz_proto.h"

/* xz_msg_t embeds a 1.5KB MCP payload, so keep it in .bss instead of on the main thread stack */
static xz_msg_t s_msg;
static char s_buf[XZ_TEXT_MAX * 2 + 320];
static char s_long[XZ_TEXT_MAX + 96];

/* ---------------- hello ---------------- */

/* The uplink parameters must match the negotiated values, otherwise the server frames packets at the wrong size */
void test_xz_proto_hello_fields(void)
{
    cJSON *root;
    cJSON *params;
    int n;

    n = xz_proto_build_hello(s_buf, sizeof(s_buf), "websocket");
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_INT(n, (int)strlen(s_buf));

    root = cJSON_Parse(s_buf);
    TEST_ASSERT_NOT_NULL(root);

    TEST_ASSERT_EQUAL_STRING("hello",
                             cJSON_GetStringValue(cJSON_GetObjectItem(root, "type")));
    TEST_ASSERT_EQUAL_INT(3, (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "version")));
    TEST_ASSERT_EQUAL_STRING("websocket",
                             cJSON_GetStringValue(cJSON_GetObjectItem(root, "transport")));

    /* audio_params is mandatory for the server; even a text-only build has to report it */
    params = cJSON_GetObjectItem(root, "audio_params");
    TEST_ASSERT_NOT_NULL(params);
    TEST_ASSERT_EQUAL_STRING("opus",
                             cJSON_GetStringValue(cJSON_GetObjectItem(params, "format")));
    TEST_ASSERT_EQUAL_INT(XZ_UPLINK_SAMPLE_RATE,
                          (int)cJSON_GetNumberValue(cJSON_GetObjectItem(params, "sample_rate")));
    TEST_ASSERT_EQUAL_INT(1,
                          (int)cJSON_GetNumberValue(cJSON_GetObjectItem(params, "channels")));
    TEST_ASSERT_EQUAL_INT(XZ_UPLINK_FRAME_MS,
                          (int)cJSON_GetNumberValue(cJSON_GetObjectItem(params, "frame_duration")));

#if XIAOZHI_FEATURE_MCP
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(root, "features"));
#endif

    cJSON_Delete(root);
}

/* The transport name comes from the build-time selection; NULL falls back to "websocket" */
void test_xz_proto_hello_default_transport(void)
{
    TEST_ASSERT_TRUE(xz_proto_build_hello(s_buf, sizeof(s_buf), NULL) > 0);
    TEST_ASSERT_NOT_NULL(strstr(s_buf, "\"transport\":\"websocket\""));
}

/* ---------------- JSON escaping ---------------- */

/* Every character class that needs escaping must become the form JSON understands */
void test_xz_proto_escape_specials(void)
{
    const char *in = "q\"b\\s\nn\tt\x01";
    const char *want = "q\\\"b\\\\s\\nn\\tt\\u0001";
    int n;

    n = xz_json_escape(in, s_buf, sizeof(s_buf));
    TEST_ASSERT_EQUAL_INT((int)strlen(want), n);
    TEST_ASSERT_EQUAL_STRING(want, s_buf);
}

/* Must fail when the buffer is too small - never return a truncated string as if it were valid */
void test_xz_proto_escape_overflow(void)
{
    char small[4];

    TEST_ASSERT_TRUE(xz_json_escape("abcd", small, sizeof(small)) < 0);
    TEST_ASSERT_TRUE(xz_json_escape("\"", small, 2) < 0);
    /* Only an exact fit (2 characters plus the terminating NUL) counts as success */
    TEST_ASSERT_EQUAL_INT(2, xz_json_escape("ab", small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("ab", small);
}

/*
 * Escaping and then parsing back must yield the original text.
 * This is stronger than "does the output contain a backslash": it checks the
 * escaping and the JSON validity at the same time.
 */
void test_xz_proto_detect_roundtrip(void)
{
    const char *text = "say \"hi\"\\o/\nline2\ttab";
    cJSON *root;
    int n;

    n = xz_proto_build_detect(s_buf, sizeof(s_buf), "sid-1", text);
    TEST_ASSERT_TRUE(n > 0);

    root = cJSON_Parse(s_buf);
    TEST_ASSERT_NOT_NULL(root);

    TEST_ASSERT_EQUAL_STRING("listen",
                             cJSON_GetStringValue(cJSON_GetObjectItem(root, "type")));
    TEST_ASSERT_EQUAL_STRING("sid-1",
                             cJSON_GetStringValue(cJSON_GetObjectItem(root, "session_id")));
    TEST_ASSERT_EQUAL_STRING(text,
                             cJSON_GetStringValue(cJSON_GetObjectItem(root, "text")));

    cJSON_Delete(root);
}

/* ---------------- decoding ---------------- */

/* Every field of a real hello must land in the struct - none may be dropped */
void test_xz_proto_parse_hello(void)
{
    static const char *const hello =
        "{"
        "\"type\":\"hello\","
        "\"session_id\":\"abc123\","
        "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":24000,"
        "\"channels\":1,\"frame_duration\":60}"
        "}";

    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_proto_parse(hello, &s_msg));
    TEST_ASSERT_EQUAL_INT(XZ_MSG_HELLO, s_msg.type);
    TEST_ASSERT_EQUAL_STRING("abc123", s_msg.session_id);
    TEST_ASSERT_EQUAL_INT(24000, (int)s_msg.sample_rate);
    TEST_ASSERT_EQUAL_INT(60, (int)s_msg.frame_duration);
}

/* Older servers send duration instead of frame_duration; both must be accepted */
void test_xz_proto_parse_hello_duration_alias(void)
{
    static const char *const hello =
        "{\"type\":\"hello\",\"session_id\":\"s2\","
        "\"audio_params\":{\"duration\":\"60\"}}";

    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_proto_parse(hello, &s_msg));
    TEST_ASSERT_EQUAL_INT(XZ_MSG_HELLO, s_msg.type);
    TEST_ASSERT_EQUAL_INT(60, (int)s_msg.frame_duration);
    /* The sample rate stays 0 when absent, letting the upper layer fall back to its default */
    TEST_ASSERT_EQUAL_INT(0, (int)s_msg.sample_rate);
}

/* All three tts states must be recognised */
void test_xz_proto_parse_tts_states(void)
{
    static const char *const start =
        "{\"type\":\"tts\",\"state\":\"start\",\"text\":\"hi\"}";
    static const char *const sentence =
        "{\"type\":\"tts\",\"state\":\"sentence_start\",\"text\":\"你好\"}";
    static const char *const stop = "{\"type\":\"tts\",\"state\":\"stop\"}";

    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_proto_parse(start, &s_msg));
    TEST_ASSERT_EQUAL_INT(XZ_MSG_TTS, s_msg.type);
    TEST_ASSERT_EQUAL_STRING("start", s_msg.state);

    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_proto_parse(sentence, &s_msg));
    TEST_ASSERT_EQUAL_STRING("sentence_start", s_msg.state);
    TEST_ASSERT_EQUAL_STRING("你好", s_msg.text);

    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_proto_parse(stop, &s_msg));
    TEST_ASSERT_EQUAL_STRING("stop", s_msg.state);
    TEST_ASSERT_EQUAL_STRING("", s_msg.text);
}

/* An unknown type is normal (the server keeps adding messages) and must not be an error */
void test_xz_proto_parse_unknown_type(void)
{
    TEST_ASSERT_EQUAL_INT(XZ_OK,
                          xz_proto_parse("{\"type\":\"brand_new_message\"}", &s_msg));
    TEST_ASSERT_EQUAL_INT(XZ_MSG_UNKNOWN, s_msg.type);
}

/* Malformed input must return an error code, never crash or yield a half-filled message */
void test_xz_proto_parse_malformed(void)
{
    TEST_ASSERT_EQUAL_INT(XZ_ERR_PROTOCOL, xz_proto_parse("not json at all", &s_msg));
    TEST_ASSERT_EQUAL_INT(XZ_ERR_PROTOCOL, xz_proto_parse("", &s_msg));
    TEST_ASSERT_EQUAL_INT(XZ_ERR_PROTOCOL, xz_proto_parse("{", &s_msg));
    TEST_ASSERT_EQUAL_INT(XZ_ERR_PROTOCOL, xz_proto_parse("{\"type\":}", &s_msg));
    /* Valid JSON but no type field */
    TEST_ASSERT_EQUAL_INT(XZ_ERR_PROTOCOL, xz_proto_parse("{}", &s_msg));
    TEST_ASSERT_EQUAL_INT(XZ_ERR_PROTOCOL, xz_proto_parse("[1,2,3]", &s_msg));
}

/* Over-long text is truncated but must stay NUL-terminated and must not be over-read */
void test_xz_proto_parse_text_truncated(void)
{
    size_t i;

    for (i = 0; i < sizeof(s_long) - 1; i++)
        s_long[i] = 'a';
    s_long[sizeof(s_long) - 1] = '\0';

    TEST_ASSERT_TRUE(rt_snprintf(s_buf, sizeof(s_buf),
                                 "{\"type\":\"stt\",\"text\":\"%s\"}", s_long) > 0);

    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_proto_parse(s_buf, &s_msg));
    TEST_ASSERT_EQUAL_INT(XZ_MSG_STT, s_msg.type);
    TEST_ASSERT_EQUAL_INT(XZ_TEXT_MAX - 1, (int)strlen(s_msg.text));
}

/* ---------------- encoding boundaries ---------------- */

/* Must return an error when the buffer is too small, not a length that only looks successful */
void test_xz_proto_build_overflow(void)
{
    char tiny[8];

    TEST_ASSERT_TRUE(xz_proto_build_listen_stop(tiny, sizeof(tiny), "sid") < 0);
    TEST_ASSERT_TRUE(xz_proto_build_listen_start(tiny, sizeof(tiny), "sid",
                                                 XZ_MODE_AUTO) < 0);
    TEST_ASSERT_TRUE(xz_proto_build_abort(tiny, sizeof(tiny), "sid", 0) < 0);
}

/* An invalid mode value must collapse to auto instead of indexing out of bounds */
void test_xz_proto_listen_mode_clamp(void)
{
    TEST_ASSERT_TRUE(xz_proto_build_listen_start(s_buf, sizeof(s_buf), "sid", 99) > 0);
    TEST_ASSERT_NOT_NULL(strstr(s_buf, "\"mode\":\"auto\""));

    TEST_ASSERT_TRUE(xz_proto_build_listen_start(s_buf, sizeof(s_buf), "sid", -5) > 0);
    TEST_ASSERT_NOT_NULL(strstr(s_buf, "\"mode\":\"auto\""));

    TEST_ASSERT_TRUE(xz_proto_build_listen_start(s_buf, sizeof(s_buf), "sid",
                                                 XZ_MODE_REALTIME) > 0);
    TEST_ASSERT_NOT_NULL(strstr(s_buf, "\"mode\":\"realtime\""));
}

/* Only wake-word aborts carry a reason; a user-initiated abort does not */
void test_xz_proto_abort_reason(void)
{
    TEST_ASSERT_TRUE(xz_proto_build_abort(s_buf, sizeof(s_buf), "sid", 1) > 0);
    TEST_ASSERT_NOT_NULL(strstr(s_buf, "\"reason\":\"wake_word_detected\""));

    TEST_ASSERT_TRUE(xz_proto_build_abort(s_buf, sizeof(s_buf), "sid", 0) > 0);
    TEST_ASSERT_NULL(strstr(s_buf, "\"reason\""));
    TEST_ASSERT_NOT_NULL(strstr(s_buf, "\"type\":\"abort\""));
}
