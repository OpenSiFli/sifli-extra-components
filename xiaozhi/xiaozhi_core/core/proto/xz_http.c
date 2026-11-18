/*
 *
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include <rtthread.h>
#include <webclient.h>
#include <cJSON.h>

#include "xz_http.h"

#define LOG_TAG "xz_http"
#include "log.h"

#define XZ_BOOTSTRAP_BODY_MAX   2048
#define XZ_BOOTSTRAP_RESP_MAX   4096


static void copy_str(char *dst, size_t dst_len, const char *src)
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

static const char *get_str(cJSON *obj, const char *key)
{
    cJSON *item;

    if (!obj)
        return NULL;
    item = cJSON_GetObjectItem(obj, key);
    if (!item || !cJSON_IsString(item))
        return NULL;
    return item->valuestring;
}

static int build_url(char *buf, size_t len, const char *server_url)
{
    size_t n;
    int ret;

    if (!buf || len == 0)
        return XZ_ERR_PARAM;
    if (!server_url || !server_url[0])
        server_url = "https://api.tenclass.net";

    n = strlen(server_url);
    if (n && server_url[n - 1] == '/')
        ret = rt_snprintf(buf, len, "%sxiaozhi/ota/", server_url);
    else
        ret = rt_snprintf(buf, len, "%s/xiaozhi/ota/", server_url);

    if (ret < 0 || (size_t)ret >= len)
        return XZ_ERR_PARAM;
    return XZ_OK;
}

/*
 * HTTP bootstrap: fetch the link parameters from /xiaozhi/ota/.
 *
 * POSTs the device information, then parses websocket.url/token or the
 * mqtt.* credentials, plus the activation code and message.
 */

int xz_http_bootstrap(const xz_config_t *cfg, xz_bootstrap_t *out)
{
    struct webclient_session *session = NULL;
    char url[320];
    char body[XZ_BOOTSTRAP_BODY_MAX];
    char *resp = NULL;
    int body_len;
    int status;
    int total = 0;
    int ret = XZ_ERR_NET;
    cJSON *root = NULL;

    if (!cfg || !out)
        return XZ_ERR_PARAM;

    memset(out, 0, sizeof(*out));

    if (build_url(url, sizeof(url), cfg->server_url) != XZ_OK)
        return XZ_ERR_PARAM;

    body_len = rt_snprintf(body, sizeof(body),
                           "{"
                           "\"version\":2,"
                           "\"flash_size\":4194304,"
                           "\"psram_size\":0,"
                           "\"minimum_free_heap_size\":123456,"
                           "\"mac_address\":\"%s\","
                           "\"uuid\":\"%s\","
                           "\"chip_model_name\":\"%s\","
                           "\"chip_info\":{"
                           "\"model\":1,\"cores\":2,\"revision\":0,\"features\":0"
                           "},"
                           "\"application\":{"
                           "\"name\":\"xiaozhi-core\","
                           "\"version\":\"%s\","
                           "\"compile_time\":\"2026-01-01T00:00:00Z\","
                           "\"idf_version\":\"4.2-dev\","
                           "\"elf_sha256\":\"\""
                           "},"
                           "\"partition_table\":["
                           "{\"label\":\"app\",\"type\":1,\"subtype\":2,"
                           "\"address\":65536,\"size\":1048576}"
                           "],"
                           "\"ota\":{\"label\":\"ota_0\"},"
                           "\"board\":{"
                           "\"type\":\"%s\","
                           "\"mac\":\"%s\""
                           "}"
                           "}",
                           cfg->device_id ? cfg->device_id : "",
                           cfg->client_id ? cfg->client_id : "",
                           cfg->model ? cfg->model : "sf32lb52",
                           cfg->firmware_version ? cfg->firmware_version : XZ_VERSION_STR,
                           cfg->model ? cfg->model : "sf32lb52",
                           cfg->device_id ? cfg->device_id : "");
    if (body_len < 0 || (size_t)body_len >= sizeof(body))
        return XZ_ERR_PARAM;

    resp = rt_calloc(1, XZ_BOOTSTRAP_RESP_MAX);
    if (!resp)
        return XZ_ERR_NO_MEM;

    session = webclient_session_create(4096);
    if (!session)
    {
        rt_free(resp);
        return XZ_ERR_NO_MEM;
    }

    webclient_set_timeout(session, cfg->timeout_bootstrap_ms ?
                          (int)cfg->timeout_bootstrap_ms : 10000);

    webclient_header_fields_add(session, "Device-Id: %s\r\n",
                                cfg->device_id ? cfg->device_id : "");
    webclient_header_fields_add(session, "Client-Id: %s\r\n",
                                cfg->client_id ? cfg->client_id : "");
    webclient_header_fields_add(session, "Content-Type: application/json\r\n");
    webclient_header_fields_add(session, "Content-Length: %d\r\n", body_len);

    status = webclient_post(session, url, body, (size_t)body_len);
    if (status != 200)
    {
        LOG_E("http status %d", status);
        goto out;
    }

    while (total < XZ_BOOTSTRAP_RESP_MAX - 1)
    {
        int n = webclient_read(session, resp + total,
                               XZ_BOOTSTRAP_RESP_MAX - 1 - total);
        if (n <= 0)
            break;
        total += n;
    }
    resp[total] = '\0';

    if (total == 0)
    {
        LOG_E("empty response");
        goto out;
    }

    root = cJSON_Parse(resp);
    if (!root)
    {
        LOG_E("JSON parse failed");
        ret = XZ_ERR_PROTOCOL;
        goto out;
    }

    {
        cJSON *ws = cJSON_GetObjectItem(root, "websocket");
        cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
        cJSON *act = cJSON_GetObjectItem(root, "activation");

        if (ws)
        {
            copy_str(out->ws_url, sizeof(out->ws_url), get_str(ws, "url"));
            copy_str(out->ws_token, sizeof(out->ws_token), get_str(ws, "token"));
            out->has_websocket = out->ws_url[0] != '\0';
        }

        if (mqtt)
        {
            copy_str(out->mqtt_endpoint, sizeof(out->mqtt_endpoint),
                     get_str(mqtt, "endpoint"));
            copy_str(out->mqtt_client_id, sizeof(out->mqtt_client_id),
                     get_str(mqtt, "client_id"));
            copy_str(out->mqtt_username, sizeof(out->mqtt_username),
                     get_str(mqtt, "username"));
            copy_str(out->mqtt_password, sizeof(out->mqtt_password),
                     get_str(mqtt, "password"));
            copy_str(out->mqtt_topic, sizeof(out->mqtt_topic),
                     get_str(mqtt, "publish_topic"));
            out->has_mqtt = out->mqtt_endpoint[0] != '\0';
        }

        if (act)
        {
            copy_str(out->activation_code, sizeof(out->activation_code),
                     get_str(act, "code"));
            copy_str(out->activation_message, sizeof(out->activation_message),
                     get_str(act, "message"));
            out->activation_required = out->activation_code[0] != '\0';
        }
    }

    if (!out->has_websocket && !out->has_mqtt)
    {
        LOG_E("response has no transport info");
        ret = XZ_ERR_PROTOCOL;
        goto out;
    }

    ret = XZ_OK;

out:
    if (root)
        cJSON_Delete(root);
    if (session)
        webclient_close(session);
    if (resp)
        rt_free(resp);
    return ret;
}
