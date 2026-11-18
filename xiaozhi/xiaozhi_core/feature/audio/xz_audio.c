/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>
#include <string.h>
#include <stdint.h>

#include "audio_server.h"
#include "opus.h"
#include "opus_types.h"

#include "xiaozhi.h"
#include "core/channel/xz_channel.h"
#include "xz_audio.h"

#define LOG_TAG "xz_audio"
#include "log.h"


#define XZ_MIC_RATE         XZ_UPLINK_SAMPLE_RATE
#define XZ_MIC_FRAME_MS     XZ_UPLINK_FRAME_MS
#define XZ_MIC_FRAME_SAMPLES (XZ_MIC_RATE / 1000 * XZ_MIC_FRAME_MS)  /* 960 */

#define XZ_MIC_Q_NUM        4

#define XZ_SPK_CACHE_BYTES  16000
/* speaker volume, 0~15 */
#define XZ_SPK_VOLUME       8


#define XZ_PLAY_PKT_MAX     512
#define XZ_PLAY_Q_NUM       8

#define XZ_PLAY_MAX_SAMPLES 2880

#define XZ_AUDIO_STACK      32768
#define XZ_AUDIO_PRIO       18

#define EVT_MIC             (1u << 0)
#define EVT_PLAY            (1u << 1)
#define EVT_EXIT            (1u << 2)
#define EVT_MIC_ON          (1u << 3)
#define EVT_MIC_OFF         (1u << 4)
#define EVT_HW_OFF          (1u << 5)
#define EVT_ALL             (EVT_MIC | EVT_PLAY | EVT_EXIT | \
                             EVT_MIC_ON | EVT_MIC_OFF | EVT_HW_OFF)

/* ---------------- state ---------------- */

static rt_event_t       s_evt;
static rt_sem_t         s_exit_sem;
static struct rt_thread s_thread;
static rt_uint8_t       s_stack[XZ_AUDIO_STACK] ALIGN(RT_ALIGN_SIZE);
static int              s_inited;

static xz_audio_out_cb_t s_out_cb;
static void             *s_out_user;

/* audio_server handles */
static audio_client_t   s_mic;
static audio_client_t   s_spk;
static uint32_t         s_spk_rate;

static OpusEncoder     *s_enc;
static int16_t          s_enc_in[XZ_MIC_FRAME_SAMPLES];
static uint32_t         s_enc_in_len;      /* samples buffered so far */

static OpusDecoder     *s_dec;
static uint32_t         s_dec_rate;

/* a full frame is queued for the audio thread to encode */
static int16_t          s_mic_q[XZ_MIC_Q_NUM][XZ_MIC_FRAME_SAMPLES];
static volatile uint32_t s_mic_wr;
static volatile uint32_t s_mic_rd;

/* downlink: Opus packet queue -> decode -> out_cb */
typedef struct
{
    uint16_t len;
    uint8_t  data[XZ_PLAY_PKT_MAX];
} xz_play_pkt_t;

static xz_play_pkt_t    s_play_q[XZ_PLAY_Q_NUM];
static volatile uint32_t s_play_wr;
static volatile uint32_t s_play_rd;

static int16_t          s_pcm_out[XZ_PLAY_MAX_SAMPLES];

/* ---------------- speaker ---------------- */

/*
 * Opened when the first downlink frame arrives.
 */
static int speaker_open(uint32_t rate)
{
    audio_parameter_t pa;
    audio_client_t spk;

    memset(&pa, 0, sizeof(pa));
    pa.write_bits_per_sample = 16;
    pa.write_channnel_num = 1;
    pa.write_samplerate = rate;
    pa.write_cache_size = XZ_SPK_CACHE_BYTES;

    spk = audio_open(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_TX, &pa, RT_NULL, RT_NULL);
    if (!spk)
    {
        LOG_E("open speaker failed");
        return -1;
    }

    s_spk = spk;
    s_spk_rate = rate;
    LOG_I("speaker on: %u Hz", (unsigned)rate);
    return 0;
}

static void speaker_close(void)
{
    if (!s_spk)
        return;

    audio_close(s_spk);
    s_spk = RT_NULL;
    s_spk_rate = 0;
    LOG_I("speaker off");
}


static void speaker_write(const uint8_t *data, uint32_t len)
{
    int tries = 0;

    if (!s_spk)
        return;

    while (len > 0 && tries < 20)
    {
        int n = audio_write(s_spk, (uint8_t *)data, len);

        if (n > 0)
        {
            data += n;
            len -= (uint32_t)n;
            tries = 0;
        }
        else
        {
            rt_thread_mdelay(10);
            tries++;
        }
    }
}

/* default downlink sink: the local speaker */
static void xz_audio_speaker_out(const int16_t *pcm, size_t samples,
                                 uint32_t sample_rate, void *user)
{
    (void)user;

    if (!pcm || samples == 0)
        return;

    if (s_spk && s_spk_rate != sample_rate)
        speaker_close();
    if (!s_spk && speaker_open(sample_rate) != 0)
        return;

    speaker_write((const uint8_t *)pcm, (uint32_t)samples * 2);
}

/* ---------------- microphone ---------------- */

/* Upload only while listening, other states drop data and reset the partial frame */
static int xz_audio_can_send(void)
{
    return xz_get_state() == XZ_STATE_LISTENING;
}

static void xz_audio_drop_pending(void)
{
    s_enc_in_len = 0;
    s_mic_wr = 0;
    s_mic_rd = 0;
}

static void xz_audio_feed_pcm(const int16_t *pcm, size_t samples)
{
    if (!s_inited || !pcm || samples == 0)
        return;

    if (!xz_audio_can_send())
    {
        xz_audio_drop_pending();
        return;                 /* not listening, drop it */
    }

    while (samples > 0)
    {
        uint32_t room = XZ_MIC_FRAME_SAMPLES - s_enc_in_len;
        uint32_t take = (samples < room) ? (uint32_t)samples : room;

        memcpy(&s_enc_in[s_enc_in_len], pcm, take * sizeof(int16_t));
        s_enc_in_len += take;
        pcm += take;
        samples -= take;

        if (s_enc_in_len < XZ_MIC_FRAME_SAMPLES)
            break;

        /* full frame goes to the queue, dropped when the queue is full */
        if ((s_mic_wr + 1) % XZ_MIC_Q_NUM != s_mic_rd)
        {
            memcpy(s_mic_q[s_mic_wr], s_enc_in, sizeof(s_enc_in));
            s_mic_wr = (s_mic_wr + 1) % XZ_MIC_Q_NUM;
            rt_event_send(s_evt, EVT_MIC);
        }
        s_enc_in_len = 0;
    }
}

/* called every 10 ms by audio_server, data_len is always 320 (16 kHz/16-bit/mono) */
static int mic_cb(audio_server_callback_cmt_t cmd, void *user, uint32_t reserved)
{
    audio_server_coming_data_t *d;

    (void)user;

    if (cmd != as_callback_cmd_data_coming)
        return 0;

    d = (audio_server_coming_data_t *)reserved;
    if (!d || !d->data || d->data_len == 0)
        return 0;

    xz_audio_feed_pcm((const int16_t *)d->data, d->data_len / 2);
    return 0;
}

static int mic_open(void)
{
    audio_parameter_t pa;
    audio_client_t mic;

    if (s_mic)
        return 0;               /* already open, ignore duplicate opens */

    memset(&pa, 0, sizeof(pa));
    pa.read_bits_per_sample = 16;
    pa.read_channnel_num = 1;
    pa.read_samplerate = XZ_MIC_RATE;
    pa.read_cache_size = 0;
    pa.write_bits_per_sample = 16;
    pa.write_channnel_num = 1;
    pa.write_samplerate = XZ_MIC_RATE;
    pa.write_cache_size = 0;

    mic = audio_open(AUDIO_TYPE_LOCAL_RECORD, AUDIO_RX, &pa, mic_cb, RT_NULL);
    if (!mic)
    {
        LOG_E("open mic failed");
        return -1;
    }

    s_mic = mic;
    LOG_I("mic on");
    return 0;
}

static void mic_close(void)
{
    if (!s_mic)
        return;

    audio_close(s_mic);
    s_mic = RT_NULL;
    LOG_I("mic off");
}

/* ---------------- encoding ---------------- */

static void xz_audio_encode_tick(void)
{
    uint8_t out[XZ_PLAY_PKT_MAX];

    while (s_mic_rd != s_mic_wr)
    {
        int16_t *pcm = s_mic_q[s_mic_rd];
        int n;

        s_mic_rd = (s_mic_rd + 1) % XZ_MIC_Q_NUM;

        if (!s_enc)
            continue;

        n = opus_encode(s_enc, pcm, XZ_MIC_FRAME_SAMPLES, out, sizeof(out));
        if (n > 0)
            xz_ch_send_bin(out, (size_t)n);
    }
}

/* ---------------- decoding + playback ---------------- */

static int xz_audio_decoder_ensure(void)
{
    uint32_t rate = XZ_DEFAULT_SAMPLE_RATE;
    int err = 0;

    if (xz_get_negotiated(&rate, RT_NULL) != XZ_OK || rate == 0)
        rate = XZ_DEFAULT_SAMPLE_RATE;

    if (s_dec && s_dec_rate == rate)
        return 0;

    if (s_dec)
    {
        opus_decoder_destroy(s_dec);
        s_dec = RT_NULL;
    }

    s_dec = opus_decoder_create((opus_int32)rate, 1, &err);
    if (!s_dec)
    {
        LOG_E("opus decoder create failed: %d", err);
        return -1;
    }

    s_dec_rate = rate;
    return 0;
}

static void xz_audio_play_tick(void)
{
    while (s_play_rd != s_play_wr)
    {
        xz_play_pkt_t *pkt = &s_play_q[s_play_rd];
        int samples;

        s_play_rd = (s_play_rd + 1) % XZ_PLAY_Q_NUM;

        if (!s_out_cb)
            continue;

        if (xz_audio_decoder_ensure() != 0)
            continue;

        samples = opus_decode(s_dec, pkt->data, pkt->len,
                              (opus_int16 *)s_pcm_out, XZ_PLAY_MAX_SAMPLES, 0);
        if (samples > 0)
            s_out_cb(s_pcm_out, (size_t)samples, s_dec_rate, s_out_user);
    }
}

/* ---------------- worker thread ---------------- */

static void xz_audio_thread_entry(void *arg)
{
    (void)arg;

    for (;;)
    {
        rt_uint32_t evt = 0;

        rt_event_recv(s_evt, EVT_ALL,
                      RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      RT_WAITING_FOREVER, &evt);

        if (evt & EVT_EXIT)
            break;

        /*
         * All hardware toggling happens on this thread so it never races with
         * xz_audio_speaker_out() over s_mic / s_spk. Open wins over close when
         * both arrive in the same round.
         */
        if (evt & EVT_MIC_OFF)
            mic_close();

        if (evt & EVT_HW_OFF)
        {
            speaker_close();
            mic_close();
        }

        if (evt & EVT_MIC_ON)
        {
            audio_server_set_private_volume(AUDIO_TYPE_LOCAL_MUSIC, XZ_SPK_VOLUME);
            mic_open();
        }

        if (evt & EVT_MIC)
            xz_audio_encode_tick();

        if (evt & EVT_PLAY)
            xz_audio_play_tick();
    }

    rt_sem_release(s_exit_sem);
}

/* ---------------- internal API ---------------- */

int xz_audio_ctx_open(xz_audio_out_cb_t out_cb, void *user)
{
    int err = 0;

    if (s_inited)
        return 0;

    /* Ring indices and the partial frame must be cleared, otherwise leftover packets would be decoded as valid Opus */
    s_enc_in_len = 0;
    s_mic_wr = 0;
    s_mic_rd = 0;
    s_play_wr = 0;
    s_play_rd = 0;

    /* Without an out_cb the local speaker is used, otherwise the PCM is handed over */
    s_out_cb = out_cb ? out_cb : xz_audio_speaker_out;
    s_out_user = user;

    s_evt = rt_event_create("xz_audio", 0);
    s_exit_sem = rt_sem_create("xz_aexit", 0, RT_IPC_FLAG_FIFO);
    if (!s_evt || !s_exit_sem)
        goto fail;

    s_enc = opus_encoder_create(XZ_MIC_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (!s_enc)
    {
        LOG_E("opus encoder create failed: %d", err);
        goto fail;
    }

    /*
     * Uplink is 16 kHz / mono / 60 ms. Only SILK supports 60 ms frames, so the
     * bandwidth is capped at wideband; 16 kbps at complexity 0 fits SF32LB52.
     */
    opus_encoder_ctl(s_enc, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_60_MS));
    opus_encoder_ctl(s_enc, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND));
    opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(16000));
    opus_encoder_ctl(s_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(s_enc, OPUS_SET_VBR(1));
    opus_encoder_ctl(s_enc, OPUS_SET_VBR_CONSTRAINT(1));
    opus_encoder_ctl(s_enc, OPUS_SET_LSB_DEPTH(16));
    opus_encoder_ctl(s_enc, OPUS_SET_DTX(0));
    opus_encoder_ctl(s_enc, OPUS_SET_INBAND_FEC(0));

    if (rt_thread_init(&s_thread, "xz_audio", xz_audio_thread_entry, RT_NULL,
                       s_stack, sizeof(s_stack),
                       XZ_AUDIO_PRIO, 10) != RT_EOK)
        goto fail;
    rt_thread_startup(&s_thread);

    s_inited = 1;

    LOG_I("audio pipeline ready, opus %s", opus_get_version_string());
    return 0;

fail:
    if (s_enc)
    {
        opus_encoder_destroy(s_enc);
        s_enc = RT_NULL;
    }
    if (s_evt)
    {
        rt_event_delete(s_evt);
        s_evt = RT_NULL;
    }
    if (s_exit_sem)
    {
        rt_sem_delete(s_exit_sem);
        s_exit_sem = RT_NULL;
    }
    s_out_cb = RT_NULL;
    LOG_E("audio pipeline init failed");
    return -1;
}

void xz_audio_ctx_close(void)
{
    if (!s_inited)
        return;

    /* Stop the thread first so s_out_cb can no longer reach the backend */
    rt_event_send(s_evt, EVT_EXIT);
    if (rt_sem_take(s_exit_sem, rt_tick_from_millisecond(3000)) != RT_EOK)
    {
        /* Thread still alive: release nothing, else the callback and static stack are used after free */
        LOG_E("audio thread exit timeout, skip cleanup");
        return;
    }

    {
        rt_tick_t deadline = rt_tick_get() + rt_tick_from_millisecond(1000);

        while (rt_object_get_type((rt_object_t)&s_thread) ==
                   RT_Object_Class_Thread &&
               (int32_t)(deadline - rt_tick_get()) > 0)
        {
            rt_thread_mdelay(1);
        }

        if (rt_object_get_type((rt_object_t)&s_thread) ==
            RT_Object_Class_Thread)
        {
            LOG_E("audio thread still alive, skip cleanup");
            return;
        }
    }

    mic_close();
    speaker_close();

    if (s_dec)
    {
        opus_decoder_destroy(s_dec);
        s_dec = RT_NULL;
    }
    if (s_enc)
    {
        opus_encoder_destroy(s_enc);
        s_enc = RT_NULL;
    }

    rt_event_delete(s_evt);
    rt_sem_delete(s_exit_sem);
    s_evt = RT_NULL;
    s_exit_sem = RT_NULL;
    s_out_cb = RT_NULL;
    s_out_user = RT_NULL;
    s_inited = 0;
}

/*
 * Hardware only, thread and Opus are untouched. Posted to the audio thread, so
 * it can be called from any thread.
 */
void xz_audio_mic_on(void)
{
    if (!s_inited || !s_evt)
        return;

    rt_event_send(s_evt, EVT_MIC_ON);
}

void xz_audio_mic_off(void)
{
    if (!s_inited || !s_evt)
        return;

    rt_event_send(s_evt, EVT_MIC_OFF);
}

void xz_audio_hw_off(void)
{
    if (!s_inited || !s_evt)
        return;

    rt_event_send(s_evt, EVT_HW_OFF);
}

void xz_audio_on_tts_audio(const uint8_t *data, size_t len)
{
    if (!s_inited || !data || len == 0 || len > XZ_PLAY_PKT_MAX)
        return;

    if ((s_play_wr + 1) % XZ_PLAY_Q_NUM == s_play_rd)
    {
        LOG_W("play queue full, drop %u bytes", (unsigned)len);
        return;
    }

    s_play_q[s_play_wr].len = (uint16_t)len;
    memcpy(s_play_q[s_play_wr].data, data, len);
    s_play_wr = (s_play_wr + 1) % XZ_PLAY_Q_NUM;

    rt_event_send(s_evt, EVT_PLAY);
}

/* ---------------- public API ---------------- */

int xz_audio_is_busy(void)
{
    if (!s_inited)
        return 0;

    if (xz_get_state() == XZ_STATE_LISTENING)
        return 1;
    if (s_play_rd != s_play_wr)
        return 1;

    return 0;
}
