/*
 * msh debug commands
 *
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>
#include <string.h>
#include <stdlib.h>

#include "xiaozhi.h"

static const char *state_str(xz_state_t state)
{
    switch (state)
    {
    case XZ_STATE_INIT:       return "init";
    case XZ_STATE_BOOTSTRAP:  return "bootstrap";
    case XZ_STATE_CONNECTING: return "connecting";
    case XZ_STATE_IDLE:       return "idle";
    case XZ_STATE_LISTENING:  return "listening";
    case XZ_STATE_SPEAKING:   return "speaking";
    case XZ_STATE_PAUSED:     return "paused";
    case XZ_STATE_CLOSED:     return "closed";
    default:                  return "unknown";
    }
}

#if XIAOZHI_FEATURE_AUDIO
static int parse_mode(const char *s)
{
    if (!s)
        return XZ_MODE_AUTO;
    if (strcmp(s, "manual") == 0)
        return XZ_MODE_MANUAL;
    if (strcmp(s, "realtime") == 0)
        return XZ_MODE_REALTIME;
    return XZ_MODE_AUTO;
}
#endif

static void xz_usage(void)
{
    rt_kprintf("usage:\n");
#if XIAOZHI_FEATURE_AUDIO
    rt_kprintf("  xz listen [auto|manual|realtime]\n");
    rt_kprintf("  xz listen_stop\n");
#endif
    rt_kprintf("  xz abort\n");
    rt_kprintf("  xz text <content>\n");
    rt_kprintf("  xz stat\n");
    rt_kprintf("  xz act confirm\n");
}

static int xz_cmd(int argc, char **argv)
{
    const char *cmd;

    if (argc < 2)
    {
        xz_usage();
        return 0;
    }

    if (xz_get_state() == XZ_STATE_CLOSED)
    {
        rt_kprintf("xiaozhi core not started\n");
        return 0;
    }

    cmd = argv[1];

#if XIAOZHI_FEATURE_AUDIO
    /* audio only: these two commands do not exist without the audio path */
    if (strcmp(cmd, "listen") == 0)
    {
        int mode = parse_mode(argc > 2 ? argv[2] : NULL);

        rt_kprintf("xz_listen_start: %d\n",
                   xz_listen_start((xz_listen_mode_t)mode));
        return 0;
    }
    if (strcmp(cmd, "listen_stop") == 0)
    {
        rt_kprintf("xz_listen_stop: %d\n", xz_listen_stop());
        return 0;
    }
#endif

    if (strcmp(cmd, "abort") == 0)
    {
        int ret = xz_abort(XZ_ABORT_USER);
        rt_kprintf("xz_abort: %d\n", ret);
    }
    else if (strcmp(cmd, "text") == 0)
    {
        char text[512];
        size_t used = 0;
        int i;
        int ret;

        if (argc < 3)
        {
            rt_kprintf("usage: xz text <content>\n");
            return 0;
        }

        text[0] = '\0';
        for (i = 2; i < argc; i++)
        {
            int n = rt_snprintf(text + used, sizeof(text) - used,
                                "%s%s", (i == 2) ? "" : " ", argv[i]);
            if (n < 0 || (size_t)n >= sizeof(text) - used)
                break;
            used += (size_t)n;
        }

        ret = xz_send_text(text);
        rt_kprintf("xz_send_text: %d\n", ret);
    }
    else if (strcmp(cmd, "stat") == 0)
    {
        uint32_t rate = 0;
        uint32_t frame = 0;

        xz_get_negotiated(&rate, &frame);
        rt_kprintf("state      : %s\n", state_str(xz_get_state()));
        rt_kprintf("session    : %s\n", xz_get_session_id());
        rt_kprintf("audio      : %u Hz / %u ms\n",
                   (unsigned)rate, (unsigned)frame);
    }
    else if (strcmp(cmd, "act") == 0 &&
             argc > 2 && strcmp(argv[2], "confirm") == 0)
    {
        int ret = xz_activation_confirm();
        rt_kprintf("xz_activation_confirm: %d\n", ret);
    }
    else
    {
        xz_usage();
    }

    return 0;
}
MSH_CMD_EXPORT_ALIAS(xz_cmd, xz, "xiaozhi core commands");
