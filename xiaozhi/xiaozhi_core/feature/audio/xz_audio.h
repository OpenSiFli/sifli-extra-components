/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __XZ_AUDIO_H__
#define __XZ_AUDIO_H__

#include <stdint.h>
#include <stddef.h>

#include "xiaozhi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Create the audio thread and the Opus encoder. No hardware is opened here:
 * the microphone is opened by xz_listen_start() and closed by xz_listen_stop(),
 * the speaker is opened when the first downlink audio frame arrives.
 * When out_cb is NULL the downlink PCM goes straight to the local speaker.
 */
int  xz_audio_ctx_open(xz_audio_out_cb_t out_cb, void *user);

/* Stop the thread and release Opus, used by xz_stop() */
void xz_audio_ctx_close(void);

/* Only toggles hardware, thread and codecs are untouched; runs on the audio
 * thread so it does not take effect immediately */
void xz_audio_mic_on(void);      /* open the microphone, start of recording */
void xz_audio_mic_off(void);     /* close the microphone, speaker untouched */
void xz_audio_hw_off(void);      /* close both microphone and speaker */

/* One downlink Opus frame, copied into the play queue right away (the caller's
 * buffer is no longer used) */
void xz_audio_on_tts_audio(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __XZ_AUDIO_H__ */
