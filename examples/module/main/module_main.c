/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file module_main.c
 * @brief F*CK example, a task: the module found on J3, and what runs on its chips.
 *
 * This is the standard layer, fck.h: app_main() is a task, so it can wait, and every call of fck.h
 * that waits is a plain call that answers true or false, with fck_error() saying why. Nothing here
 * is an Active Object. The Active Object side of the same module is fck_sys.h, in the other
 * examples (wifi, gnss, radio, subghz, console_actions).
 *
 * Wire: the F*CK module on J3, and the Dot on a USB-C cable. Nothing else.
 *
 * Try it: once the module is found, the log says what the owner knows of it (fck_status()) and what
 * each chip runs (fck_firmware(): the firmware the module ships with, or a firmware of your own, with its
 * version and whether it keeps the radio limits). Pull the module out and the log says so; put it back
 * and it is read again. A short press of the bottom button ends the SDK and returns to kodeOS; holding it
 * goes home as in every app (CONFIG_KODEOS_HOME_HOLD). NOTHING TRANSMITS: every call here reads.
 *
 * ---- Events and waiting ---------------------------------------------------
 *
 * fck_event_add_listener() tells the app when the module is found, gone, or something that is not
 * an F*CK sits on J3. A listener runs on a task of the SDK's own, not on the owner's and not on this
 * one, so it may log and may call fck.h (but not fck_event_wait(), nor fck_end(): they would wait
 * for the task they run on). This task still waits for the module with fck_wait_present() and asks
 * once a second whether it is there: a short, steady read of fck_status(), which never waits.
 */
#include <stdio.h>
#include <string.h>

#include "kode.h"
#include "kode_log.h"

#include "fck.h"

#define TAG "module"

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("F*CK module", "example");

/** One wait for the module; refused, it is asked again. */
#define MODULE_WAIT_MS   5000U
/** How often the module is asked whether it is still there. */
#define MODULE_POLL_MS   1000U

/**
 * @brief Told on the SDK's events task: a line on the log and nothing else.
 * @param e What happened: its name, a number and a word (see fck.h's table).
 */
static void on_event(const fck_event_t *e)
{
    if ( 0 == strcmp(e->name, "wrong_module") )
    {
        KLOG_W(TAG, "event: that is not an F*CK (%s)",
               ((int32_t)FCK_WRONG_MODULE_HOLDS_PAIR == e->value) ? "something holds the I2C pair"
                                                                    : "another chip at the expander's address");
    }
    else
    {
        KLOG_I(TAG, "event: %s", e->name);
    }
}

/**
 * @brief One chip's line: what it runs, as it greeted.
 * @param name The chip.
 * @param a    Its greeting.
 */
static void show_firmware(char const * const name, fck_firmware_t const * const a)
{
    if ( !a->known )
    {
        KLOG_I(TAG, "%-4s said nothing", name);
    }
    else
    {
        /* 0.1.0 is a version; one that printed none has no numbers to show. */
        char ver[16] = "";
        if ( a->has_version )
        {
            (void)snprintf(ver, sizeof(ver), " %u.%u.%u", (unsigned)a->ver_major, (unsigned)a->ver_minor,
                           (unsigned)a->ver_patch);
        }
        KLOG_I(TAG, "%-4s runs %s%s: %s%s", name, a->fw, ver,
               a->official ? "the module's firmware" : "a firmware of your own",
               a->official ? "" : (a->compatible ? ", speaks the SDK's words" : ", not the SDK's words"));
    }
}

/**
 * @brief What the owner knows of the module, then what each chip runs.
 * @return The module's generation, to notice another one later.
 */
static uint32_t read_module(void)
{
    fck_status_t st;
    fck_firmware_t  firmware;

    if ( !fck_status(&st) )
    {
        KLOG_W(TAG, "no status: %s", fck_error());
        return 0U;
    }
    KLOG_I(TAG, "module %lu: C5 %s, nRF %s, CC1101 %s (0x%02X/0x%02X), GNSS %s",
           (unsigned long)st.generation, st.c5_on ? "on" : "held in reset",
           st.nrf_on ? "on" : "held in reset", st.cc1101_ok ? "asleep" : "missing",
           (unsigned)st.cc1101_part, (unsigned)st.cc1101_version,
           st.gnss_running ? "running" : "in reset");

    if ( fck_firmware(FCK_C5, &firmware) )
    {
        show_firmware("C5", &firmware);
    }
    else
    {
        KLOG_W(TAG, "C5: %s", fck_error());
    }
    if ( fck_firmware(FCK_NRF, &firmware) )
    {
        show_firmware("nRF", &firmware);
    }
    else
    {
        KLOG_W(TAG, "nRF: %s", fck_error());
    }
    return st.generation;
}

void app_main(void)
{
    /* First, always: brings up logging, QF, the event pools and the core Active Objects. */
    kode_init();

    /* Before fck_begin(): none of the events is missed. A listener is a plain function. */
    (void)fck_event_add_listener("module_found", on_event);
    (void)fck_event_add_listener("module_lost", on_event);
    (void)fck_event_add_listener("wrong_module", on_event);

    /* Lends J3, starts the owner, switches the module's supply on. Does not wait for a module. */
    if ( !fck_begin() )
    {
        KLOG_E(TAG, "the SDK did not start: %s", fck_error());
        return;
    }
    KLOG_I(TAG, "up; looking for the F*CK on J3");

    /* The press count is shared and starts at boot: the first ask clears any press made before. */
    (void)kode_button_was_pressed("bottom");

    uint32_t generation = 0U;
    bool     reading    = false;
    while ( !kode_button_was_pressed("bottom") )
    {
        if ( !reading )
        {
            /* Waits up to MODULE_WAIT_MS: false is "not yet" (fck_error() says so), not a failure. */
            if ( fck_wait_present(MODULE_WAIT_MS) )
            {
                generation = read_module();
                reading    = true;
            }
        }
        else
        {
            fck_status_t now;
            if ( fck_status(&now) && !now.present )
            {
                KLOG_W(TAG, "the module left J3");
                reading = false;
            }
            else if ( fck_status(&now) && (now.generation != generation) )
            {
                KLOG_I(TAG, "another module was set up; reading its chips");
                generation = read_module();
            }
            else
            {
                /* Still the same module. */
            }
            kode_delay(MODULE_POLL_MS);
        }
    }

    (void)fck_end();
    KLOG_I(TAG, "SDK ended; leaving");
    kode_exit();
}
