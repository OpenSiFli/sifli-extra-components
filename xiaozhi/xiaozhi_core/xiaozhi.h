/**
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __XIAOZHI_H__
#define __XIAOZHI_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XZ_VERSION_STR      "0.1.0"

#ifndef PKG_USING_XIAOZHI

#ifndef XIAOZHI_FEATURE_MCP
    #define XIAOZHI_FEATURE_MCP       1
#endif
#endif /* !PKG_USING_XIAOZHI */

#ifndef XIAOZHI_FEATURE_AUDIO
    #define XIAOZHI_FEATURE_AUDIO     0
#endif /* XIAOZHI_FEATURE_AUDIO */

#ifndef XIAOZHI_THREAD_STACK_SIZE
    #define XIAOZHI_THREAD_STACK_SIZE 12288
#endif /* XIAOZHI_THREAD_STACK_SIZE */

/** Maximum number of tools that can be registered */
#if XIAOZHI_FEATURE_MCP
    #define XZ_MAX_TOOLS            8
    #define XZ_TOOL_NAME_MAX        48
    #define XZ_TOOL_DESC_MAX        128
    #define XZ_TOOL_SCHEMA_MAX      512
#endif /* XIAOZHI_FEATURE_MCP */

/** Downlink defaults used when hello carries no value */
#define XZ_DEFAULT_SAMPLE_RATE  24000
#define XZ_DEFAULT_FRAME_MS     60

/** Uplink audio format, fixed at 16 kHz / 60 ms */
#define XZ_UPLINK_SAMPLE_RATE   16000
#define XZ_UPLINK_FRAME_MS      60

typedef struct xz_t xz_t;

/** Common return codes: 0 on success, negative on failure */
typedef enum
{
    XZ_OK = 0,                  /**< Success */
    XZ_ERR_PARAM = -1,          /**< Invalid or missing argument */
    XZ_ERR_STATE = -2,          /**< Operation not allowed in the current state */
    XZ_ERR_NO_MEM = -3,         /**< Out of memory */
    XZ_ERR_NET = -4,            /**< Network send or receive failed */
    XZ_ERR_AUTH = -5,           /**< Authentication failed */
    XZ_ERR_PROTOCOL = -6,       /**< Protocol parse error or bad server response */
    XZ_ERR_TIMEOUT = -7,        /**< Timeout */
    XZ_ERR_UNSUPPORTED = -8,    /**< Not supported by this build or transport */
    XZ_ERR_BUSY = -9,           /**< Resource busy */
    XZ_ERR_INTERNAL = -10,      /**< Internal error */
    XZ_ERR_BUF = -11,           /**< Internal buffer too small for the message */
} xz_err_t;

/** Component state, see xz_get_state() */
typedef enum
{
    XZ_STATE_INIT = 0,          /**< Not started, or started but not yet linking */
    XZ_STATE_BOOTSTRAP,         /**< Fetching link parameters */
    XZ_STATE_CONNECTING,        /**< Linking, waiting for hello */
    XZ_STATE_IDLE,              /**< Connected and idle */
    XZ_STATE_LISTENING,         /**< Listening, waiting for server recognition */
    XZ_STATE_SPEAKING,          /**< Playing back the reply */
    XZ_STATE_PAUSED,            /**< Disconnected, waiting for xz_reconnect() */
    XZ_STATE_CLOSED,            /**< Stopped */
} xz_state_t;

/** Listen mode, maps to listen.mode in the protocol */
typedef enum
{
    XZ_MODE_AUTO = 0,           /**< Server VAD decides when speech ends */
    XZ_MODE_MANUAL,             /**< Device ends it with xz_listen_stop() */
    XZ_MODE_REALTIME,           /**< Full-duplex audio, needs device-side AEC */
} xz_listen_mode_t;

/** Abort reason, maps to abort.reason in the protocol */
typedef enum
{
    XZ_ABORT_USER = 0,          /**< Interrupted by the user */
    XZ_ABORT_WAKE_WORD,         /**< Interrupted by a wake word */
} xz_abort_reason_t;

/** Event types reported through on_event */
typedef enum
{
    XZ_EVT_CONNECTING = 0,      /**< About to attempt a connection */
    XZ_EVT_CONNECTED,           /**< Hello received, connection is usable */
    XZ_EVT_DISCONNECTED,        /**< Disconnected, see u.disconnect.reason */
    XZ_EVT_ACTIVATION_REQUIRED, /**< Needs activation, code in u.activation.code */
    XZ_EVT_SESSION_STARTED,     /**< Session started, precedes CONNECTED */
    XZ_EVT_SESSION_ENDED,       /**< Server sent goodbye */
    XZ_EVT_ASR_FINAL,           /**< Speech recognition result, see u.text.text */
    XZ_EVT_TTS_START,           /**< Playback started */
    XZ_EVT_TTS_SENTENCE,        /**< One sentence to play, see u.text.text */
    XZ_EVT_TTS_STOP,            /**< Playback finished */
    XZ_EVT_EMOTION,             /**< Emotion label, see u.text.text */
} xz_event_type_t;

/** Disconnect reasons, carried in u.disconnect.reason of XZ_EVT_DISCONNECTED */
typedef enum
{
    XZ_DISCONNECT_NET = 0,      /**< Link dropped or send/receive failed */
    XZ_DISCONNECT_AUTH,         /**< Authentication failed */
    XZ_DISCONNECT_SERVER,       /**< Rejected by the server */
    XZ_DISCONNECT_TIMEOUT,      /**< Timed out waiting for hello */
} xz_disconnect_reason_t;

/** Event payload, pick the union member matching type */
typedef struct
{
    xz_event_type_t type;
    union
    {
        struct { const char *text; } text;                            /**< ASR_FINAL / TTS_SENTENCE / EMOTION */
        struct { const char *code; const char *message; } activation; /**< ACTIVATION_REQUIRED */
        struct { int reason; } disconnect;                            /**< DISCONNECTED */
    } u;
} xz_event_t;

/** Event callback, runs on the component worker thread */
typedef void (*xz_event_cb_t)(const xz_event_t *ev, void *user);

#if XIAOZHI_FEATURE_AUDIO

/**
 * @brief  Downlink PCM callback, already decoded to 16-bit mono
 * @param  pcm         Sample buffer, invalid once the callback returns
 * @param  samples     Number of samples (bytes = samples * 2)
 * @param  sample_rate Downlink sample rate from hello, usually 24000
 * @note   Runs on the audio thread and may block
 */
typedef void (*xz_audio_out_cb_t)(const int16_t *pcm, size_t samples,
                                  uint32_t sample_rate, void *user);

#endif /* XIAOZHI_FEATURE_AUDIO */

/* ==================== Device capabilities (MCP tools) ==================== */
#if XIAOZHI_FEATURE_MCP

/** Tool callback return value meaning "accepted, will reply later" */
#define XZ_TOOL_ASYNC 1

/**
 * @brief  Tool execution callback
 * @param  args_json   Arguments from the server as a JSON object, "{}" when empty
 * @param  result      Buffer receiving the result text sent back to the server
 * @param  result_size Size of result
 * @param  call_id     Call identifier, pass it to xz_tool_complete() for async
 * @return 0 done synchronously; XZ_TOOL_ASYNC accepted; <0 failed
 */
typedef int (*xz_tool_cb_t)(const char *args_json,
                            char *result, size_t result_size,
                            uint32_t call_id, void *user);

/** Tool description, all tools are registered at once through xz_config_t.tools */
typedef struct
{
    const char *name;           /**< Tool name, namespaced like "self.demo.get_version" */
    const char *description;    /**< Hint telling the LLM when to call this tool */
    const char *input_schema;   /**< Argument JSON schema, NULL when it takes none */
    xz_tool_cb_t callback;      /**< Execution callback */
    void *user;                 /**< Context passed back to the callback */
    uint32_t timeout_ms;        /**< Reserved, currently unused */
} xz_tool_t;

/**
 * @brief Send the result of an asynchronous tool call
 * @param call_id     Identifier received by the tool callback
 * @param result_text Result text sent back to the server
 */
int xz_tool_complete(uint32_t call_id, const char *result_text);

#endif /* XIAOZHI_FEATURE_MCP */

/** Component configuration, string fields are copied during xz_start() */
typedef struct
{
    const char *device_id;          /**< [required] Device MAC, used as Device-Id */
    const char *client_id;          /**< [required] Client UUID, clearing it re-activates */
    const char *model;              /**< Product model, reported to the OTA API */
    const char *firmware_version;   /**< Firmware version, reported to the OTA API */
    const char *server_url;         /**< Server address, default https://api.tenclass.net */

    uint32_t timeout_bootstrap_ms;  /**< Bootstrap timeout, default 10000 */
    uint32_t timeout_connect_ms;    /**< Timeout waiting for hello, default 15000 */

#if XIAOZHI_FEATURE_MCP
    const xz_tool_t *tools;         /**< Optional, tools registered once at start */
    uint8_t tool_count;
#endif /* XIAOZHI_FEATURE_MCP */

#if XIAOZHI_FEATURE_AUDIO
    xz_audio_out_cb_t audio_out_cb; /**< Optional TTS sink, NULL uses the local speaker */
#endif /* XIAOZHI_FEATURE_AUDIO */

    xz_event_cb_t on_event;         /**< Event callback, may be NULL */
    void *user;                     /**< Context passed to on_event and audio_out_cb */
} xz_config_t;

/* ==================== Lifecycle ==================== */

/**
 * @brief  Start the component: create instance, thread, bootstrap, then link
 * @param  cfg Configuration, strings are copied and may be freed after return
 * @return XZ_OK; XZ_ERR_PARAM bad config; XZ_ERR_STATE already running; XZ_ERR_NO_MEM
 */
int xz_start(const xz_config_t *cfg);

/**
 * @brief  Stop the component and release every resource
 * @note   Waits up to 5 seconds for the worker thread, then kills it
 * @return Always XZ_OK, safe to call when not started
 */
int xz_stop(void);

/* ==================== Conversation ==================== */
/* All functions below require a live connection, otherwise they return XZ_ERR_STATE */

/**
 * @brief  Send a text message, the server goes straight to LLM -> TTS
 * @param  text Up to 511 bytes, longer input is truncated; state is unchanged
 */
int xz_send_text(const char *text);

/**
 * @brief  Abort the current playback, link and session are kept
 * @param  reason Why it was aborted
 */
int xz_abort(xz_abort_reason_t reason);

/* ==================== Audio (XIAOZHI_FEATURE_AUDIO) ==================== */
/* The whole audio path (capture, Opus encode/decode, playback) is handled by the
 * component and board differences live in Kconfig. The microphone is owned by the
 * component and only powered while listening, do not open a second stream. */
#if XIAOZHI_FEATURE_AUDIO

/**
 * @brief Start listening, state becomes LISTENING
 * @param mode Listen mode
 */
int xz_listen_start(xz_listen_mode_t mode);

/**
 * @brief Stop listening, state becomes IDLE
 * @note  This means "I finished speaking", use xz_abort() to stop playback
 */
int xz_listen_stop(void);

/**
 * @brief Whether audio is in use: listening, or downlink audio still playing
 * @note  Does not account for the DAC buffer, a short tail may remain after 0
 */
int xz_audio_is_busy(void);

#endif /* XIAOZHI_FEATURE_AUDIO */

/* ==================== Status ==================== */

/** @brief Current state, callable at any time; XZ_STATE_CLOSED when not started */
xz_state_t xz_get_state(void);

/**
 * @brief Audio parameters negotiated in hello
 * @param sample_rate    May be NULL
 * @param frame_duration May be NULL, in milliseconds
 */
int xz_get_negotiated(uint32_t *sample_rate, uint32_t *frame_duration);

/** @brief Current session id, changes on every reconnect; empty when not connected */
const char *xz_get_session_id(void);

/* ==================== Activation ==================== */

/**
 * @brief Confirm that web activation is done
 * @note  While waiting, the component blocks up to 5 minutes; calling this lets it continue
 */
int xz_activation_confirm(void);

/* ==================== Connection control ==================== */

/**
 * @brief Disconnect the link and power down microphone and speaker, state becomes PAUSED
 * @note  Threads and Opus contexts are kept so xz_reconnect() is fast.
 *        Asynchronous: the link may still be closing when this returns.
 *        The server drops the session, conversation context is lost.
 */
int xz_disconnect(void);

/**
 * @brief Bootstrap again, link, then wait for hello
 * @note  The microphone is not opened here, xz_listen_start() is still required.
 *        A new session is created and context is not preserved.
 *        Repeated calls result in a single reconnect.
 */
int xz_reconnect(void);

#ifdef __cplusplus
}
#endif

#endif /* __XIAOZHI_H__ */
