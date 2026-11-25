/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file gnss_main.c
 * @brief F*CK example: the GNSS receiver: a fix read every second, its quality logged.
 *
 * The same three lines as every example in this directory: kode_init() brings
 * kodeOS up, then the example's own Active Object is constructed and started.
 * Everything worth reading, including what it does, is in gnss_ao.c.
 */
#include "kode_sys.h"

#include "gnss_ao.h"

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("F*CK gnss", "example");

void app_main(void)
{
    /* First, always: posting to an AO before its queue exists is a boot loop. */
    kode_init();

    Gnss_ctor();
    Gnss_start();
}
