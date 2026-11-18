/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Xiaozhi protocol encoding and decoding.
 */

#ifndef __XZ_PROTO_H__
#define __XZ_PROTO_H__

#include <stdint.h>
#include <stddef.h>

#include "xiaozhi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XZ_SESSION_ID_MAX   64
#define XZ_TEXT_MAX         512

#if XIAOZHI_FEATURE_MCP
    #define XZ_PAYLOAD_MAX      1536
#endif /* XIAOZHI_FEATURE_MCP */

typedef enum
{
    XZ_MSG_UNKNOWN = 0,
    XZ_MSG_HELLO,
    XZ_MSG_GOODBYE,
    XZ_MSG_STT,
    XZ_MSG_TTS,
    XZ_MSG_LLM,
#if XIAOZHI_FEATURE_MCP
    XZ_MSG_MCP,
#endif /* XIAOZHI_FEATURE_MCP */
} xz_msg_type_t;

typedef enum
{
    XZ_TTS_STATE_NONE = 0,
    XZ_TTS_STATE_START,
    XZ_TTS_STATE_SENTENCE,
    XZ_TTS_STATE_STOP,
} xz_tts_state_t;

typedef struct
{
    xz_msg_type_t type;
    char session_id[XZ_SESSION_ID_MAX];
    char text[XZ_TEXT_MAX];
    char state[32];
    char emotion[32];
    uint32_t sample_rate;
    uint32_t frame_duration;
#if XIAOZHI_FEATURE_MCP
    char payload[XZ_PAYLOAD_MAX];
#endif /* XIAOZHI_FEATURE_MCP */
} xz_msg_t;

int xz_proto_parse(const char *json, xz_msg_t *msg);

int xz_proto_build_hello(char *buf, size_t len, const char *transport);
int xz_proto_build_listen_start(char *buf, size_t len, const char *sid, int mode);
int xz_proto_build_listen_stop(char *buf, size_t len, const char *sid);
int xz_proto_build_abort(char *buf, size_t len, const char *sid, int wake_word);
int xz_proto_build_detect(char *buf, size_t len, const char *sid, const char *text);
#if XIAOZHI_FEATURE_MCP
int xz_proto_build_mcp(char *buf, size_t len, const char *sid, const char *payload);
#endif /* XIAOZHI_FEATURE_MCP */

int xz_json_escape(const char *in, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* __XZ_PROTO_H__ */
