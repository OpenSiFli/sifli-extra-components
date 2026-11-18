/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Internal link channel interface, implemented by xz_ws.c or xz_mqtt.c
 * (only one of them is compiled, chosen by Kconfig).
 */

#ifndef __XZ_CHANNEL_H__
#define __XZ_CHANNEL_H__

#include <stdint.h>
#include <stddef.h>

#include "xiaozhi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    XZ_CH_EVT_CONNECTED = 0,
    XZ_CH_EVT_DISCONNECTED,
    XZ_CH_EVT_TEXT,
    XZ_CH_EVT_BIN,
} xz_ch_evt_t;

typedef void (*xz_ch_cb_t)(xz_ch_evt_t evt, const uint8_t *data, size_t len, void *user);

typedef struct
{
    const char *url;            /* websocket mode: wss://host/path */
    const char *token;          /* websocket mode: bearer token */

    const char *endpoint;       /* mqtt mode: host name */
    const char *mqtt_client_id;
    const char *mqtt_username;
    const char *mqtt_password;
    const char *mqtt_topic;

    const char *device_id;
    const char *client_id;
} xz_ch_cfg_t;

int  xz_ch_open(const xz_ch_cfg_t *cfg, xz_ch_cb_t cb, void *user);
int  xz_ch_send_text(const void *data, size_t len);
int  xz_ch_send_bin(const void *data, size_t len);
void xz_ch_close(void);

#ifdef __cplusplus
}
#endif

#endif /* __XZ_CHANNEL_H__ */
