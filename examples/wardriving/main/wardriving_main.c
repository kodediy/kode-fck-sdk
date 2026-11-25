/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file wardriving_main.c
 * @brief F*CK example, a task: Wi-Fi, Bluetooth and the GNSS receiver at once, written to the
 *        microSD card as a WiGLE CSV file - every network and device heard, with the position it
 *        was heard at.
 *
 * This is the standard layer, fck.h: app_main() is a task, so each scan is a plain call that
 * waits and answers true or false, with fck_error() saying why.
 *
 * Wire: the F*CK module on J3, a microSD card in the Dot, and a view of the sky. The Dot on a
 * USB-C cable to read the log, or on its battery to walk.
 *
 * Try it: once the module is found and the receiver has a position, the example goes round and
 * round: the networks on 2.4 GHz (fck_wifi_scan()), the networks on 5 GHz, the Bluetooth LE
 * devices advertising for five seconds (fck_ble_scan()). After each of the three it reads the
 * latest fix (fck_gnss_read()) and, when that fix is recent, writes one row per network or device
 * to sd/wardriving/WigleWifi_<date><time>.csv, the file WiGLE's site takes. The log says, for
 * each lap, how many were heard and written and how long each scan took - counts and times, and
 * nothing else. A network heard in every lap has a row for every lap: WiGLE takes them all as
 * sightings of the one network. A short press of the bottom button is looked at between laps:
 * the lap under way finishes, the receiver goes back to reset and the example returns to kodeOS;
 * holding the button goes home as in every app.
 *
 * THIS EXAMPLE TRANSMITS. fck_wifi_scan() on 2.4 GHz sends probe requests on channels 1 to 13,
 * inside the radio limits the module's firmware keeps (one that keeps none itself is refused by the
 * SDK, and the example goes on with what only listens). 5 GHz is only listened to, and so is
 * Bluetooth. Nothing is sent before there is a position to write a row with.
 *
 * WHAT IT WRITES IS OTHER PEOPLE'S: the names and addresses of the networks and devices around,
 * each with where it was heard. That is what a wardriving file is, and why none of it reaches
 * the log. The file stays on your card; where it goes from there is yours to answer for.
 *
 * ---- One radio, three scans, and a receiver that goes on -------------------
 *
 * The C5 is the module's only Wi-Fi and Bluetooth radio, and the GNSS receiver's sentences reach
 * the Kode through it. So the scans go one after another, never two at once, and the receiver is
 * not stopped for any of them: the C5 relays its sentences between the lines of a scan's list,
 * and the SDK keeps the latest fix all along. A fix says how long ago it was read (age_ms), and
 * that is what pairs it with a scan: read right after the scan answers, a fix of a receiver that
 * sends one a second is under a second old. An older one - no sky, a relay that stopped - places
 * nothing, and the rows of that scan are counted as not placed and left out.
 *
 * The position is the one at the end of each scan, not the one at the instant each network was
 * heard: on foot the two are a few metres apart. A lap is as long as its three scans; the log
 * says each, so the time a lap takes with the networks around you is read there.
 */
#include <stdio.h>
#include <string.h>

#include "kode.h"
#include "kode_log.h"

#include "fck.h"

#include "wardriving_wigle.h"

#define TAG "wardriving"

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("F*CK wardriving", "example");

/** One wait for the module, and one for a first position; refused, each is asked again. */
#define WARDRIVING_WAIT_MS          5000U
#define WARDRIVING_PRESS_POLL_MS    50U /**< How often the buttons are asked before the start. */
/** How long the C5 listens for Bluetooth LE in each lap. */
#define WARDRIVING_BLE_SECONDS      5U
/** The oldest fix that still says where a scan's results were heard. */
#define WARDRIVING_FIX_AGE_MAX_MS   2000U
/** Where the files go; the card's root must be there, the folder is made. */
#define WARDRIVING_FOLDER           "sd/wardriving"
/** Rows gathered before they are written: one write to the card for many rows. */
#define WARDRIVING_TEXT_MAX         2048U

/** @brief What one lap did: counts and times, the only things the log is given. */
typedef struct {
    uint32_t heard;       /**< Networks and devices the three scans listed.          */
    uint32_t written;     /**< Rows that reached the file.                           */
    uint32_t not_placed;  /**< Heard with no recent fix to say where: left out.      */
    uint32_t lost;        /**< Rows that were not written: the card did not take
                           *   them, or one could not be made.                       */
    uint32_t wifi_2g_ms;  /**< How long the 2.4 GHz scan took; 0 when it was refused. */
    uint32_t wifi_5g_ms;
    uint32_t ble_ms;
    uint32_t fix_age_ms;  /**< The oldest fix a scan of this lap was paired with.    */
} wardriving_lap_t;

/* The scans' lists. Static: 64 networks and 128 devices are 14 KB, too much for a task's stack. */
static fck_wifi_network_t s_networks[FCK_WIFI_NETWORK_MAX];
static fck_ble_device_t   s_devices[FCK_BLE_DEVICE_MAX];

/* The file, and the rows waiting to go to it. */
static char   s_path[KODE_FILE_PATH_MAX];
static bool   s_file_open;
static char   s_text[WARDRIVING_TEXT_MAX];
static size_t s_text_len;
static uint32_t s_text_rows;

/* Whether "no position yet" was said for the wait under way: once is enough. */
static bool s_waiting_said;

/**
 * @brief Write the rows gathered so far to the card.
 * @param lap Where the rows are counted: written, or lost with the reason on the log.
 */
static void flush_rows(wardriving_lap_t * const lap)
{
    if ( s_text_len > 0U )
    {
        if ( kode_file_append(s_path, s_text) )
        {
            lap->written += s_text_rows;
        }
        else
        {
            lap->lost += s_text_rows;
            KLOG_W(TAG, "the card did not take %lu row(s): %s", (unsigned long)s_text_rows, kode_error());
        }
        s_text_len  = 0U;
        s_text_rows = 0U;
        s_text[0]   = '\0';
    }
}

/**
 * @brief Add one row to those waiting, writing them first when it would not fit.
 * @param lap The lap's counts.
 * @param row The row, NUL-terminated.
 * @param len Its length.
 */
static void add_row(wardriving_lap_t * const lap, char const * const row, size_t len)
{
    if ( (s_text_len + len + 1U) > sizeof(s_text) )
    {
        flush_rows(lap);
    }
    (void)memcpy(&s_text[s_text_len], row, len + 1U);
    s_text_len += len;
    s_text_rows++;
}

/**
 * @brief Make the folder and start the file, named after the fix's date and time (UTC).
 * @param fix A fix that places: it has a date and a time.
 * @return true when the file is there with its two first lines.
 */
static bool open_file(fck_gnss_fix_t const * const fix)
{
    char header[WARDRIVING_WIGLE_HEADER_MAX];

    if ( !kode_file_create_folder(WARDRIVING_FOLDER) )
    {
        KLOG_W(TAG, "no folder on the card: %s", kode_error());
        return false;
    }
    (void)snprintf(s_path, sizeof(s_path), WARDRIVING_FOLDER "/WigleWifi_%04u%02u%02u%02u%02u%02u.csv",
                   (unsigned)fix->year, (unsigned)fix->month, (unsigned)fix->day, (unsigned)fix->hour,
                   (unsigned)fix->min, (unsigned)fix->sec);
    if ( (0U == wardriving_wigle_header(header, sizeof(header))) || !kode_file_write(s_path, header) )
    {
        KLOG_W(TAG, "the file could not be started: %s", kode_error());
        return false;
    }
    KLOG_I(TAG, "writing to %s", s_path);
    return true;
}

/**
 * @brief The latest fix, when it is recent enough to say where a scan's results were heard.
 * @param lap The lap, which keeps the oldest fix it used.
 * @param fix Out: the fix.
 * @return true when rows may be written with it.
 */
static bool read_place(wardriving_lap_t * const lap, fck_gnss_fix_t * const fix)
{
    bool const places = fck_gnss_read(fix, 0U) && wardriving_wigle_fix_places(fix, WARDRIVING_FIX_AGE_MAX_MS);

    if ( places && (fix->age_ms > lap->fix_age_ms) )
    {
        lap->fix_age_ms = fix->age_ms;
    }
    return places;
}

/**
 * @brief One band's networks, each a row at the fix read when the scan answers.
 * @param lap  The lap's counts.
 * @param band FCK_BAND_2G (the C5 sends probe requests) or FCK_BAND_5G (it only listens).
 * @return How long the scan took, in ms; 0 when it was refused or failed.
 */
static uint32_t scan_wifi(wardriving_lap_t * const lap, fck_band_t band)
{
    size_t         count = 0U;
    uint32_t const start = kode_uptime_ms();
    fck_gnss_fix_t fix;
    char           row[WARDRIVING_WIGLE_ROW_MAX];

    if ( !fck_wifi_scan(band, s_networks, FCK_WIFI_NETWORK_MAX, &count) )
    {
        KLOG_W(TAG, "no %s scan: %s", (FCK_BAND_2G == band) ? "2.4 GHz" : "5 GHz", fck_error());
        return 0U;
    }
    uint32_t const took = kode_uptime_ms() - start;

    lap->heard += (uint32_t)count;
    if ( !read_place(lap, &fix) )
    {
        lap->not_placed += (uint32_t)count;
        return took;
    }
    for ( size_t i = 0U; i < count; i++ )
    {
        size_t const len = wardriving_wigle_wifi_row(row, sizeof(row), &s_networks[i], &fix);
        if ( len > 0U )
        {
            add_row(lap, row, len);
        }
        else
        {
            lap->lost++;
        }
    }
    return took;
}

/**
 * @brief The Bluetooth LE devices advertising, each a row at the fix read when the scan answers.
 *
 * A device's row carries the number of the company whose data it advertised, when it advertised
 * one (fck_ble_manufacturer_data(), with a firmware that says what each advertiser sent; with
 * another no device says any).
 *
 * @param lap The lap's counts.
 * @return How long the scan took, in ms; 0 when it failed.
 */
static uint32_t scan_ble(wardriving_lap_t * const lap)
{
    size_t         count = 0U;
    uint32_t const start = kode_uptime_ms();
    fck_gnss_fix_t fix;
    char           row[WARDRIVING_WIGLE_ROW_MAX];

    if ( !fck_ble_scan(WARDRIVING_BLE_SECONDS, s_devices, FCK_BLE_DEVICE_MAX, &count) )
    {
        KLOG_W(TAG, "no Bluetooth scan: %s", fck_error());
        return 0U;
    }
    uint32_t const took = kode_uptime_ms() - start;

    lap->heard += (uint32_t)count;
    if ( !read_place(lap, &fix) )
    {
        lap->not_placed += (uint32_t)count;
        return took;
    }
    for ( size_t i = 0U; i < count; i++ )
    {
        uint16_t         company     = 0U;
        bool const       has_company = fck_ble_manufacturer_data(&s_devices[i], &company, NULL, 0U, NULL);
        uint16_t const * maker       = has_company ? &company : NULL;
        size_t const     len         = wardriving_wigle_ble_row(row, sizeof(row), &s_devices[i], maker, &fix);
        if ( len > 0U )
        {
            add_row(lap, row, len);
        }
        else
        {
            lap->lost++;
        }
    }
    return took;
}

/**
 * @brief Whether there is a module with its receiver running and a position to write rows with.
 *
 * Waits for whichever is missing, up to WARDRIVING_WAIT_MS, and starts the file at the first
 * position. Nothing is scanned - and so nothing transmitted - until this answers true.
 *
 * @return true when a lap can be run.
 */
static bool ready(void)
{
    fck_status_t   status;
    fck_gnss_fix_t fix;

    if ( !fck_is_present() && !fck_wait_present(WARDRIVING_WAIT_MS) )
    {
        return false;       /* Not yet: fck_error() says so, and it is asked again. */
    }
    if ( fck_status(&status) && !status.gnss_running )
    {
        if ( !fck_gnss_begin() )
        {
            KLOG_W(TAG, "the receiver did not start: %s", fck_error());
            kode_delay(WARDRIVING_WAIT_MS);
            return false;
        }
        KLOG_I(TAG, "the receiver is on; waiting for a position");
    }
    if ( !(fck_gnss_read(&fix, 0U) && wardriving_wigle_fix_places(&fix, WARDRIVING_FIX_AGE_MAX_MS)) )
    {
        /* The next fix, when it comes: without the sky it comes every second, with no position. */
        uint32_t const start = kode_uptime_ms();
        bool const     came  = fck_gnss_read(&fix, WARDRIVING_WAIT_MS);
        if ( !(came && wardriving_wigle_fix_places(&fix, WARDRIVING_FIX_AGE_MAX_MS)) )
        {
            if ( !s_waiting_said )
            {
                KLOG_I(TAG, "no position yet: nothing is scanned until there is one");
                s_waiting_said = true;
            }
            if ( !came && ((kode_uptime_ms() - start) < WARDRIVING_WAIT_MS) )
            {
                /* Refused at once, not waited for: the receiver is off, or the module left. The
                 * next ask is not made at once either. */
                kode_delay(WARDRIVING_WAIT_MS);
            }
            return false;
        }
    }
    s_waiting_said = false;
    if ( !s_file_open )
    {
        s_file_open = open_file(&fix);
        if ( !s_file_open )
        {
            kode_delay(WARDRIVING_WAIT_MS);
        }
    }
    return s_file_open;
}

void app_main(void)
{
    /* First, always: brings up logging, QF, the event pools and the core Active Objects. */
    kode_init();

    /* Lends J3, starts the owner, switches the module's supply on. Does not wait for a module. */
    if ( !fck_begin() )
    {
        KLOG_E(TAG, "the SDK did not start: %s", fck_error());
        return;
    }
    KLOG_I(TAG, "up; looking for the F*CK on J3. Press the top button to start: the 2.4 GHz scan TRANSMITS probe "
                "requests. The bottom button leaves");

    /* The press counts are shared and start at boot: the first ask clears any press made before. */
    (void)kode_button_was_pressed("top");
    (void)kode_button_was_pressed("bottom");

    /* Nothing is scanned or written before a press: the scan transmits and the rows go to the card. */
    bool go   = false;
    bool quit = false;
    while ( !(go || quit) )
    {
        kode_delay(WARDRIVING_PRESS_POLL_MS);
        go   = kode_button_was_pressed("top");
        quit = kode_button_was_pressed("bottom");
    }
    if ( go )
    {
        KLOG_I(TAG, "started: lap after lap once the receiver has a position, until the bottom button");
    }

    uint32_t laps = 0U;
    while ( !(quit || kode_button_was_pressed("bottom")) )
    {
        if ( !ready() )
        {
            continue;
        }
        wardriving_lap_t lap   = { 0 };
        uint32_t const   start = kode_uptime_ms();

        lap.wifi_2g_ms = scan_wifi(&lap, FCK_BAND_2G);
        lap.wifi_5g_ms = scan_wifi(&lap, FCK_BAND_5G);
        lap.ble_ms     = scan_ble(&lap);
        flush_rows(&lap);
        laps++;
        if ( (0U == lap.wifi_2g_ms) && (0U == lap.wifi_5g_ms) && (0U == lap.ble_ms) )
        {
            /* None of the three ran - a firmware that is too old, a C5 that is busy - and each said
             * why. The next lap is not begun at once: a lap of refusals takes no time at all. */
            kode_delay(WARDRIVING_WAIT_MS);
        }
        KLOG_I(TAG, "lap %lu: %lu heard, %lu written, %lu not placed, %lu lost; 2.4 GHz %lu ms, 5 GHz %lu ms, "
                    "Bluetooth %lu ms, the lap %lu ms; fix up to %lu ms old",
               (unsigned long)laps, (unsigned long)lap.heard, (unsigned long)lap.written,
               (unsigned long)lap.not_placed, (unsigned long)lap.lost, (unsigned long)lap.wifi_2g_ms,
               (unsigned long)lap.wifi_5g_ms, (unsigned long)lap.ble_ms,
               (unsigned long)(kode_uptime_ms() - start), (unsigned long)lap.fix_age_ms);
    }

    /* The receiver back in reset, where it costs nothing and gives the module's bus back. */
    (void)fck_gnss_end();
    (void)fck_end();
    KLOG_I(TAG, "SDK ended; leaving");
    kode_exit();
}
