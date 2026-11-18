/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>

#include "sifli_unity.h"

/**
 * @brief  Main program
 * @param  None
 * @retval 0 if success, otherwise failure number
 */
int main(void)
{
    rt_kprintf("\nSiFli SDK unit tests (xiaozhi core)\n");
    rt_kprintf("  unity_list                  list the registered cases\n");
    rt_kprintf("  unity_run [suite|case|suite.case]\n");

    /*
     * With TEST_FWK_AUTORUN enabled at build time this runs every suite right
     * away, so the host only has to read the serial port; without it this is a
     * no-op and msh stays available.
     */
    sifli_unity_autorun();

    while (1)
    {
        rt_thread_mdelay(1000);
    }
    return 0;
}
