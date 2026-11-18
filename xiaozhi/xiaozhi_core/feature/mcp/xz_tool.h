/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */


#ifndef __XZ_TOOL_H__
#define __XZ_TOOL_H__

#include <stdint.h>
#include <stddef.h>
#include "xiaozhi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XZ_TOOL_RESULT_MAX  512
#define XZ_TOOL_PENDING_MAX 4

typedef int (*xz_tool_send_fn)(const char *payload_json, void *user);

typedef struct
{
    char name[XZ_TOOL_NAME_MAX];
    char description[XZ_TOOL_DESC_MAX];
    char input_schema[XZ_TOOL_SCHEMA_MAX];
    xz_tool_cb_t cb;
    void *user;
    uint32_t timeout_ms;
} xz_tool_entry_t;

typedef struct
{
    xz_tool_entry_t tools[XZ_MAX_TOOLS];
    int count;

    xz_tool_send_fn send;
    void *send_user;

    struct
    {
        int used;
        int id_is_string;
        int id_int;
        char id_str[32];
        int tool;
    } pending[XZ_TOOL_PENDING_MAX];
} xz_tool_ctx_t;

void xz_tool_ctx_init(xz_tool_ctx_t *ctx, xz_tool_send_fn send, void *user);
int  xz_tool_register(xz_tool_ctx_t *ctx, const xz_tool_t *tool);
int  xz_tool_handle_mcp(xz_tool_ctx_t *ctx, const char *payload);
int  xz_tool_ctx_complete(xz_tool_ctx_t *ctx, uint32_t call_id, const char *result_text);

#ifdef __cplusplus
}
#endif

#endif /* __XZ_TOOL_H__ */
