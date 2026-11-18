/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <rtthread.h>
#include <cJSON.h>

#include "xz_tool.h"

#define LOG_TAG "xz_tool"
#include "log.h"

typedef struct
{
    int is_string;
    int num;
    char str[32];
} rpc_id_t;

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

static void read_id(cJSON *root, rpc_id_t *id)
{
    cJSON *item = cJSON_GetObjectItem(root, "id");

    memset(id, 0, sizeof(*id));
    if (!item)
    {
        id->is_string = 1;
        strcpy(id->str, "0");
        return;
    }
    if (cJSON_IsString(item))
    {
        id->is_string = 1;
        copy_str(id->str, sizeof(id->str), item->valuestring);
    }
    else if (cJSON_IsNumber(item))
    {
        id->num = item->valueint;
    }
}

static void add_id(cJSON *root, const rpc_id_t *id)
{
    if (id->is_string)
        cJSON_AddStringToObject(root, "id", id->str);
    else
        cJSON_AddNumberToObject(root, "id", id->num);
}

static uint32_t id_to_call_id(const rpc_id_t *id)
{
    uint32_t h = 0;
    const char *p;

    if (!id->is_string)
        return (uint32_t)id->num;

    for (p = id->str; *p; p++)
        h = h * 31u + (uint8_t)*p;
    return h;
}

static void send_json(xz_tool_ctx_t *ctx, cJSON *root)
{
    char *text;

    if (!ctx || !root)
        return;

    text = cJSON_PrintUnformatted(root);
    if (text)
    {
        if (ctx->send)
            ctx->send(text, ctx->send_user);
        cJSON_free(text);
    }
}

static void send_result(xz_tool_ctx_t *ctx, const rpc_id_t *id, cJSON *result)
{
    cJSON *root = cJSON_CreateObject();

    if (!root)
    {
        if (result)
            cJSON_Delete(result);
        return;
    }

    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    add_id(root, id);
    if (result)
        cJSON_AddItemToObject(root, "result", result);
    send_json(ctx, root);
    cJSON_Delete(root);
}

static void send_error(xz_tool_ctx_t *ctx, const rpc_id_t *id,
                       int code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *error;

    if (!root)
        return;

    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    add_id(root, id);
    error = cJSON_AddObjectToObject(root, "error");
    if (error)
    {
        cJSON_AddNumberToObject(error, "code", code);
        cJSON_AddStringToObject(error, "message", message ? message : "error");
    }
    send_json(ctx, root);
    cJSON_Delete(root);
}

static cJSON *tool_result_content(const char *text)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *content;

    if (!result)
        return NULL;

    content = cJSON_AddArrayToObject(result, "content");
    if (content)
    {
        cJSON *item = cJSON_CreateObject();
        if (item)
        {
            cJSON_AddStringToObject(item, "type", "text");
            cJSON_AddStringToObject(item, "text", text ? text : "");
            cJSON_AddItemToArray(content, item);
        }
    }
    return result;
}

static int find_tool(xz_tool_ctx_t *ctx, const char *name)
{
    int i;

    if (!ctx || !name)
        return -1;
    for (i = 0; i < ctx->count; i++)
    {
        if (strcmp(ctx->tools[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int pending_alloc(xz_tool_ctx_t *ctx, const rpc_id_t *id, int tool)
{
    int i;

    for (i = 0; i < XZ_TOOL_PENDING_MAX; i++)
    {
        if (!ctx->pending[i].used)
        {
            ctx->pending[i].used = 1;
            ctx->pending[i].id_is_string = id->is_string;
            ctx->pending[i].id_int = id->num;
            copy_str(ctx->pending[i].id_str, sizeof(ctx->pending[i].id_str), id->str);
            ctx->pending[i].tool = tool;
            return i;
        }
    }
    return -1;
}

void xz_tool_ctx_init(xz_tool_ctx_t *ctx, xz_tool_send_fn send, void *user)
{
    if (!ctx)
        return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->send = send;
    ctx->send_user = user;
}

int xz_tool_register(xz_tool_ctx_t *ctx, const xz_tool_t *tool)
{
    xz_tool_entry_t *dst;

    if (!ctx || !tool || !tool->name || !tool->callback)
        return XZ_ERR_PARAM;
    if (ctx->count >= XZ_MAX_TOOLS)
        return XZ_ERR_NO_MEM;
    if (find_tool(ctx, tool->name) >= 0)
        return XZ_ERR_BUSY;
    if (strlen(tool->name) >= XZ_TOOL_NAME_MAX)
        return XZ_ERR_PARAM;
    if (tool->description && strlen(tool->description) >= XZ_TOOL_DESC_MAX)
        return XZ_ERR_PARAM;
    if (tool->input_schema && strlen(tool->input_schema) >= XZ_TOOL_SCHEMA_MAX)
        return XZ_ERR_PARAM;

    dst = &ctx->tools[ctx->count];
    memset(dst, 0, sizeof(*dst));
    copy_str(dst->name, sizeof(dst->name), tool->name);
    copy_str(dst->description, sizeof(dst->description), tool->description);
    copy_str(dst->input_schema, sizeof(dst->input_schema), tool->input_schema);
    dst->cb = tool->callback;
    dst->user = tool->user;
    dst->timeout_ms = tool->timeout_ms;

    ctx->count++;
    LOG_I("tool registered: %s", dst->name);
    return XZ_OK;
}

static void handle_initialize(xz_tool_ctx_t *ctx, const rpc_id_t *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *caps;
    cJSON *info;

    if (!result)
        return;

    cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
    caps = cJSON_AddObjectToObject(result, "capabilities");
    if (caps)
        cJSON_AddObjectToObject(caps, "tools");
    info = cJSON_AddObjectToObject(result, "serverInfo");
    if (info)
    {
        cJSON_AddStringToObject(info, "name", "xiaozhi-core");
        cJSON_AddStringToObject(info, "version", "0.1.0");
    }
    send_result(ctx, id, result);
}

static void handle_tools_list(xz_tool_ctx_t *ctx, const rpc_id_t *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *tools;
    int i;

    if (!result)
        return;

    tools = cJSON_AddArrayToObject(result, "tools");
    for (i = 0; tools && i < ctx->count; i++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON *schema;

        if (!item)
            continue;
        cJSON_AddStringToObject(item, "name", ctx->tools[i].name);
        cJSON_AddStringToObject(item, "description", ctx->tools[i].description);
        schema = ctx->tools[i].input_schema[0] ?
                 cJSON_Parse(ctx->tools[i].input_schema) : NULL;
        if (!schema)
        {
            schema = cJSON_CreateObject();
            if (schema)
                cJSON_AddStringToObject(schema, "type", "object");
        }
        if (schema)
            cJSON_AddItemToObject(item, "inputSchema", schema);
        cJSON_AddItemToArray(tools, item);
    }
    send_result(ctx, id, result);
}

static void handle_tools_call(xz_tool_ctx_t *ctx, const rpc_id_t *id, cJSON *params)
{
    const char *name = get_str(params, "name");
    cJSON *args = params ? cJSON_GetObjectItem(params, "arguments") : NULL;
    char *args_str = NULL;
    char result[XZ_TOOL_RESULT_MAX];
    int idx;
    int ret;
    uint32_t call_id;

    if (!name)
    {
        send_error(ctx, id, -32602, "missing tool name");
        return;
    }

    idx = find_tool(ctx, name);
    if (idx < 0)
    {
        send_error(ctx, id, -32601, "unknown tool");
        return;
    }

    if (args)
    {
        args_str = cJSON_PrintUnformatted(args);
        if (!args_str)
        {
            send_error(ctx, id, -32603, "no memory");
            return;
        }
    }

    LOG_I("tool call: %s(%s)", name, args_str ? args_str : "{}");

    memset(result, 0, sizeof(result));
    call_id = id_to_call_id(id);
    ret = ctx->tools[idx].cb(args_str ? args_str : "{}",
                             result, sizeof(result),
                             call_id, ctx->tools[idx].user);
    if (args_str)
        cJSON_free(args_str);

    if (ret != XZ_TOOL_ASYNC && ret >= 0)
        LOG_I("tool result: %s", result);

    if (ret == XZ_TOOL_ASYNC)
    {
        if (pending_alloc(ctx, id, idx) < 0)
            send_error(ctx, id, -32603, "too many pending calls");
        return;
    }

    if (ret < 0)
    {
        send_error(ctx, id, -32603, "tool execution failed");
        return;
    }

    send_result(ctx, id, tool_result_content(result));
}

int xz_tool_handle_mcp(xz_tool_ctx_t *ctx, const char *payload)
{
    cJSON *root;
    const char *method;
    rpc_id_t id;
    cJSON *params;

    if (!ctx || !payload)
        return XZ_ERR_PARAM;

    root = cJSON_Parse(payload);
    if (!root)
        return XZ_ERR_PROTOCOL;

    method = get_str(root, "method");
    read_id(root, &id);
    params = cJSON_GetObjectItem(root, "params");

    if (!method)
    {
        send_error(ctx, &id, -32600, "missing method");
    }
    else if (strcmp(method, "initialize") == 0)
    {
        handle_initialize(ctx, &id);
    }
    else if (strcmp(method, "tools/list") == 0)
    {
        handle_tools_list(ctx, &id);
    }
    else if (strcmp(method, "tools/call") == 0)
    {
        handle_tools_call(ctx, &id, params);
    }
    else if (strcmp(method, "notifications/initialized") == 0)
    {
        /* notifications produce no response */
    }
    else if (strcmp(method, "ping") == 0)
    {
        send_result(ctx, &id, cJSON_CreateObject());
    }
    else
    {
        send_error(ctx, &id, -32601, "method not found");
    }

    cJSON_Delete(root);
    return XZ_OK;
}

int xz_tool_ctx_complete(xz_tool_ctx_t *ctx, uint32_t call_id, const char *result_text)
{
    int i;

    if (!ctx)
        return XZ_ERR_PARAM;

    for (i = 0; i < XZ_TOOL_PENDING_MAX; i++)
    {
        rpc_id_t id;
        uint32_t cid;

        if (!ctx->pending[i].used)
            continue;

        memset(&id, 0, sizeof(id));
        id.is_string = ctx->pending[i].id_is_string;
        id.num = ctx->pending[i].id_int;
        copy_str(id.str, sizeof(id.str), ctx->pending[i].id_str);

        cid = id_to_call_id(&id);
        if (cid != call_id)
            continue;

        ctx->pending[i].used = 0;
        send_result(ctx, &id, tool_result_content(result_text));
        return XZ_OK;
    }

    return XZ_ERR_STATE;
}
