/*
 *
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * MQTT transport. Incomplete and not validated yet.
 */

#include <string.h>
#include <stdlib.h>
#include <rtthread.h>
#include <lwip/api.h>
#include <lwip/dns.h>
#include <lwip/apps/mqtt.h>
#include <lwip/tcpip.h>
#ifdef LWIP_ALTCP_TLS
#include <lwip/altcp_tls.h>
#endif

#include "xz_channel.h"

#define LOG_TAG "xz_mqtt"
#include "log.h"

#define XZ_MQ_RX_MAX 2048

static struct
{
    mqtt_client_t client;
    struct mqtt_connect_client_info_t info;
    struct altcp_tls_config *tls;
    rt_sem_t sem;
    xz_ch_cb_t cb;
    void *user;
    volatile int connected;
    char topic[128];
    char rx[XZ_MQ_RX_MAX];
    size_t rx_len;
    size_t rx_expected;
} s_mq;

static int parse_host_port(const char *endpoint, char *host, size_t host_len,
                           uint16_t *port, int *secure)
{
    const char *p = endpoint;
    const char *colon;
    size_t n;

    if (!endpoint || !host || !port || !secure)
        return XZ_ERR_PARAM;

    *secure = 1;
    if (strncmp(p, "mqtts://", 8) == 0)
    {
        *port = 8883;
        *secure = 1;
        p += 8;
    }
    else if (strncmp(p, "mqtt://", 7) == 0)
    {
        *port = 1883;
        *secure = 0;
        p += 7;
    }
    else
    {
        *port = 8883;
    }

    colon = strrchr(p, ':');
    if (colon && colon[1])
    {
        n = (size_t)(colon - p);
        *port = (uint16_t)atoi(colon + 1);
    }
    else
    {
        n = strlen(p);
    }

    if (n == 0 || n >= host_len)
        return XZ_ERR_PARAM;

    memcpy(host, p, n);
    host[n] = '\0';
    if (*port == 1883)
        *secure = 0;
    else if (*port == 8883)
        *secure = 1;
    return XZ_OK;
}

static void mqtt_conn_cb(mqtt_client_t *client, void *arg,
                         mqtt_connection_status_t status)
{
    (void)client;
    (void)arg;

    if (status == MQTT_CONNECT_ACCEPTED)
    {
        s_mq.connected = 1;
        if (s_mq.sem)
            rt_sem_release(s_mq.sem);
    }
    else
    {
        s_mq.connected = 0;
        LOG_E("connect failed: %d", status);
        if (s_mq.sem)
            rt_sem_release(s_mq.sem);
    }
}

static void mqtt_inpub_cb(void *arg, const char *topic, u32_t tot_len)
{
    (void)arg;
    (void)topic;

    s_mq.rx_len = 0;
    s_mq.rx_expected = tot_len;
    if (s_mq.rx_expected >= sizeof(s_mq.rx))
        s_mq.rx_expected = sizeof(s_mq.rx) - 1;
}

static void mqtt_indata_cb(void *arg, const u8_t *data, u16_t len, u8_t flags)
{
    (void)arg;

    if (s_mq.rx_len + len < sizeof(s_mq.rx))
    {
        memcpy(s_mq.rx + s_mq.rx_len, data, len);
        s_mq.rx_len += len;
    }

    if (!flags)
        return;

    s_mq.rx[s_mq.rx_len] = '\0';
    if (s_mq.cb)
        s_mq.cb(XZ_CH_EVT_TEXT, (const uint8_t *)s_mq.rx, s_mq.rx_len, s_mq.user);
}

static void mqtt_dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    ip_addr_t *dst = (ip_addr_t *)arg;

    (void)name;

    if (ipaddr)
    {
        memcpy(dst, ipaddr, sizeof(*dst));
        if (s_mq.sem)
            rt_sem_release(s_mq.sem);
    }
}

static void mqtt_sub_cb(void *arg, err_t err)
{
    (void)arg;
    if (err != ERR_OK)
        LOG_E("subscribe failed: %d", err);
}

int xz_ch_open(const xz_ch_cfg_t *cfg, xz_ch_cb_t cb, void *user)
{
    char host[96];
    uint16_t port;
    int secure = 1;
    ip_addr_t addr;
    err_t err;
    int ret = XZ_ERR_NET;

    if (!cfg || !cfg->endpoint)
        return XZ_ERR_PARAM;

    if (parse_host_port(cfg->endpoint, host, sizeof(host), &port,
                        &secure) != XZ_OK)
        return XZ_ERR_PARAM;

    memset(&s_mq, 0, sizeof(s_mq));
    s_mq.cb = cb;
    s_mq.user = user;
    if (cfg->mqtt_topic)
    {
        strncpy(s_mq.topic, cfg->mqtt_topic, sizeof(s_mq.topic) - 1);
        s_mq.topic[sizeof(s_mq.topic) - 1] = '\0';
    }

    s_mq.sem = rt_sem_create("xz_mq", 0, RT_IPC_FLAG_FIFO);
    if (!s_mq.sem)
        return XZ_ERR_NO_MEM;

    LOCK_TCPIP_CORE();
    err = dns_gethostbyname(host, &addr, mqtt_dns_cb, &addr);
    UNLOCK_TCPIP_CORE();

    if (err == ERR_INPROGRESS)
    {
        if (rt_sem_take(s_mq.sem, rt_tick_from_millisecond(5000)) != RT_EOK)
        {
            LOG_E("dns timeout");
            ret = XZ_ERR_TIMEOUT;
            goto out;
        }
    }
    else if (err != ERR_OK)
    {
        LOG_E("dns failed: %d", err);
        ret = XZ_ERR_NET;
        goto out;
    }

    s_mq.info.client_id = cfg->mqtt_client_id ? cfg->mqtt_client_id : "xiaozhi";
    s_mq.info.client_user = cfg->mqtt_username;
    s_mq.info.client_pass = cfg->mqtt_password;
    s_mq.info.keep_alive = 90;

#ifdef LWIP_ALTCP_TLS
    if (secure)
    {
        s_mq.tls = altcp_tls_create_config_client(NULL, 0);
        s_mq.info.tls_config = s_mq.tls;
    }
#else
    (void)secure;
#endif

    LOCK_TCPIP_CORE();
    err = mqtt_client_connect(&s_mq.client, &addr, port,
                              mqtt_conn_cb, &s_mq, &s_mq.info);
    UNLOCK_TCPIP_CORE();

    if (err != ERR_OK)
    {
        LOG_E("mqtt_client_connect failed: %d", err);
        ret = XZ_ERR_NET;
        goto out;
    }

    if (rt_sem_take(s_mq.sem, rt_tick_from_millisecond(10000)) != RT_EOK)
    {
        LOG_E("connect timeout");
        ret = XZ_ERR_TIMEOUT;
        goto out;
    }

    if (!s_mq.connected)
    {
        ret = XZ_ERR_NET;
        goto out;
    }

    mqtt_set_inpub_callback(&s_mq.client, mqtt_inpub_cb, mqtt_indata_cb, &s_mq);
    LOCK_TCPIP_CORE();
    mqtt_subscribe(&s_mq.client, s_mq.topic, 0, mqtt_sub_cb, &s_mq);
    UNLOCK_TCPIP_CORE();

    ret = XZ_OK;

out:
    if (ret != XZ_OK)
        xz_ch_close();
    return ret;
}

int xz_ch_send_text(const void *data, size_t len)
{
    err_t err;

    if (!data || len == 0 || len > 0xFFFF)
        return XZ_ERR_PARAM;
    if (!s_mq.connected)
        return XZ_ERR_STATE;

    LOCK_TCPIP_CORE();
    err = mqtt_publish(&s_mq.client, s_mq.topic, data, (u16_t)len, 0, 0,
                       NULL, NULL);
    UNLOCK_TCPIP_CORE();

    return (err == ERR_OK) ? XZ_OK : XZ_ERR_NET;
}

int xz_ch_send_bin(const void *data, size_t len)
{
    (void)data;
    (void)len;
    /* UDP media path is not implemented yet */
    return XZ_ERR_UNSUPPORTED;
}

void xz_ch_close(void)
{
    s_mq.cb = NULL;
    s_mq.user = NULL;

    if (s_mq.connected)
    {
        LOCK_TCPIP_CORE();
        mqtt_disconnect(&s_mq.client);
        UNLOCK_TCPIP_CORE();
        s_mq.connected = 0;
    }

#ifdef LWIP_ALTCP_TLS
    if (s_mq.tls)
    {
        LOCK_TCPIP_CORE();
        altcp_tls_free_config(s_mq.tls);
        UNLOCK_TCPIP_CORE();
        s_mq.tls = NULL;
        s_mq.info.tls_config = NULL;
    }
#endif

    if (s_mq.sem)
    {
        rt_sem_delete(s_mq.sem);
        s_mq.sem = NULL;
    }
}
