# xiaozhi core 组件

## 1. 组件功能介绍

把小智（xiaozhi.me）云端对话业务封装成一个可复用的组件，产品侧只需要调用简单的接口就可以轻松把小智业务用起来，组件自带拓展功能例如MCP，音频等能力，用户可自由裁剪。


### 1.1 基础功能

| 功能 | 用起来是什么效果 |
|---|---|
| 设备激活 | 首次使用给用户一个绑定码，到 xiaozhi.me 绑定后即可使用 |
| 文本对话 | 发一段文字，服务端理解后给出回答 |
| 打断 | 播报过程中随时可以打断 |
| 对话回调 | 识别到的文字、每句播报内容、情绪标签都回调给你，可以显示到屏幕或切表情 |
| 断线重连 | 网络恢复后调一个接口就能重新连上小智，不用重启设备 |


### 1.2 扩展功能（按需开启）

| 功能 | 开关 | 用起来是什么效果 |
|---|---|---|
| 语音对话 | `CONFIG_XIAOZHI_FEATURE_AUDIO` | 支持与小智语音交流 |
| 设备工具 | `CONFIG_XIAOZHI_FEATURE_MCP` | 支持提供工具给小智使用 |
| 调试命令 | `CONFIG_XIAOZHI_USING_MSH` | 串口敲命令验证功能 |

每个功能独立，应用可按需裁剪。

### 1.3 应用使用小智组件需要负责什么

- **让板子先能上网**。组件不联网，`xz_start()` 之前必须已经拿到 IP。
  蓝牙 PAN、WiFi 都行，组件不关心。
- **写产品逻辑**。什么时候发起对话、按键怎么响应、回调内容怎么显示，都由应用决定。
- **网络恢复后调一次 `xz_reconnect()`**。组件不会自己重连。

### 1.4 目录结构

```
xiaozhi/
├── xiaozhi.h                 # 对外头文件，业务代码只需要 include 这个
├── xiaozhi.c                 # 组件实现
├── core/                     # 基础部分，开了组件就会编译
│   ├── channel/              #   WebSocket / MQTT 传输，编译期二选一
│   └── proto/                #   协议处理 + 建链参数获取
├── feature/                  # 扩展部分，一个子目录对应一个开关
│   ├── audio/                #   语音对话（CONFIG_XIAOZHI_FEATURE_AUDIO）
│   ├── mcp/                  #   设备工具（CONFIG_XIAOZHI_FEATURE_MCP）
│   └── shell/                #   调试命令（CONFIG_XIAOZHI_USING_MSH）
├── Kconfig
└── SConscript
```

---

## 2. 组件对外提供的 API 介绍

业务代码只需要 `#include "xiaozhi.h"`。

### 2.1 配置结构体 `xz_config_t`

启动前填好这个结构体，只有前两个字段是必填的。

```c
typedef struct
{
    const char *device_id;          /**< 【必填】设备 MAC */
    const char *client_id;          /**< 【必填】设备唯一 UUID，绑定关系靠它区分 */
    const char *model;              /**< 产品型号，可填 NULL */
    const char *firmware_version;   /**< 固件版本，可填 NULL */
    const char *server_url;         /**< 服务端地址，留 NULL 用官方地址 */

    uint32_t timeout_bootstrap_ms;  /**< 建链参数获取超时，默认 10000 */
    uint32_t timeout_connect_ms;    /**< 连接超时，默认 15000 */

    xz_event_cb_t on_event;         /**< 事件回调，见 2.6 */
    void *user;                     /**< 透传给回调的上下文 */

    /* 下面两个字段只在对应开关打开时才存在 */
    const xz_tool_t *tools;         /**< 设备工具表，见 3.3 */
    uint8_t tool_count;
    xz_audio_out_cb_t audio_out_cb; /**< 留 NULL 用本机喇叭，见 3.2 */
} xz_config_t;
```

字符串字段在 `xz_start()` 里会被拷一份，函数返回后你可以释放。

### 2.2 启动与停止

| 函数 | 用途 | 注意事项 |
|---|---|---|
| `int xz_start(const xz_config_t *cfg)` | 启动组件 | 返回 `0` 只代表流程启动了，真正可用要等 `XZ_EVT_CONNECTED` 事件 |
| `int xz_stop(void)` | 停止并释放资源 | 之后要用得重新 `xz_start()` |


### 2.3 文本对话

| 函数 | 用途 |
|---|---|
| `int xz_send_text(const char *text)` | 发一段文字，服务端理解后返回回答 |
| `int xz_abort(xz_abort_reason_t reason)` | 打断正在进行的播报 |

`text` 最长 511 字节。`reason` 填 `XZ_ABORT_USER`（用户主动打断）。

两个函数都要求已经连上服务器，否则返回 `-2`。

### 2.4 状态查询

| 函数 | 用途 |
|---|---|
| `xz_state_t xz_get_state(void)` | 查询小智当前状态 |
| `const char *xz_get_session_id(void)` | 查询当前会话 ID |
| `int xz_get_negotiated(uint32_t *rate, uint32_t *frame_ms)` | 查询服务端协商的音频参数，出参可传 NULL |
| `int xz_activation_confirm(void)` | 用户完成网页绑定后调，让组件立即继续 |

### 2.5 连接管理

| 函数 | 用途 |
|---|---|
| `int xz_disconnect(void)` | 断开连接，保留资源，等网络恢复后可以快速重连 |
| `int xz_reconnect(void)` | 重新连接，网络恢复后调这个 |


### 2.6 小智事件

| 事件 | 什么时候来 | 能取到什么 |
|---|---|---|
| `XZ_EVT_CONNECTING` | 开始连接 | — |
| `XZ_EVT_CONNECTED` | 连接成功，可以发指令了 | — |
| `XZ_EVT_DISCONNECTED` | 连接断开 | `u.disconnect.reason` |
| `XZ_EVT_ACTIVATION_REQUIRED` | 设备需要绑定 | `u.activation.code`，显示给用户 |
| `XZ_EVT_SESSION_STARTED` | 会话建立 | — |
| `XZ_EVT_SESSION_ENDED` | 会话结束 | — |
| `XZ_EVT_ASR_FINAL` | 识别出用户说了什么 | `u.text.text`，可显示到屏幕 |
| `XZ_EVT_TTS_START` | 开始播报 | — |
| `XZ_EVT_TTS_SENTENCE` | 这一句要播报的内容 | `u.text.text`，可同步显示字幕 |
| `XZ_EVT_TTS_STOP` | 播报结束 | — |
| `XZ_EVT_EMOTION` | 情绪标签 | `u.text.text`（`happy` / `sad` / …），可切表情 |

### 2.7 返回值

`0` 表示成功，负值表示失败。实际开发中最常见的两个：

- `-2`（`XZ_ERR_STATE`）：还没连上服务器就调了接口
- `-4`（`XZ_ERR_NET`）：网络发不出去

完整错误码定义见 `xiaozhi.h` 里的 `xz_err_t`。

### 2.8 扩展 API：语音控制（需开 `CONFIG_XIAOZHI_FEATURE_AUDIO`）

| 函数 | 用途 |
|---|---|
| `int xz_listen_start(xz_listen_mode_t mode)` | 开始录音，用户按下按键时调 |
| `int xz_listen_stop(void)` | 结束录音，用户松开按键时调 |
| `int xz_audio_is_busy(void)` | 是否正在说话或播报，可以用来做 UI 状态 |

`mode` 填 `XZ_MODE_MANUAL`：由你的按键决定什么时候结束。目前组件还不支持 `XZ_MODE_REALTIME` 功能。

> 注意 `xz_listen_stop()` 是「我说完了」，不是「你别说了」。
> 要让板子停止播报请用 `xz_abort()`。

### 2.9 扩展 API：工具回填（需开 `CONFIG_XIAOZHI_FEATURE_MCP`）

| 函数 | 用途 |
|---|---|
| `int xz_tool_complete(uint32_t call_id, const char *result_text)` | 异步工具执行完后补结果 |

同步工具不需要它，只有回调返回了 `XZ_TOOL_ASYNC` 时才用，详见 3.3。

---

## 3. 组件的使用方法

### 3.0 接入准备（最小配置）

**1) 打开依赖和组件开关**，加到工程的 `proj.conf`：

```
# 组件依赖
CONFIG_RT_USING_LWIP=y
CONFIG_RT_USING_NETDEV=y
CONFIG_PKG_USING_CJSON=y
CONFIG_PKG_USING_WEBCLIENT=y
CONFIG_PKG_USING_MBEDTLS=y
CONFIG_RT_LWIP_USING_WEBSOCKET=y

# 打开组件（最小配置，不含任何扩展）
CONFIG_PKG_USING_XIAOZHI=y
CONFIG_XIAOZHI_TRANSPORT_WS=y
```


**2) 确认网络可用**：组件不管联网，`xz_start()` 之前板子必须已经拿到 IP。

### 3.1 最小可用代码（纯文本对话）

这一步只有基础功能：连上服务器、发文字、收回调。不带音频，不带工具。

```c
/*
 * 小智组件最小接入示例
 *
 * 依赖：
 *   CONFIG_PKG_USING_XIAOZHI=y
 *   CONFIG_XIAOZHI_TRANSPORT_WS=y
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>
#include <stdio.h>
#include <string.h>

#include "xiaozhi.h"

#define LOG_TAG "app"
#include "log.h"

/* ---------------- 1. 设备身份 ---------------- */
#define APP_DEVICE_ID       "a0:b1:c2:d3:e4:f5"     /* 换成设备真实 MAC */
#define APP_CLIENT_ID       "28add114-1f77-e226-26da-340821b70854"
#define APP_MODEL           "my-product"
#define APP_FW_VERSION      "1.0.0"

static volatile int s_connected;    /* 没连上时不要调对话接口 */

/* ---------------- 2. 事件回调 ---------------- */
/* 在组件线程里执行，不要做耗时操作 */
static void on_xz_event(const xz_event_t *ev, void *user)
{
    (void)user;

    switch (ev->type)
    {
    case XZ_EVT_CONNECTED:
        s_connected = 1;
        LOG_I("connected, session=%s", xz_get_session_id());
        break;

    case XZ_EVT_DISCONNECTED:
        s_connected = 0;
        LOG_W("disconnected, reason=%d", ev->u.disconnect.reason);
        /* 网络恢复后由应用层调 xz_reconnect() */
        break;

    case XZ_EVT_ACTIVATION_REQUIRED:
        /* 把这个码显示给用户，让他到 xiaozhi.me 输入 */
        LOG_W("activation code: %s", ev->u.activation.code);
        break;

    case XZ_EVT_ASR_FINAL:
        LOG_I("asr: %s", ev->u.text.text);
        break;

    case XZ_EVT_TTS_SENTENCE:
        /* 这一句要播报的内容，可以同步显示成字幕 */
        LOG_I("tts: %s", ev->u.text.text);
        break;

    case XZ_EVT_TTS_STOP:
        LOG_I("tts stop");
        break;

    case XZ_EVT_EMOTION:
        /* happy / sad / angry ... 可以用来切表情 */
        LOG_I("emotion: %s", ev->u.text.text);
        break;

    default:
        break;
    }
}

/* ---------------- 3. 启动与停止 ---------------- */
int app_xiaozhi_start(void)
{
    xz_config_t cfg;
    int ret;

    /* 已经在跑就不重复初始化，走重连 */
    if (xz_get_state() != XZ_STATE_CLOSED)
        return xz_reconnect();

    memset(&cfg, 0, sizeof(cfg));
    cfg.device_id        = APP_DEVICE_ID;
    cfg.client_id        = APP_CLIENT_ID;
    cfg.model            = APP_MODEL;
    cfg.firmware_version = APP_FW_VERSION;
    cfg.on_event         = on_xz_event;

    ret = xz_start(&cfg);
    LOG_I("xz_start: %d", ret);
    return ret;
}

int app_xiaozhi_stop(void)
{
    s_connected = 0;
    return xz_stop();
}

/* ---------------- 4. 业务接口 ---------------- */
int app_send_text(const char *text)
{
    if (!s_connected)
        return XZ_ERR_STATE;
    return xz_send_text(text);
}

int app_abort(void)
{
    return xz_abort(XZ_ABORT_USER);
}

/* ---------------- 5. 调试命令 ---------------- */
static void xzapp_cmd(int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : "";

    if (strcmp(sub, "start") == 0)
    {
        rt_kprintf("start: %d\n", app_xiaozhi_start());
    }
    else if (strcmp(sub, "stop") == 0)
    {
        rt_kprintf("stop: %d\n", app_xiaozhi_stop());
    }
    else if (strcmp(sub, "text") == 0 && argc > 2)
    {
        rt_kprintf("text: %d\n", app_send_text(argv[2]));
    }
    else if (strcmp(sub, "abort") == 0)
    {
        rt_kprintf("abort: %d\n", app_abort());
    }
    else
    {
        rt_kprintf("usage: xzapp start|stop|text <content>|abort\n");
    }
}
MSH_CMD_EXPORT(xzapp_cmd, xiaozhi app: start | stop | text | abort);
```

#### 什么时候启动组件

组件不联网，所以启动要挂到你自己的联网流程上。

```c
/* 网络就绪时 */
if (网络连接成功)
    app_xiaozhi_start();

/* 网络断开时 */
if (网络断开连接)
    xz_disconnect();
```

到这里基础功能就能用了。要加语音看 3.2，要加设备控制看 3.3。

### 3.2 扩展：如何开启音频功能

#### 步骤 1：开开关

```
CONFIG_PKG_LIB_OPUS=y
CONFIG_XIAOZHI_FEATURE_AUDIO=y
CONFIG_AUDIO=y
CONFIG_MULTI_CLIENTS_AT_WORKING=y
```

#### 步骤 2：在 3.1 的代码上补三处

```c
/* 1) 按键按下：开始说话 */
xz_listen_start(XZ_MODE_MANUAL);

/* 2) 按键松开：说完了 */
xz_listen_stop();

/* 3) 想让 TTS 走别的出口时（默认用本机喇叭，可以不管） */
cfg.audio_out_cb = my_audio_out;
```

麦克风在 `xz_listen_start()` 时才打开，说完自动关，平时不占硬件。
喇叭在服务端第一段语音到达时才打开，采样率用服务端下发的值。


### 3.3 扩展：如何开启 mcp 功能并添加工具

MCP 让大模型能主动调用设备上的功能。比如你注册一个「打开指示灯」的工具，
用户说「帮我把灯打开」，大模型就会调用你的回调。

#### 步骤 1：开开关

```
CONFIG_XIAOZHI_FEATURE_MCP=y
```

#### 步骤 2：写工具回调

回调收到服务端传来的参数，把结果文本写进 `result` 返回就行：

```c
static int led_open_cb(const char *args_json, char *result,
                       size_t result_size, uint32_t call_id, void *user)
{
    /* TODO: 换成真实的点灯代码 */
    my_led_on();

    rt_snprintf(result, result_size, "指示灯已打开");
    return 0;                       /* 返回 0 表示执行完了 */
}
```

#### 步骤 3：把工具放进数组，挂到 cfg

```c
static const xz_tool_t s_tools[] =
{
    {
        .name         = "self.demo.led_open",
        .description  = "打开板载指示灯。用户说开灯、点亮、亮一下时调用。",
        .input_schema = NULL,       /* 没有参数就填 NULL */
        .callback     = led_open_cb,
    },
};

/* 在 3.1 的 app_xiaozhi_start() 里补这两行 */
cfg.tools      = s_tools;
cfg.tool_count = sizeof(s_tools) / sizeof(s_tools[0]);
```

注册在启动时一次性完成，最多 8 个工具。

**`description` 怎么写最关键**：这句话是给大模型看的，它靠这句话判断什么时候该调用
你的工具。写清楚「什么场景下用」，比写清楚参数更重要。

```c
/* 好：说清了触发场景 */
.description = "打开板载指示灯。用户说开灯、点亮、亮一下时调用。"

/* 不好：模型不知道什么时候该用 */
.description = "控制 LED"
```

#### 可选：需要等待的工具

如果工具要等外设动作完成（比如电机转到位），先返回 `XZ_TOOL_ASYNC`，
动作完成后再补结果：

```c
static int motor_cb(const char *args_json, char *result,
                    size_t result_size, uint32_t call_id, void *user)
{
    my_motor_start(call_id);    /* 记下 call_id */
    return XZ_TOOL_ASYNC;       /* 告诉组件：稍后回包 */
}

/* 电机转到位后，任意线程调用都可以 */
xz_tool_complete(call_id, "电机已转到位");
```

一旦返回了 `XZ_TOOL_ASYNC`，就必须保证最后会调 `xz_tool_complete()`，
否则这个调用会一直挂着。

#### 怎么确认工具注册成功

串口会打印：

```
I/xz_tool xz_tool: tool registered: self.demo.led_open
I/xz_tool xz_tool: tool call: self.demo.led_open({})     ← 模型调用时
I/xz_tool xz_tool: tool result: 指示灯已打开             ← 回调返回时
```

没有 `tool registered` 说明开关没开或 `tool_count` 配错了。

### 3.4 扩展：如何使用 msh 命令验证功能

打开 `CONFIG_XIAOZHI_USING_MSH=y` 后可以直接在串口敲命令，开发阶段建议打开。

#### 基础命令

| 命令 | 作用 |
|---|---|
| `xz stat` | 查看当前状态、会话 ID、音频参数 |
| `xz text <内容>` | 文本对话，最快验证链路是否通 |
| `xz abort` | 打断播报 |
| `xz act confirm` | 确认已完成网页绑定 |

#### 开了音频后可用

| 命令 | 作用 |
|---|---|
| `xz listen [auto\|manual\|realtime]` | 开始录音，不写参数默认自动模式 |
| `xz listen_stop` | 结束录音 |



