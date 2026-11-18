/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <rtthread.h>
#include <lwip/opt.h>
#include <lwip/err.h>
#include <lwip/ip_addr.h>
#include <lwip/apps/websocket_client.h>

#include "xz_channel.h"

#define LOG_TAG "xz_ws"
#include "log.h"

#ifndef XZ_WS_CONNECT_TIMEOUT_MS
#define XZ_WS_CONNECT_TIMEOUT_MS 15000
#endif

static wsock_state_t s_ws_state;

static struct
{
    int inited;
    int connected;
    xz_ch_cb_t cb;
    void *user;
    rt_sem_t sem;
    rt_mutex_t lock;
} s_ws;

static int parse_ws_url(const char *url, char *host, size_t host_len,
                        char *path, size_t path_len, uint16_t *port, int *ssl)
{
    const char *p;
    const char *slash;
    size_t n;

    if (!url || !host || !path || !port || !ssl)
        return XZ_ERR_PARAM;

    if (strncmp(url, "wss://", 6) == 0)
    {
        *ssl = 1;
        *port = 443;
        p = url + 6;
    }
    else if (strncmp(url, "ws://", 5) == 0)
    {
        *ssl = 0;
        *port = 80;
        p = url + 5;
    }
    else
    {
        return XZ_ERR_PARAM;
    }

    slash = strchr(p, '/');
    n = slash ? (size_t)(slash - p) : strlen(p);
    if (n == 0 || n >= host_len)
        return XZ_ERR_PARAM;

    memcpy(host, p, n);
    host[n] = '\0';

    /* optional :port */
    {
        char *colon = strrchr(host, ':');

        if (colon && colon[1])
        {
            *port = (uint16_t)atoi(colon + 1);
            *colon = '\0';
        }
    }

    if (slash)
    {
        if (strlen(slash) >= path_len)
            return XZ_ERR_PARAM;
        strcpy(path, slash);
    }
    else
    {
        strcpy(path, "/");
    }

    return XZ_OK;
}

static err_t ws_message_cb(int code, char *buf, size_t len)
{
    switch (code)
    {
    case WS_CONNECT:
    {
        int status = (int)(uintptr_t)buf;

        if (status == 101)
        {
            s_ws.connected = 1;
            if (s_ws.sem)
                rt_sem_release(s_ws.sem);
            if (s_ws.cb)
                s_ws.cb(XZ_CH_EVT_CONNECTED, NULL, 0, s_ws.user);
        }
        else
        {
            s_ws.connected = 0;
            if (s_ws.sem)
                rt_sem_release(s_ws.sem);
        }
        break;
    }
    case WS_DISCONNECT:
        s_ws.connected = 0;
        if (s_ws.sem)
            rt_sem_release(s_ws.sem);
        if (s_ws.cb)
            s_ws.cb(XZ_CH_EVT_DISCONNECTED, NULL, 0, s_ws.user);
        break;
    case WS_TEXT:
        if (s_ws.cb)
            s_ws.cb(XZ_CH_EVT_TEXT, (const uint8_t *)buf, len, s_ws.user);
        break;
    case WS_DATA:
        if (s_ws.cb)
            s_ws.cb(XZ_CH_EVT_BIN, (const uint8_t *)buf, len, s_ws.user);
        break;
    default:
        break;
    }

    return ERR_OK;
}

int xz_ch_open(const xz_ch_cfg_t *cfg, xz_ch_cb_t cb, void *user)
{
    char host[96];
    char path[160];
    uint16_t port;
    int ssl = 1;
    err_t err;
    int ret = XZ_ERR_NET;

    if (!cfg || !cfg->url)
        return XZ_ERR_PARAM;

    if (parse_ws_url(cfg->url, host, sizeof(host), path, sizeof(path),
                     &port, &ssl) != XZ_OK)
    {
        LOG_E("invalid websocket url: %s", cfg->url);
        return XZ_ERR_PARAM;
    }

    if (!s_ws.sem)
        s_ws.sem = rt_sem_create("xz_ws", 0, RT_IPC_FLAG_FIFO);
    if (!s_ws.lock)
        s_ws.lock = rt_mutex_create("xz_ws", RT_IPC_FLAG_FIFO);
    if (!s_ws.sem || !s_ws.lock)
        return XZ_ERR_NO_MEM;

    s_ws.cb = cb;
    s_ws.user = user;
    s_ws.connected = 0;
    rt_sem_control(s_ws.sem, RT_IPC_CMD_RESET, RT_NULL);

    memset(&s_ws_state, 0, sizeof(s_ws_state));
    err = wsock_init(&s_ws_state, ssl, 1, ws_message_cb);
    if (err != ERR_OK)
    {
        LOG_E("wsock_init failed: %d", err);
        return XZ_ERR_NET;
    }
    s_ws.inited = 1;

    err = wsock_connect(&s_ws_state, 4096, host, path, port,
                        cfg->token, NULL,
                        "Protocol-Version: 1\r\n"
                        "Device-Id: %s\r\n"
                        "Client-Id: %s\r\n",
                        cfg->device_id ? cfg->device_id : "",
                        cfg->client_id ? cfg->client_id : "");
    if (err != ERR_OK)
    {
        LOG_E("wsock_connect failed: %d", err);
        xz_ch_close();
        return XZ_ERR_NET;
    }

    if (rt_sem_take(s_ws.sem, rt_tick_from_millisecond(XZ_WS_CONNECT_TIMEOUT_MS)) != RT_EOK)
    {
        LOG_E("connect timeout");
        ret = XZ_ERR_TIMEOUT;
        goto out;
    }

    if (!s_ws.connected)
    {
        ret = XZ_ERR_NET;
        goto out;
    }

    ret = XZ_OK;

out:
    if (ret != XZ_OK)
        xz_ch_close();
    return ret;
}

int xz_ch_send_text(const void *data, size_t len)
{
    int ret;

    if (!data || len == 0 || len > 0xFFFF)
        return XZ_ERR_PARAM;
    if (!s_ws.inited || !s_ws.connected)
        return XZ_ERR_STATE;

    if (s_ws.lock)
        rt_mutex_take(s_ws.lock, RT_WAITING_FOREVER);

    ret = wsock_write(&s_ws_state, (const char *)data, (u16_t)len, OPCODE_TEXT);

    if (s_ws.lock)
        rt_mutex_release(s_ws.lock);

    return (ret == ERR_OK) ? XZ_OK : XZ_ERR_NET;
}

int xz_ch_send_bin(const void *data, size_t len)
{
    int ret;

    if (!data || len == 0 || len > 0xFFFF)
        return XZ_ERR_PARAM;
    if (!s_ws.inited || !s_ws.connected)
        return XZ_ERR_STATE;

    if (s_ws.lock)
        rt_mutex_take(s_ws.lock, RT_WAITING_FOREVER);

    ret = wsock_write(&s_ws_state, (const char *)data, (u16_t)len, OPCODE_BINARY);

    if (s_ws.lock)
        rt_mutex_release(s_ws.lock);

    return (ret == ERR_OK) ? XZ_OK : XZ_ERR_NET;
}

void xz_ch_close(void)
{
    int locked = 0;

    if (!s_ws.inited)
        return;

    if (s_ws.lock)
        locked = (rt_mutex_take(s_ws.lock, rt_tick_from_millisecond(1000)) == RT_EOK);

    s_ws.connected = 0;
    s_ws.cb = NULL;
    s_ws.user = NULL;
    wsock_close(&s_ws_state, WSOCK_RESULT_OK, ERR_OK);
    s_ws.inited = 0;

    if (locked)
        rt_mutex_release(s_ws.lock);
}
