/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __XZ_HTTP_H__
#define __XZ_HTTP_H__

#include <stdint.h>
#include <stddef.h>
#include "xiaozhi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    char ws_url[256];
    char ws_token[256];

    char mqtt_endpoint[128];
    char mqtt_client_id[64];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic[128];

    char activation_code[16];
    char activation_message[128];
    int activation_required;
    int has_websocket;
    int has_mqtt;
} xz_bootstrap_t;

int xz_http_bootstrap(const xz_config_t *cfg, xz_bootstrap_t *out);

#ifdef __cplusplus
}
#endif

#endif /* __XZ_HTTP_H__ */
