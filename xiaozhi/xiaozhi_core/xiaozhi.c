/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include <rtthread.h>

#include "xiaozhi.h"
#include "core/channel/xz_channel.h"
#include "core/proto/xz_proto.h"
#include "core/proto/xz_http.h"
#if XIAOZHI_FEATURE_MCP
#include "feature/mcp/xz_tool.h"
#endif
#if XIAOZHI_FEATURE_AUDIO
#include "feature/audio/xz_audio.h"
#endif

#define LOG_TAG "xiaozhi"
#include "log.h"

/* ---------------- internal constants ---------------- */

#define XZ_TX_BUF_SIZE      2048                        /* hello / MCP, on the component thread */
#define XZ_CMD_BUF_SIZE     256                         /* listen / abort / keepalive, no long text */
#define XZ_TEXT_BUF_SIZE    (XZ_TEXT_MAX * 2 + 256)     /* detect, includes the text escape bound */
#define XZ_THREAD_STACK     (XIAOZHI_THREAD_STACK_SIZE > 0 ? XIAOZHI_THREAD_STACK_SIZE : 12288)

/* Send a listen/stop keepalive after this much idle time on the link */
#define XZ_KEEPALIVE_MS     30000

#if XIAOZHI_FEATURE_AUDIO
#define XZ_AUDIO_Q_NUM      4
#define XZ_AUDIO_FRAME_MAX  2048
#define XZ_AUDIO_NOTIFY     ((rt_ubase_t)1)
#endif

/* ---------------- internal data structures ---------------- */

#if XIAOZHI_FEATURE_AUDIO
typedef struct
{
    uint16_t len;
    uint8_t data[XZ_AUDIO_FRAME_MAX];
} xz_audio_frame_t;
#endif

/* Message types carried in the worker mailbox */
enum
{
    XZ_Q_DISCONNECTED = 1,
    XZ_Q_TEXT,
};

typedef struct
{
    int type;
    size_t len;
    uint8_t data[1];
} xz_qmsg_t;

/* Component instance. Never exposed, xiaozhi.h only has a forward declaration. */
struct xz_t
{
    xz_config_t cfg;

    xz_state_t state;
    xz_bootstrap_t boot;
    xz_msg_t msg;
#if XIAOZHI_FEATURE_MCP
    xz_tool_ctx_t tools;
#endif

    char session_id[XZ_SESSION_ID_MAX];
    uint32_t sample_rate;
    uint32_t frame_duration;

    rt_thread_t thread;
    rt_mailbox_t mb;
    rt_sem_t stop_sem;

    volatile int running;
    volatile int link_open;
    volatile int connected;
    volatile int disconnect_req;    /* application asked to disconnect */
    volatile int reconnect_req;     /* application asked to reconnect */
    volatile int stop_req;
    volatile int activation_ok;
    volatile int boot_stale;

    rt_tick_t last_active;          /* last business data sent or received, used by keepalive */

#if XIAOZHI_FEATURE_AUDIO
    /* Downlink Opus frames: copied in the channel callback, taken by the audio thread */
    xz_audio_frame_t audio_q[XZ_AUDIO_Q_NUM];
    volatile uint32_t audio_wr;
    volatile uint32_t audio_rd;
#endif
};

#if defined(XIAOZHI_TRANSPORT_MQTT)
#define XZ_TRANSPORT_NAME "udp"
#define XZ_USE_MQTT       1
#else
#define XZ_TRANSPORT_NAME "websocket"
#define XZ_USE_MQTT       0
#endif

static xz_t *s_inst;
static void xz_inst_destroy(xz_t *xz);
static int xz_inst_stop(xz_t *xz);

static void xz_delay(xz_t *xz, uint32_t ms)
{
    while (ms > 0 && !xz->stop_req)
    {
        uint32_t step = ms > 100 ? 100 : ms;
        rt_thread_mdelay(step);
        ms -= step;
    }
}

static void xz_emit(xz_t *xz, const xz_event_t *ev)
{
    if (xz && xz->cfg.on_event && ev)
        xz->cfg.on_event(ev, xz->cfg.user);
}

static void xz_emit_simple(xz_t *xz, xz_event_type_t type)
{
    xz_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    xz_emit(xz, &ev);
}

static void xz_emit_text(xz_t *xz, xz_event_type_t type, const char *text)
{
    xz_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.u.text.text = text ? text : "";
    xz_emit(xz, &ev);
}

static void xz_emit_disconnect(xz_t *xz, int reason)
{
    xz_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = XZ_EVT_DISCONNECTED;
    ev.u.disconnect.reason = reason;
    xz_emit(xz, &ev);
}

static int xz_send_raw(xz_t *xz, const char *json, int len)
{
    int ret;

    if (!xz || !json || len <= 0)
        return XZ_ERR_PARAM;
    if (!xz->link_open)
        return XZ_ERR_STATE;

    ret = xz_ch_send_text(json, (size_t)len);
    if (ret == XZ_OK)
        xz->last_active = rt_tick_get();
    return ret;
}

#if XIAOZHI_FEATURE_MCP
static int xz_tool_send_wrap(const char *payload, void *user)
{
    xz_t *xz = (xz_t *)user;
    char buf[XZ_TX_BUF_SIZE];
    int n;

    if (!xz || !payload)
        return XZ_ERR_PARAM;

    n = xz_proto_build_mcp(buf, sizeof(buf), xz->session_id, payload);
    if (n < 0)
        return n;
    return xz_send_raw(xz, buf, n);
}
#endif

static void xz_ch_event_cb(xz_ch_evt_t evt, const uint8_t *data, size_t len,
                           void *user)
{
    xz_t *xz = (xz_t *)user;
    xz_qmsg_t *q;
    int type;

    if (!xz || !xz->mb)
        return;

    if (evt == XZ_CH_EVT_BIN)
    {
#if XIAOZHI_FEATURE_AUDIO
        uint32_t old;
        uint32_t next;

        if (!data || len == 0 || len > XZ_AUDIO_FRAME_MAX)
            return;

        old = xz->audio_wr;
        next = (old + 1) % XZ_AUDIO_Q_NUM;
        if (next == xz->audio_rd)
            return; /* queue full, drop frame */

        xz->audio_q[old].len = (uint16_t)len;
        memcpy(xz->audio_q[old].data, data, len);
        xz->audio_wr = next;

        if (rt_mb_send(xz->mb, XZ_AUDIO_NOTIFY) != RT_EOK)
            xz->audio_wr = old;
#endif
        return;     
    }

    if (evt == XZ_CH_EVT_CONNECTED)
        return;    

    type = (evt == XZ_CH_EVT_DISCONNECTED) ? XZ_Q_DISCONNECTED : XZ_Q_TEXT;

    q = rt_malloc(sizeof(xz_qmsg_t) + len + 1);
    if (!q)
        return;

    q->type = type;
    q->len = len;
    if (data && len)
        memcpy(q->data, data, len);
    q->data[len] = '\0';

    if (rt_mb_send(xz->mb, (rt_ubase_t)q) != RT_EOK)
        rt_free(q);
}

static void xz_handle_tts(xz_t *xz)
{
    if (strcmp(xz->msg.state, "start") == 0)
    {

        xz->state = XZ_STATE_SPEAKING;
#if XIAOZHI_FEATURE_AUDIO
        /* the server started talking, nothing to record in this round */
        xz_audio_mic_off();
#endif
        xz_emit_simple(xz, XZ_EVT_TTS_START);
    }
    else if (strcmp(xz->msg.state, "sentence_start") == 0)
    {
        xz_emit_text(xz, XZ_EVT_TTS_SENTENCE, xz->msg.text);
    }
    else if (strcmp(xz->msg.state, "stop") == 0)
    {
        xz->state = XZ_STATE_IDLE;
        xz_emit_simple(xz, XZ_EVT_TTS_STOP);
    }
}

static void xz_handle_text(xz_t *xz, const char *json)
{
    int ret = xz_proto_parse(json, &xz->msg);

    if (ret != XZ_OK)
    {
        LOG_W("protocol parse error %d", ret);
        return;
    }

    switch (xz->msg.type)
    {
    case XZ_MSG_HELLO:
        if (xz->msg.session_id[0])
        {
            strncpy(xz->session_id, xz->msg.session_id,
                    sizeof(xz->session_id) - 1);
            xz->session_id[sizeof(xz->session_id) - 1] = '\0';
        }
        xz->sample_rate = xz->msg.sample_rate ? xz->msg.sample_rate
                                              : XZ_DEFAULT_SAMPLE_RATE;
        xz->frame_duration = xz->msg.frame_duration ? xz->msg.frame_duration
                                                    : XZ_DEFAULT_FRAME_MS;
        xz->connected = 1;
        xz->state = XZ_STATE_IDLE;
        xz->last_active = rt_tick_get();
        xz_emit_simple(xz, XZ_EVT_SESSION_STARTED);
        xz_emit_simple(xz, XZ_EVT_CONNECTED);
        LOG_I("session %s, %u Hz / %u ms",
              xz->session_id, (unsigned)xz->sample_rate,
              (unsigned)xz->frame_duration);
        break;

    case XZ_MSG_GOODBYE:
        xz->state = XZ_STATE_IDLE;
        xz_emit_simple(xz, XZ_EVT_SESSION_ENDED);
        LOG_I("session ended by server");
        break;

    case XZ_MSG_STT:
        xz_emit_text(xz, XZ_EVT_ASR_FINAL, xz->msg.text);
        break;

    case XZ_MSG_TTS:
        xz_handle_tts(xz);
        break;

    case XZ_MSG_LLM:
        if (xz->msg.emotion[0])
            xz_emit_text(xz, XZ_EVT_EMOTION, xz->msg.emotion);
        break;

#if XIAOZHI_FEATURE_MCP
    case XZ_MSG_MCP:
        if (xz->msg.payload[0])
            xz_tool_handle_mcp(&xz->tools, xz->msg.payload);
        break;
#endif

    default:
        break;
    }
}

static void xz_handle_q(xz_t *xz, xz_qmsg_t *q)
{
    if (!xz || !q)
        return;

    switch (q->type)
    {
    case XZ_Q_DISCONNECTED:
        xz->connected = 0;
        xz->link_open = 0;
        xz->state = XZ_STATE_PAUSED;
        xz_emit_disconnect(xz, XZ_DISCONNECT_NET);
        LOG_W("channel disconnected");
#if XIAOZHI_FEATURE_AUDIO
        /* Link is gone, release mic and speaker until reconnect */
        xz_audio_hw_off();
#endif
        break;

    case XZ_Q_TEXT:
        xz_handle_text(xz, (const char *)q->data);
        break;

    default:
        break;
    }
}

#if XIAOZHI_FEATURE_AUDIO
static void xz_handle_audio(xz_t *xz)
{
    while (xz->audio_rd != xz->audio_wr)
    {
        xz_audio_frame_t *frame = &xz->audio_q[xz->audio_rd];

        xz->audio_rd = (xz->audio_rd + 1) % XZ_AUDIO_Q_NUM;
        xz_audio_on_tts_audio(frame->data, frame->len);
    }
}
#endif

static void xz_recv_one(xz_t *xz, int timeout_ms)
{
    xz_qmsg_t *q = NULL;

    if (rt_mb_recv(xz->mb, (rt_ubase_t *)&q,
                   rt_tick_from_millisecond(timeout_ms)) == RT_EOK)
    {
        xz->last_active = rt_tick_get();

#if XIAOZHI_FEATURE_AUDIO
        if ((rt_ubase_t)q == XZ_AUDIO_NOTIFY)
        {
            xz_handle_audio(xz);
            return;
        }
#endif

        xz_handle_q(xz, q);
        rt_free(q);
    }
}

static int xz_ensure_bootstrap(xz_t *xz)
{
    int ret;

    if (xz->boot_stale)
    {
        memset(&xz->boot, 0, sizeof(xz->boot));
        xz->boot_stale = 0;
    }

    if (!xz->boot.has_websocket && !xz->boot.has_mqtt)
    {
        xz->state = XZ_STATE_BOOTSTRAP;
        ret = xz_http_bootstrap(&xz->cfg, &xz->boot);
        if (ret != XZ_OK)
        {
            LOG_E("bootstrap failed: %d", ret);
            return ret;
        }
    }

#if XZ_USE_MQTT
    return xz->boot.has_mqtt ? XZ_OK : XZ_ERR_PROTOCOL;
#else
    return xz->boot.has_websocket ? XZ_OK : XZ_ERR_PROTOCOL;
#endif
}

/*
 * When the server requires activation: hand the code to the application and
 * wait for xz_activation_confirm(), up to 5 minutes. XZ_OK means linking may
 * continue.
 */
static int xz_wait_activation(xz_t *xz)
{
    xz_event_t ev;
    uint32_t deadline;

    if (!xz->boot.activation_required)
        return XZ_OK;

    memset(&ev, 0, sizeof(ev));
    ev.type = XZ_EVT_ACTIVATION_REQUIRED;
    ev.u.activation.code = xz->boot.activation_code;
    ev.u.activation.message = xz->boot.activation_message;
    xz_emit(xz, &ev);

    LOG_W("activation required, code: %s", xz->boot.activation_code);

    deadline = rt_tick_get() + rt_tick_from_millisecond(5 * 60 * 1000);
    while (!xz->activation_ok && !xz->stop_req &&
           (int32_t)(deadline - rt_tick_get()) > 0)
    {
        rt_thread_mdelay(200);
    }

    xz->activation_ok = 0;
    xz->boot.activation_required = 0;

    return xz->stop_req ? XZ_ERR_STATE : XZ_OK;
}

/*
 * Open the link with the bootstrap parameters and send hello.
 * XZ_OK only means hello was sent; the connection is usable once the worker
 * thread receives the server hello.
 */
static int xz_open_link(xz_t *xz)
{
    xz_ch_cfg_t ch;
    char hello[XZ_TX_BUF_SIZE];
    int n;
    int ret;

    memset(&ch, 0, sizeof(ch));
    if (XZ_USE_MQTT)
    {
        ch.endpoint = xz->boot.mqtt_endpoint;
        ch.mqtt_client_id = xz->boot.mqtt_client_id;
        ch.mqtt_username = xz->boot.mqtt_username;
        ch.mqtt_password = xz->boot.mqtt_password;
        ch.mqtt_topic = xz->boot.mqtt_topic;
    }
    else
    {
        ch.url = xz->boot.ws_url;
        ch.token = xz->boot.ws_token;
    }
    ch.device_id = xz->cfg.device_id;
    ch.client_id = xz->cfg.client_id;

    xz->state = XZ_STATE_CONNECTING;
    ret = xz_ch_open(&ch, xz_ch_event_cb, xz);
    if (ret != XZ_OK)
    {
        LOG_E("channel open failed: %d", ret);
        return ret;
    }
    xz->link_open = 1;

    n = xz_proto_build_hello(hello, sizeof(hello), XZ_TRANSPORT_NAME);
    if (n < 0)
    {
        xz_ch_close();
        xz->link_open = 0;
        return XZ_ERR_INTERNAL;
    }

    ret = xz_send_raw(xz, hello, n);
    if (ret != XZ_OK)
    {
        xz_ch_close();
        xz->link_open = 0;
        return ret;
    }

    return XZ_OK;
}

/* Connect once: fetch parameters -> wait for activation -> open link and send hello */
static int xz_connect_once(xz_t *xz)
{
    int ret;

    ret = xz_ensure_bootstrap(xz);
    if (ret != XZ_OK)
        return ret;

    ret = xz_wait_activation(xz);
    if (ret != XZ_OK)
        return ret;

    return xz_open_link(xz);
}

/* Close the link, state becomes PAUSED and the application decides on reconnecting */
static void xz_close_link(xz_t *xz)
{
    if (xz->link_open)
    {
        xz_ch_close();
        xz->link_open = 0;
    }
    xz->connected = 0;
    if (xz->state != XZ_STATE_CLOSED)
        xz->state = XZ_STATE_PAUSED;
}

/* Connect once and wait for hello, on failure stay in PAUSED without retrying */
static int xz_connect_and_wait(xz_t *xz)
{
    uint32_t timeout_ms;
    uint32_t deadline;

    xz_emit_simple(xz, XZ_EVT_CONNECTING);

    if (xz_connect_once(xz) != XZ_OK)
    {
        xz->state = XZ_STATE_PAUSED;
        xz_emit_disconnect(xz, XZ_DISCONNECT_NET);
        return -1;
    }

    timeout_ms = xz->cfg.timeout_connect_ms ? xz->cfg.timeout_connect_ms : 15000;
    deadline = rt_tick_get() + rt_tick_from_millisecond(timeout_ms);

    while (!xz->connected && !xz->stop_req &&
           (int32_t)(deadline - rt_tick_get()) > 0)
    {
        xz_recv_one(xz, 200);
    }

    if (!xz->connected)
    {
        LOG_E("hello timeout");
        xz_close_link(xz);
        xz_emit_disconnect(xz, XZ_DISCONNECT_TIMEOUT);
        return -1;
    }

    return 0;
}

/*
 * Keepalive: send a listen/stop after XZ_KEEPALIVE_MS with no business traffic.
 * A WebSocket ping is not used because the server ignores ping frames and still
 * closes after 60 s without business data; only application messages refresh
 * its idle timer. Sent in IDLE only so it never disturbs an interaction.
 */
static void xz_keepalive(xz_t *xz)
{
    char buf[XZ_CMD_BUF_SIZE];
    int n;

    if (xz->state != XZ_STATE_IDLE)
        return;
    if ((rt_tick_t)(rt_tick_get() - xz->last_active) <
        rt_tick_from_millisecond(XZ_KEEPALIVE_MS))
        return;

    n = xz_proto_build_listen_stop(buf, sizeof(buf), xz->session_id);
    if (n > 0)
        xz_send_raw(xz, buf, n);

    xz->last_active = rt_tick_get();
}

static void xz_thread_entry(void *arg)
{
    xz_t *xz = (xz_t *)arg;

    xz->state = XZ_STATE_INIT;

    /* First connection: on failure stay in PAUSED and wait for xz_reconnect() */
    xz_connect_and_wait(xz);

    while (!xz->stop_req)
    {
        if (xz->disconnect_req)
        {
            xz->disconnect_req = 0;
            xz_close_link(xz);
            continue;           /* no auto reconnect */
        }

        if (xz->reconnect_req)
        {
            xz->reconnect_req = 0;
            xz_close_link(xz);
            xz_connect_and_wait(xz);
            continue;
        }

        if (!xz->connected)
        {
            xz_delay(xz, 200);  /* already disconnected, wait for the application */
            continue;
        }

        xz_recv_one(xz, 1000);
        xz_keepalive(xz);
    }

    xz_close_link(xz);
    xz->state = XZ_STATE_CLOSED;
    xz->running = 0;
    rt_sem_release(xz->stop_sem);
}


/* Duplicate a string, NULL on failure. The caller may free the original. */
static char *xz_strdup(const char *s)
{
    char *p;
    size_t n;

    if (!s)
        return NULL;

    n = strlen(s) + 1;
    p = rt_malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static xz_t *xz_inst_create(const xz_config_t *cfg)
{
    xz_t *xz;

    if (!cfg || !cfg->device_id || !cfg->client_id)
        return NULL;

    xz = rt_calloc(1, sizeof(*xz));
    if (!xz)
        return NULL;

    xz->cfg.timeout_bootstrap_ms = cfg->timeout_bootstrap_ms ? cfg->timeout_bootstrap_ms : 10000;
    xz->cfg.timeout_connect_ms = cfg->timeout_connect_ms ? cfg->timeout_connect_ms : 15000;
    xz->cfg.on_event = cfg->on_event;
    xz->cfg.user = cfg->user;
#if XIAOZHI_FEATURE_MCP
    xz->cfg.tools = cfg->tools;
    xz->cfg.tool_count = cfg->tool_count;
#endif

    xz->cfg.device_id = xz_strdup(cfg->device_id);
    xz->cfg.client_id = xz_strdup(cfg->client_id);
    xz->cfg.model = xz_strdup(cfg->model ? cfg->model : "sf32lb52");
    xz->cfg.firmware_version = xz_strdup(cfg->firmware_version ?
                                          cfg->firmware_version : XZ_VERSION_STR);
    xz->cfg.server_url = xz_strdup(cfg->server_url ?
                                   cfg->server_url : "https://api.tenclass.net");
    if (!xz->cfg.device_id || !xz->cfg.client_id ||
        !xz->cfg.model || !xz->cfg.firmware_version || !xz->cfg.server_url)
    {
        xz_inst_destroy(xz);
        return NULL;
    }

    xz->mb = rt_mb_create("xz_mb", 16, RT_IPC_FLAG_FIFO);
    xz->stop_sem = rt_sem_create("xz_stop", 0, RT_IPC_FLAG_FIFO);
    if (!xz->mb || !xz->stop_sem)
    {
        xz_inst_destroy(xz);
        return NULL;
    }

#if XIAOZHI_FEATURE_MCP
    xz_tool_ctx_init(&xz->tools, xz_tool_send_wrap, xz);
#endif
    xz->state = XZ_STATE_INIT;

    return xz;
}

static void xz_inst_destroy(xz_t *xz)
{
    if (!xz)
        return;

    xz_inst_stop(xz);

    if (xz->mb)
    {
        rt_ubase_t v;

        while (rt_mb_recv(xz->mb, &v, 0) == RT_EOK)
        {
#if XIAOZHI_FEATURE_AUDIO
            if (v == XZ_AUDIO_NOTIFY)
                continue;      
#endif
            rt_free((void *)v);
        }

        rt_mb_delete(xz->mb);
    }
    if (xz->stop_sem)
        rt_sem_delete(xz->stop_sem);

    /* Free the strings copied by xz_inst_create() */
    rt_free((void *)xz->cfg.device_id);
    rt_free((void *)xz->cfg.client_id);
    rt_free((void *)xz->cfg.model);
    rt_free((void *)xz->cfg.firmware_version);
    rt_free((void *)xz->cfg.server_url);

    rt_free(xz);
}

static int xz_inst_start(xz_t *xz)
{
    if (!xz)
        return XZ_ERR_PARAM;
    if (xz->running)
        return XZ_ERR_STATE;

    xz->stop_req = 0;
    xz->disconnect_req = 0;
    xz->reconnect_req = 0;
    xz->connected = 0;
    xz->link_open = 0;
    xz->running = 1;
    xz->state = XZ_STATE_INIT;

    xz->thread = rt_thread_create("xiaozhi", xz_thread_entry, xz,
                                  XZ_THREAD_STACK, 20, 10);
    if (!xz->thread)
    {
        xz->running = 0;
        return XZ_ERR_NO_MEM;
    }

    rt_thread_startup(xz->thread);
    return XZ_OK;
}

static int xz_inst_stop(xz_t *xz)
{
    if (!xz)
        return XZ_ERR_PARAM;
    if (!xz->running && !xz->thread)
        return XZ_OK;

    xz->stop_req = 1;
    if (rt_sem_take(xz->stop_sem, rt_tick_from_millisecond(5000)) != RT_EOK)
    {

        LOG_E("stop timeout, force close link and delete thread");
        xz_ch_close();
        if (xz->thread)
            rt_thread_delete(xz->thread);
        xz->connected = 0;
        xz->link_open = 0;
        xz->running = 0;
    }
    xz->thread = NULL;
    return XZ_OK;
}

/* ==================== public API ==================== */


int xz_start(const xz_config_t *cfg)
{
    int ret;
#if XIAOZHI_FEATURE_MCP
    uint8_t i;
#endif

    if (!cfg || !cfg->device_id || !cfg->client_id)
        return XZ_ERR_PARAM;
    if (s_inst)
        return XZ_ERR_STATE;      /* already running, call xz_stop() first */

    s_inst = xz_inst_create(cfg);
    if (!s_inst)
        return XZ_ERR_NO_MEM;

#if XIAOZHI_FEATURE_MCP

    if (cfg->tools)
    {
        for (i = 0; i < cfg->tool_count && i < XZ_MAX_TOOLS; i++)
            xz_tool_register(&s_inst->tools, &cfg->tools[i]);
    }
    s_inst->cfg.tools = NULL;
    s_inst->cfg.tool_count = 0;
#endif

#if XIAOZHI_FEATURE_AUDIO
    xz_audio_ctx_open(cfg->audio_out_cb, cfg->user);
#endif

    ret = xz_inst_start(s_inst);
    if (ret != XZ_OK)
    {
        xz_t *xz = s_inst;

        s_inst = NULL;
        /* Same order as xz_stop(): stop the component first, then tear down audio */
        xz_inst_destroy(xz);
#if XIAOZHI_FEATURE_AUDIO
        xz_audio_ctx_close();
#endif
        return ret;
    }

    return XZ_OK;
}

int xz_stop(void)
{
    xz_t *xz;

    if (!s_inst)
        return XZ_OK;

    xz = s_inst;
    s_inst = NULL;

    xz_inst_destroy(xz);

#if XIAOZHI_FEATURE_AUDIO
    xz_audio_ctx_close();
#endif
    return XZ_OK;
}

int xz_tool_complete(uint32_t call_id, const char *result_text)
{
#if XIAOZHI_FEATURE_MCP
    if (!s_inst)
        return XZ_ERR_STATE;
    return xz_tool_ctx_complete(&s_inst->tools, call_id, result_text);
#else
    (void)call_id;
    (void)result_text;
    return XZ_ERR_UNSUPPORTED;
#endif
}

#if XIAOZHI_FEATURE_AUDIO

int xz_listen_start(xz_listen_mode_t mode)
{
    char buf[XZ_CMD_BUF_SIZE];
    int n;

    if (!s_inst || !s_inst->connected)
        return XZ_ERR_STATE;

    n = xz_proto_build_listen_start(buf, sizeof(buf), s_inst->session_id, (int)mode);
    if (n < 0)
        return n;
    if (xz_send_raw(s_inst, buf, n) != XZ_OK)
        return XZ_ERR_NET;

    s_inst->state = XZ_STATE_LISTENING;

    /* Open the microphone only when recording really starts, idle keeps hardware off */
    xz_audio_mic_on();
    return XZ_OK;
}

int xz_listen_stop(void)
{
    char buf[XZ_CMD_BUF_SIZE];
    int n;

    if (!s_inst || !s_inst->connected)
        return XZ_ERR_STATE;

    n = xz_proto_build_listen_stop(buf, sizeof(buf), s_inst->session_id);
    if (n < 0)
        return n;
    if (xz_send_raw(s_inst, buf, n) != XZ_OK)
        return XZ_ERR_NET;

    s_inst->state = XZ_STATE_IDLE;

    /* Recording done, close it and wait for the next listen_start */
    xz_audio_mic_off();
    return XZ_OK;
}

#endif /* XIAOZHI_FEATURE_AUDIO */

int xz_send_text(const char *text)
{
    char buf[XZ_TEXT_BUF_SIZE];
    int n;

    if (!s_inst || !text || !s_inst->connected)
        return XZ_ERR_STATE;

    n = xz_proto_build_detect(buf, sizeof(buf), s_inst->session_id, text);
    if (n < 0)
        return n;
    return xz_send_raw(s_inst, buf, n);
}

int xz_abort(xz_abort_reason_t reason)
{
    char buf[XZ_CMD_BUF_SIZE];
    int n;

    if (!s_inst || !s_inst->connected)
        return XZ_ERR_STATE;

    n = xz_proto_build_abort(buf, sizeof(buf), s_inst->session_id,
                             reason == XZ_ABORT_WAKE_WORD);
    if (n < 0)
        return n;
    if (xz_send_raw(s_inst, buf, n) != XZ_OK)
        return XZ_ERR_NET;

    s_inst->state = XZ_STATE_IDLE;
    return XZ_OK;
}

xz_state_t xz_get_state(void)
{
    return s_inst ? s_inst->state : XZ_STATE_CLOSED;
}

int xz_get_negotiated(uint32_t *sample_rate, uint32_t *frame_duration)
{
    if (!s_inst)
        return XZ_ERR_STATE;
    if (sample_rate)
        *sample_rate = s_inst->sample_rate;
    if (frame_duration)
        *frame_duration = s_inst->frame_duration;
    return XZ_OK;
}

const char *xz_get_session_id(void)
{
    return s_inst ? s_inst->session_id : "";
}

int xz_activation_confirm(void)
{
    if (!s_inst)
        return XZ_ERR_STATE;
    s_inst->activation_ok = 1;
    return XZ_OK;
}

int xz_disconnect(void)
{
    if (!s_inst || s_inst->stop_req)
        return XZ_ERR_STATE;

    /* The worker closes the link on its next round and stops in PAUSED */
    s_inst->disconnect_req = 1;

#if XIAOZHI_FEATURE_AUDIO
    /* Turn mic and speaker off right away to save power, threads and Opus are kept */
    xz_audio_hw_off();
#endif
    return XZ_OK;
}

int xz_reconnect(void)
{
    if (!s_inst || s_inst->stop_req)
        return XZ_ERR_STATE;

    s_inst->boot_stale = 1;    /* token may have expired, bootstrap again */
    s_inst->reconnect_req = 1;
    return XZ_OK;
}
