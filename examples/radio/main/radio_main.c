/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file radio_main.c
 * @brief F*CK example: the nRF listening: the 2.4 GHz band's levels, and the frames where it is busiest.
 *
 * The same three lines as every example in this directory: kode_init() brings
 * kodeOS up, then the example's own Active Object is constructed and started.
 * Everything worth reading, including what it does, is in radio_ao.c.
 */
#include "kode_sys.h"

#include "radio_ao.h"

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("F*CK radio", "example");

void app_main(void)
{
    /* First, always: posting to an AO before its queue exists is a boot loop. */
    kode_init();

    FckRadio_ctor();
    FckRadio_start();
}
