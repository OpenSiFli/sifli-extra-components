/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * xz_tool: tool registry plus MCP JSON-RPC routing.
 *
 * The tests pin down what the component promises to the application: registry
 * constraints (capacity, duplicates, invalid arguments), the response shape of
 * all three methods, and both the synchronous and the asynchronous reply paths.
 */

#include <string.h>
#include <stdio.h>

#include "unity.h"
#include "cJSON.h"

#include "xz_tool.h"

/* The tool table costs about 5.6KB for XZ_MAX_TOOLS entries; keep it in .bss, off the main thread stack */
static xz_tool_ctx_t s_ctx;

/* Captures the JSON the component would hand to the server */
static char s_sent[2048];
static int s_sent_count;

static int capture_send(const char *payload_json, void *user)
{
    (void)user;

    s_sent_count++;
    if (payload_json)
    {
        strncpy(s_sent, payload_json, sizeof(s_sent) - 1);
        s_sent[sizeof(s_sent) - 1] = '\0';
    }
    return 0;
}

/* ---------------- application callbacks under test ---------------- */

static int sync_cb(const char *args_json, char *result, size_t result_size,
                   uint32_t call_id, void *user)
{
    (void)call_id;
    (void)user;

    rt_snprintf(result, result_size, "echo:%s", args_json ? args_json : "");
    return 0;
}

static int async_cb(const char *args_json, char *result, size_t result_size,
                    uint32_t call_id, void *user)
{
    (void)args_json;
    (void)result;
    (void)result_size;
    (void)call_id;
    (void)user;

    return XZ_TOOL_ASYNC;
}

static int fail_cb(const char *args_json, char *result, size_t result_size,
                   uint32_t call_id, void *user)
{
    (void)args_json;
    (void)result;
    (void)result_size;
    (void)call_id;
    (void)user;

    return -1;
}

static const xz_tool_t s_echo_tool =
{
    .name = "self.demo.echo",
    .description = "echo the arguments back",
    .input_schema = "{\"type\":\"object\",\"properties\":{}}",
    .callback = sync_cb,
};

static void ctx_reset(void)
{
    xz_tool_ctx_init(&s_ctx, capture_send, RT_NULL);
    s_sent[0] = '\0';
    s_sent_count = 0;
}

/* Pull a string field out of the last reply, NULL when absent */
static const char *sent_str(const char *key)
{
    cJSON *root;
    const char *value;
    static char out[256];

    root = cJSON_Parse(s_sent);
    if (!root)
        return NULL;

    value = cJSON_GetStringValue(cJSON_GetObjectItem(root, key));
    if (value)
    {
        strncpy(out, value, sizeof(out) - 1);
        out[sizeof(out) - 1] = '\0';
    }
    cJSON_Delete(root);
    return value ? out : NULL;
}

/* ---------------- registration ---------------- */

/* A successful registration bumps the tool count and the callback reaches the JSON-RPC layer */
void test_xz_tool_register_ok(void)
{
    ctx_reset();

    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_tool_register(&s_ctx, &s_echo_tool));
    TEST_ASSERT_EQUAL_INT(1, s_ctx.count);
    TEST_ASSERT_EQUAL_STRING("self.demo.echo", s_ctx.tools[0].name);
    TEST_ASSERT_EQUAL_STRING("echo the arguments back", s_ctx.tools[0].description);
}

/* Registering the same name twice must be rejected, otherwise tools/list reports it twice to the model */
void test_xz_tool_register_duplicate(void)
{
    ctx_reset();

    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_tool_register(&s_ctx, &s_echo_tool));
    TEST_ASSERT_EQUAL_INT(XZ_ERR_BUSY, xz_tool_register(&s_ctx, &s_echo_tool));
    TEST_ASSERT_EQUAL_INT(1, s_ctx.count);
}

/* A missing name or callback is a caller bug and must be rejected */
void test_xz_tool_register_invalid(void)
{
    xz_tool_t bad = s_echo_tool;

    ctx_reset();

    bad.name = NULL;
    TEST_ASSERT_EQUAL_INT(XZ_ERR_PARAM, xz_tool_register(&s_ctx, &bad));

    bad = s_echo_tool;
    bad.callback = NULL;
    TEST_ASSERT_EQUAL_INT(XZ_ERR_PARAM, xz_tool_register(&s_ctx, &bad));

    bad = s_echo_tool;
    bad.name = NULL;
    TEST_ASSERT_EQUAL_INT(XZ_ERR_PARAM, xz_tool_register(&s_ctx, NULL));

    TEST_ASSERT_EQUAL_INT(0, s_ctx.count);
}

/* The table holds XZ_MAX_TOOLS entries; entry N+1 must be an explicit error, not a silent drop */
void test_xz_tool_register_limit(void)
{
    xz_tool_t tool = s_echo_tool;
    char name[32];
    int i;

    ctx_reset();

    for (i = 0; i < XZ_MAX_TOOLS; i++)
    {
        rt_snprintf(name, sizeof(name), "self.test.t%d", i);
        tool.name = name;
        TEST_ASSERT_EQUAL_INT(XZ_OK, xz_tool_register(&s_ctx, &tool));
    }

    TEST_ASSERT_EQUAL_INT(XZ_MAX_TOOLS, s_ctx.count);

    tool.name = "self.test.overflow";
    TEST_ASSERT_EQUAL_INT(XZ_ERR_NO_MEM, xz_tool_register(&s_ctx, &tool));
    TEST_ASSERT_EQUAL_INT(XZ_MAX_TOOLS, s_ctx.count);
}

/* ---------------- JSON-RPC routing ---------------- */

/* initialize must report the protocol version and echo the id back unchanged */
void test_xz_tool_initialize(void)
{
    static const char *const req =
        "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"initialize\"}";

    ctx_reset();
    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_tool_handle_mcp(&s_ctx, req));
    TEST_ASSERT_EQUAL_INT(1, s_sent_count);

    TEST_ASSERT_EQUAL_STRING("2.0", sent_str("jsonrpc"));
    TEST_ASSERT_NOT_NULL(strstr(s_sent, "\"protocolVersion\""));
    TEST_ASSERT_NOT_NULL(strstr(s_sent, "\"id\":13"));
    TEST_ASSERT_NOT_NULL(strstr(s_sent, "\"tools\""));
}

/* tools/list must carry out every registered name and description */
void test_xz_tool_tools_list(void)
{
    static const char *const req =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}";

    ctx_reset();
    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_tool_register(&s_ctx, &s_echo_tool));
    s_sent_count = 0;

    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_tool_handle_mcp(&s_ctx, req));
    TEST_ASSERT_EQUAL_INT(1, s_sent_count);

    TEST_ASSERT_NOT_NULL(strstr(s_sent, "\"name\":\"self.demo.echo\""));
    TEST_ASSERT_NOT_NULL(strstr(s_sent, "\"description\":\"echo the arguments back\""));
    TEST_ASSERT_NOT_NULL(strstr(s_sent, "\"inputSchema\""));
}

/* Synchronous tool: the text filled in by the callback shows up in result.content[0].text */
void test_xz_tool_tools_call_sync(void)
{
    static const char *const req =
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"self.demo.echo\",\"arguments\":{\"x\":1}}}";
    cJSON *root;
    cJSON *content;
    cJSON *item;

    ctx_reset();
    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_tool_register(&s_ctx, &s_echo_tool));
    s_sent_count = 0;

    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_tool_handle_mcp(&s_ctx, req));
    TEST_ASSERT_EQUAL_INT(1, s_sent_count);

    root = cJSON_Parse(s_sent);
    TEST_ASSERT_NOT_NULL(root);

    content = cJSON_GetObjectItem(root, "result");
    TEST_ASSERT_NOT_NULL(content);
    content = cJSON_GetObjectItem(content, "content");
    TEST_ASSERT_NOT_NULL(content);
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(content));

    item = cJSON_GetArrayItem(content, 0);
    TEST_ASSERT_EQUAL_STRING("text",
                             cJSON_GetStringValue(cJSON_GetObjectItem(item, "type")));
    /* The callback receives the raw arguments text */
    TEST_ASSERT_EQUAL_STRING("echo:{\"x\":1}",
                             cJSON_GetStringValue(cJSON_GetObjectItem(item, "text")));

    cJSON_Delete(root);
}

/* Asynchronous tool: accepted with no reply, replied only from complete, and only once */
void test_xz_tool_tools_call_async(void)
{
    static const char *const req =
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"self.demo.slow\"}}";
    xz_tool_t slow = s_echo_tool;

    ctx_reset();
    slow.name = "self.demo.slow";
    slow.callback = async_cb;
    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_tool_register(&s_ctx, &slow));
    s_sent_count = 0;

    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_tool_handle_mcp(&s_ctx, req));
    TEST_ASSERT_EQUAL_INT(0, s_sent_count);         /* accepted, no reply yet */

    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_tool_ctx_complete(&s_ctx, 8, "done"));
    TEST_ASSERT_EQUAL_INT(1, s_sent_count);
    TEST_ASSERT_NOT_NULL(strstr(s_sent, "\"id\":8"));
    TEST_ASSERT_NOT_NULL(strstr(s_sent, "done"));

    /* The same call_id must not be replied to a second time */
    TEST_ASSERT_EQUAL_INT(XZ_ERR_STATE, xz_tool_ctx_complete(&s_ctx, 8, "again"));
    TEST_ASSERT_EQUAL_INT(1, s_sent_count);
}

/* A negative return from the callback must become a JSON-RPC error, not an empty result */
void test_xz_tool_tools_call_failed(void)
{
    static const char *const req =
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"self.demo.fail\"}}";
    xz_tool_t bad = s_echo_tool;

    ctx_reset();
    bad.name = "self.demo.fail";
    bad.callback = fail_cb;
    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_tool_register(&s_ctx, &bad));
    s_sent_count = 0;

    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_tool_handle_mcp(&s_ctx, req));
    TEST_ASSERT_EQUAL_INT(1, s_sent_count);
    TEST_ASSERT_NOT_NULL(strstr(s_sent, "\"error\""));
    TEST_ASSERT_NULL(strstr(s_sent, "\"result\""));
}

/* Unknown tool / missing name / unknown method: three distinct error codes, not one */
void test_xz_tool_errors(void)
{
    static const char *const unknown_tool =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"no.such.tool\"}}";
    static const char *const missing_name =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{}}";
    static const char *const unknown_method =
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/does_not_exist\"}";
    static const char *const missing_method = "{\"id\":4}";

    ctx_reset();

    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_tool_handle_mcp(&s_ctx, unknown_tool));
    TEST_ASSERT_NOT_NULL(strstr(s_sent, "-32601"));

    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_tool_handle_mcp(&s_ctx, missing_name));
    TEST_ASSERT_NOT_NULL(strstr(s_sent, "-32602"));

    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_tool_handle_mcp(&s_ctx, unknown_method));
    TEST_ASSERT_NOT_NULL(strstr(s_sent, "-32601"));

    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_tool_handle_mcp(&s_ctx, missing_method));
    TEST_ASSERT_NOT_NULL(strstr(s_sent, "-32600"));
}

/* Malformed JSON returns an error code without crashing; notification-style messages produce no reply */
void test_xz_tool_malformed_and_notification(void)
{
    static const char *const notify =
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";

    ctx_reset();

    TEST_ASSERT_EQUAL_INT(XZ_ERR_PROTOCOL, xz_tool_handle_mcp(&s_ctx, "not json"));
    TEST_ASSERT_EQUAL_INT(0, s_sent_count);

    TEST_ASSERT_EQUAL_INT(XZ_OK, xz_tool_handle_mcp(&s_ctx, notify));
    TEST_ASSERT_EQUAL_INT(0, s_sent_count);
}
