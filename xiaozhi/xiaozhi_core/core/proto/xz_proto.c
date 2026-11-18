/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <rtthread.h>
#include <cJSON.h>

#include "xz_proto.h"

#define XZ_STR_(x)  #x
#define XZ_STR(x)   XZ_STR_(x)

#ifndef XZ_TEXT_INPUT_STATE
#define XZ_TEXT_INPUT_STATE "detect"
#endif

static const char *const s_mode_str[] = {"auto", "manual", "realtime"};

static const char *jstr(cJSON *obj, const char *key)
{
    cJSON *item;

    if (!obj)
        return NULL;
    item = cJSON_GetObjectItem(obj, key);
    if (!item || !cJSON_IsString(item))
        return NULL;
    return item->valuestring;
}

static int jint(cJSON *obj, const char *key, int def)
{
    cJSON *item;

    if (!obj)
        return def;
    item = cJSON_GetObjectItem(obj, key);
    if (!item)
        return def;
    if (cJSON_IsNumber(item))
        return item->valueint;
    if (cJSON_IsString(item) && item->valuestring)
        return atoi(item->valuestring);
    return def;
}

static void jcopy(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0)
        return;
    if (!src)
    {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

int xz_json_escape(const char *in, char *out, size_t out_len)
{
    size_t j = 0;

    if (!out || out_len == 0)
        return XZ_ERR_PARAM;
    if (!in)
        in = "";

    for (; *in; in++)
    {
        unsigned char c = (unsigned char)*in;
        const char *rep = NULL;
        char tmp[8];

        switch (c)
        {
        case '"':  rep = "\\\""; break;
        case '\\': rep = "\\\\"; break;
        case '\n': rep = "\\n";  break;
        case '\r': rep = "\\r";  break;
        case '\t': rep = "\\t";  break;
        default:
            if (c < 0x20)
            {
                snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                rep = tmp;
            }
            break;
        }

        if (rep)
        {
            size_t n = strlen(rep);
            if (j + n >= out_len)
                return XZ_ERR_BUF;
            memcpy(out + j, rep, n);
            j += n;
        }
        else
        {
            if (j + 1 >= out_len)
                return XZ_ERR_BUF;
            out[j++] = (char)c;
        }
    }

    out[j] = '\0';
    return (int)j;
}

int xz_proto_parse(const char *json, xz_msg_t *msg)
{
    cJSON *root;
    const char *type;

    if (!json || !msg)
        return XZ_ERR_PARAM;

    memset(msg, 0, sizeof(*msg));

    root = cJSON_Parse(json);
    if (!root)
        return XZ_ERR_PROTOCOL;

    type = jstr(root, "type");
    if (!type)
    {
        cJSON_Delete(root);
        return XZ_ERR_PROTOCOL;
    }

    if (strcmp(type, "hello") == 0)
    {
        cJSON *ap;

        msg->type = XZ_MSG_HELLO;
        jcopy(msg->session_id, sizeof(msg->session_id), jstr(root, "session_id"));

        ap = cJSON_GetObjectItem(root, "audio_params");
        msg->sample_rate = (uint32_t)jint(ap, "sample_rate", 0);
        msg->frame_duration = (uint32_t)jint(ap, "frame_duration", 0);
        if (msg->frame_duration == 0)
            msg->frame_duration = (uint32_t)jint(ap, "duration", 0);
    }
    else if (strcmp(type, "goodbye") == 0)
    {
        msg->type = XZ_MSG_GOODBYE;
    }
    else if (strcmp(type, "stt") == 0)
    {
        msg->type = XZ_MSG_STT;
        jcopy(msg->text, sizeof(msg->text), jstr(root, "text"));
    }
    else if (strcmp(type, "tts") == 0)
    {
        const char *state;

        msg->type = XZ_MSG_TTS;
        state = jstr(root, "state");
        jcopy(msg->state, sizeof(msg->state), state);
        jcopy(msg->text, sizeof(msg->text), jstr(root, "text"));
    }
    else if (strcmp(type, "llm") == 0)
    {
        msg->type = XZ_MSG_LLM;
        jcopy(msg->emotion, sizeof(msg->emotion), jstr(root, "emotion"));
        jcopy(msg->text, sizeof(msg->text), jstr(root, "text"));
    }
#if XIAOZHI_FEATURE_MCP
    else if (strcmp(type, "mcp") == 0)
    {
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        char *printed = NULL;

        msg->type = XZ_MSG_MCP;
        if (payload)
            printed = cJSON_PrintUnformatted(payload);
        if (printed)
        {
            jcopy(msg->payload, sizeof(msg->payload), printed);
            cJSON_free(printed);
        }
    }
#endif
    else
    {
        msg->type = XZ_MSG_UNKNOWN;
    }

    cJSON_Delete(root);
    return XZ_OK;
}

int xz_proto_build_hello(char *buf, size_t len, const char *transport)
{
    int n;

    if (!buf || len == 0)
        return XZ_ERR_PARAM;
    if (!transport)
        transport = "websocket";

    n = rt_snprintf(buf, len,
                    "{"
                    "\"type\":\"hello\","
                    "\"version\":3,"
#if XIAOZHI_FEATURE_MCP
                    "\"features\":{\"mcp\":true},"
#endif
                    "\"transport\":\"%s\","
                    "\"audio_params\":{"
                    "\"format\":\"opus\","
                    "\"sample_rate\":" XZ_STR(XZ_UPLINK_SAMPLE_RATE) ","
                    "\"channels\":1,"
                    "\"frame_duration\":" XZ_STR(XZ_UPLINK_FRAME_MS)
                    "}"
                    "}",
                    transport);
    if (n < 0 || (size_t)n >= len)
        return XZ_ERR_BUF;
    return n;
}

int xz_proto_build_listen_start(char *buf, size_t len, const char *sid, int mode)
{
    int n;

    if (!buf || len == 0 || !sid)
        return XZ_ERR_PARAM;
    if (mode < 0 || mode > 2)
        mode = 0;

    n = rt_snprintf(buf, len,
                    "{\"session_id\":\"%s\",\"type\":\"listen\","
                    "\"state\":\"start\",\"mode\":\"%s\"}",
                    sid, s_mode_str[mode]);
    if (n < 0 || (size_t)n >= len)
        return XZ_ERR_BUF;
    return n;
}

int xz_proto_build_listen_stop(char *buf, size_t len, const char *sid)
{
    int n;

    if (!buf || len == 0 || !sid)
        return XZ_ERR_PARAM;

    n = rt_snprintf(buf, len,
                    "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}",
                    sid);
    if (n < 0 || (size_t)n >= len)
        return XZ_ERR_BUF;
    return n;
}

int xz_proto_build_abort(char *buf, size_t len, const char *sid, int wake_word)
{
    int n;

    if (!buf || len == 0 || !sid)
        return XZ_ERR_PARAM;

    n = rt_snprintf(buf, len,
                    "{\"session_id\":\"%s\",\"type\":\"abort\"%s}",
                    sid,
                    wake_word ? ",\"reason\":\"wake_word_detected\"" : "");
    if (n < 0 || (size_t)n >= len)
        return XZ_ERR_BUF;
    return n;
}

int xz_proto_build_detect(char *buf, size_t len, const char *sid, const char *text)
{
    char escaped[XZ_TEXT_MAX * 2];
    int n;

    if (!buf || len == 0 || !sid || !text)
        return XZ_ERR_PARAM;
    if (xz_json_escape(text, escaped, sizeof(escaped)) < 0)
        return XZ_ERR_BUF;

    n = rt_snprintf(buf, len,
                    "{\"session_id\":\"%s\",\"type\":\"listen\","
                    "\"state\":\"" XZ_TEXT_INPUT_STATE "\",\"text\":\"%s\"}",
                    sid, escaped);
    if (n < 0 || (size_t)n >= len)
        return XZ_ERR_BUF;
    return n;
}

#if XIAOZHI_FEATURE_MCP
int xz_proto_build_mcp(char *buf, size_t len, const char *sid, const char *payload)
{
    int n;

    if (!buf || len == 0 || !sid || !payload)
        return XZ_ERR_PARAM;

    n = rt_snprintf(buf, len,
                    "{\"session_id\":\"%s\",\"type\":\"mcp\",\"payload\":%s}",
                    sid, payload);
    if (n < 0 || (size_t)n >= len)
        return XZ_ERR_BUF;
    return n;
}
#endif /* XIAOZHI_FEATURE_MCP */
