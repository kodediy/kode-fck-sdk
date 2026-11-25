/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file wardriving_wigle.c
 * @brief What the example heard, as the lines of a WiGLE CSV file (WigleWifi-1.6).
 *
 * See wardriving_wigle.h. Plain C: a network or a device and a fix go in, text comes out.
 */
#include "wardriving_wigle.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/** The longest name a row carries: a network's 32 bytes; a device's is shorter. */
#define NAME_BYTES_MAX  32U
/** A name between its quotes: every byte a quote, doubled. */
#define NAME_QUOTED_MAX ((2U * NAME_BYTES_MAX) + 3U)
/** A company's number as text: five digits. */
#define COMPANY_TEXT_MAX 8U
/** "-90.0000000,-180.0000000,100000,500.0" and its end. */
#define PLACE_TEXT_MAX  48U

/** The first byte that is text and not a control, and the one control past the letters. */
#define ASCII_SPACE     0x20U
#define ASCII_DELETE    0x7FU

/** A latitude and a longitude are on the Earth inside these, in degrees. */
#define LATITUDE_MAX_DEG   90.0
#define LONGITUDE_MAX_DEG  180.0
/** An altitude is one between a mine's floor and where the air ends, in metres. */
#define ALTITUDE_MIN_M     (-1000.0)
#define ALTITUDE_MAX_M     100000.0
/** What the receiver says for its dilution with no fix; past it, it is no dilution. */
#define HDOP_MAX           100.0
/** What the receiver's dilution is multiplied by to say an accuracy in metres. */
#define METRES_PER_HDOP    5.0

/** Wi-Fi's channels: 2.4 GHz counts 5 MHz a channel from 2407, with 14 by itself; 5 GHz from 5000. */
#define WIFI_CHANNEL_STEP_MHZ   5U
#define WIFI_2G_BASE_MHZ        2407U
#define WIFI_2G_CHANNEL_LAST    13U
#define WIFI_2G_CHANNEL_JAPAN   14U
#define WIFI_2G_JAPAN_MHZ       2484U
#define WIFI_5G_BASE_MHZ        5000U
#define WIFI_5G_CHANNEL_FIRST   32U
#define WIFI_5G_CHANNEL_LAST    177U

/**
 * @brief Whether @p fix holds what a row needs, whatever its age.
 * @param fix The fix; NULL is none.
 * @return true with a position on the Earth that the receiver vouches for, a date and a time.
 */
static bool fix_is_position(fck_gnss_fix_t const * const fix)
{
    return (NULL != fix) && fix->valid && fix->has_date && fix->has_time
           && isfinite(fix->lat) && isfinite(fix->lon)
           && (LATITUDE_MAX_DEG >= fabs(fix->lat)) && (LONGITUDE_MAX_DEG >= fabs(fix->lon));
}

/**
 * @brief How many bytes the UTF-8 character at @p s takes, when there is one.
 *
 * The table of Unicode's chapter 3: what the first byte says of how many follow, and the range
 * of the second, which is narrower after four of the first bytes - that is what leaves out the
 * long way to write a short character, the halves of a UTF-16 pair and what is past U+10FFFF.
 *
 * @param s    The bytes.
 * @param left How many there are; at least 1.
 * @return 1 to 4; 0 when the bytes there are not a character: a byte that only continues one,
 *         a character cut short, or a byte UTF-8 never has.
 */
static size_t utf8_length(unsigned char const * const s, size_t left)
{
    unsigned char const first  = s[0];
    unsigned char       low    = 0x80U;
    unsigned char       high   = 0xBFU;
    size_t              follow = 0U;
    size_t              length = 0U;
    bool                is_one = true;

    if ( 0x80U > first )
    {
        /* ASCII: itself. */
    }
    else if ( (0xC2U <= first) && (0xDFU >= first) )
    {
        follow = 1U;
    }
    else if ( (0xE0U <= first) && (0xEFU >= first) )
    {
        follow = 2U;
        if ( 0xE0U == first )
        {
            low = 0xA0U;
        }
        else if ( 0xEDU == first )
        {
            high = 0x9FU;
        }
        else
        {
            /* The whole range. */
        }
    }
    else if ( (0xF0U <= first) && (0xF4U >= first) )
    {
        follow = 3U;
        if ( 0xF0U == first )
        {
            low = 0x90U;
        }
        else if ( 0xF4U == first )
        {
            high = 0x8FU;
        }
        else
        {
            /* The whole range. */
        }
    }
    else
    {
        is_one = false;
    }

    if ( is_one && (0U < follow) )
    {
        is_one = (follow < left) && (low <= s[1]) && (high >= s[1]);
        for ( size_t i = 2U; is_one && (i <= follow); i++ )
        {
            is_one = (0x80U == (s[i] & 0xC0U));
        }
    }
    if ( is_one )
    {
        length = follow + 1U;
    }
    return length;
}

/**
 * @brief A name as a CSV field (RFC 4180): between quotes, its own quotes doubled.
 *
 * A name is whatever its owner typed: a comma is kept by the quotes, and a line break or any
 * other control byte, which would start a row of the name's own, becomes a question mark. The
 * file is UTF-8 and most names are; a byte that is not - a name in another encoding, or one cut
 * in the middle of a character by its 32 bytes - becomes a question mark too.
 *
 * @param out  Out: the field, NUL-terminated. NAME_QUOTED_MAX bytes.
 * @param name The name's bytes.
 * @param len  How many; at most NAME_BYTES_MAX are read.
 */
static void quote_name(char out[NAME_QUOTED_MAX], char const * const name, size_t len)
{
    unsigned char const * const bytes = (unsigned char const *)name;
    size_t const                n     = (NAME_BYTES_MAX < len) ? NAME_BYTES_MAX : len;
    size_t                      k     = 0U;
    size_t                      i     = 0U;

    out[k++] = '"';
    while ( i < n )
    {
        unsigned char const c      = bytes[i];
        size_t const        length = utf8_length(&bytes[i], n - i);

        if ( '"' == c )
        {
            out[k++] = '"';
            out[k++] = '"';
            i++;
        }
        else if ( (ASCII_SPACE > c) || (ASCII_DELETE == c) || (0U == length) )
        {
            out[k++] = '?';
            i++;
        }
        else
        {
            (void)memcpy(&out[k], &bytes[i], length);
            k += length;
            i += length;
        }
    }
    out[k++] = '"';
    out[k]   = '\0';
}

/**
 * @brief The centre of a Wi-Fi channel, in MHz.
 * @param band    FCK_BAND_2G or FCK_BAND_5G.
 * @param channel The channel.
 * @return The frequency; 0 for a channel the band does not have.
 */
static unsigned channel_mhz(fck_band_t band, uint8_t channel)
{
    unsigned mhz = 0U;

    if ( FCK_BAND_5G == band )
    {
        if ( (WIFI_5G_CHANNEL_FIRST <= channel) && (WIFI_5G_CHANNEL_LAST >= channel) )
        {
            mhz = WIFI_5G_BASE_MHZ + (WIFI_CHANNEL_STEP_MHZ * (unsigned)channel);
        }
    }
    else if ( WIFI_2G_CHANNEL_JAPAN == channel )
    {
        mhz = WIFI_2G_JAPAN_MHZ;
    }
    else if ( (1U <= channel) && (WIFI_2G_CHANNEL_LAST >= channel) )
    {
        mhz = WIFI_2G_BASE_MHZ + (WIFI_CHANNEL_STEP_MHZ * (unsigned)channel);
    }
    else
    {
        /* Not a 2.4 GHz channel. */
    }
    return mhz;
}

/**
 * @brief A network's security as WiGLE's own app writes it: Android's capability flags.
 *
 * The scan says the kind of security, not the cipher, so each kind is written with the cipher
 * it uses by default.
 *
 * @param auth The security.
 * @return The flags; "[ESS]" alone for an open network and for a kind this example does not know.
 */
static char const *security_flags(fck_wifi_security_t auth)
{
    char const *flags = "[ESS]";

    switch ( auth )
    {
        case FCK_WIFI_SECURITY_WEP:                  flags = "[WEP][ESS]"; break;
        case FCK_WIFI_SECURITY_WPA_PSK:              flags = "[WPA-PSK-TKIP][ESS]"; break;
        case FCK_WIFI_SECURITY_WPA2_PSK:             flags = "[WPA2-PSK-CCMP][ESS]"; break;
        case FCK_WIFI_SECURITY_WPA_WPA2_PSK:         flags = "[WPA-PSK-TKIP][WPA2-PSK-CCMP][ESS]"; break;
        case FCK_WIFI_SECURITY_WPA2_ENTERPRISE:      flags = "[WPA2-EAP-CCMP][ESS]"; break;
        case FCK_WIFI_SECURITY_WPA3_PSK:             flags = "[WPA3-SAE-CCMP][ESS]"; break;
        case FCK_WIFI_SECURITY_WPA2_WPA3_PSK:        flags = "[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]"; break;
        case FCK_WIFI_SECURITY_WAPI_PSK:             flags = "[WAPI-PSK][ESS]"; break;
        case FCK_WIFI_SECURITY_OWE:                  flags = "[OWE][ESS]"; break;
        case FCK_WIFI_SECURITY_WPA3_ENTERPRISE_192:  flags = "[WPA3-EAP-SUITE-B-192][ESS]"; break;
        case FCK_WIFI_SECURITY_DPP:                  flags = "[DPP][ESS]"; break;
        case FCK_WIFI_SECURITY_WPA3_ENTERPRISE:      flags = "[WPA3-EAP-CCMP][ESS]"; break;
        case FCK_WIFI_SECURITY_WPA2_WPA3_ENTERPRISE: flags = "[WPA2-EAP-CCMP][WPA3-EAP-CCMP][ESS]"; break;
        case FCK_WIFI_SECURITY_WPA_ENTERPRISE:       flags = "[WPA-EAP-TKIP][ESS]"; break;
        case FCK_WIFI_SECURITY_OPEN:
        case FCK_WIFI_SECURITY_UNKNOWN:
        default:
            /* Open, or not known: a network and nothing more. */
            break;
    }
    return flags;
}

/**
 * @brief The columns a fix fills after the signal: latitude, longitude, altitude, accuracy.
 *
 * WiGLE takes the altitude in whole metres, and an accuracy in metres that this receiver does
 * not give: its horizontal dilution times METRES_PER_HDOP, the usual figure for one. Either is 0
 * when the receiver did not say, or said what cannot be one.
 *
 * @param out Out: "lat,lon,altitude,accuracy", NUL-terminated.
 * @param cap How many bytes @p out holds; PLACE_TEXT_MAX is enough.
 * @param fix A fix that is a position.
 * @return true when the four are there whole.
 */
static bool place_columns(char * const out, size_t cap, fck_gnss_fix_t const * const fix)
{
    long   altitude = 0L;
    double accuracy = 0.0;

    if ( isfinite(fix->alt_m) && (ALTITUDE_MIN_M <= fix->alt_m) && (ALTITUDE_MAX_M >= fix->alt_m) )
    {
        altitude = lround(fix->alt_m);
    }
    if ( isfinite(fix->hdop) && (0.0 <= fix->hdop) && (HDOP_MAX >= fix->hdop) )
    {
        accuracy = fix->hdop * METRES_PER_HDOP;
    }
    int const len = snprintf(out, cap, "%.7f,%.7f,%ld,%.1f", fix->lat, fix->lon, altitude, accuracy);

    return (0 < len) && ((size_t)len < cap);
}

/**
 * @brief Hand a finished row over, whole or not at all.
 * @param out Out: the row.
 * @param cap How many bytes @p out holds.
 * @param row The row, NUL-terminated.
 * @param len What snprintf() said of it.
 * @param max How many bytes @p row holds.
 * @return The row's length; 0, and nothing written, when it was cut or does not fit.
 */
static size_t give_row(char * const out, size_t cap, char const * const row, int len, size_t max)
{
    size_t given = 0U;

    if ( (len > 0) && ((size_t)len < max) && ((size_t)len < cap) )
    {
        (void)memcpy(out, row, (size_t)len + 1U);
        given = (size_t)len;
    }
    return given;
}

bool wardriving_wigle_fix_places(const fck_gnss_fix_t *fix, uint32_t age_max_ms)
{
    return fix_is_position(fix) && (fix->age_ms <= age_max_ms);
}

size_t wardriving_wigle_header(char *out, size_t cap)
{
    /* The first line's fields are WiGLE's, in its order; star, body and subBody say Earth. */
    static char const header[] =
        "WigleWifi-1.6,appRelease=fck-wardriving-1,model=Kode Dot,release=kodeOS,device=F*CK,display=,"
        "board=ESP32-P4,brand=Kode,star=Sol,body=3,subBody=0\n"
        "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,"
        "AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n";
    size_t given = 0U;

    if ( (NULL != out) && (sizeof(header) <= cap) )
    {
        (void)memcpy(out, header, sizeof(header));
        given = sizeof(header) - 1U;
    }
    return given;
}

size_t wardriving_wigle_wifi_row(char *out, size_t cap, const fck_wifi_network_t *network, const fck_gnss_fix_t *fix)
{
    size_t given = 0U;

    if ( (NULL != out) && (NULL != network) && fix_is_position(fix) )
    {
        char name[NAME_QUOTED_MAX];
        char place[PLACE_TEXT_MAX];
        char row[WARDRIVING_WIGLE_ROW_MAX];
        int  len = 0;

        quote_name(name, network->ssid, network->ssid_len);
        if ( place_columns(place, sizeof(place), fix) )
        {
            /* MAC, SSID, AuthMode, FirstSeen, Channel, Frequency, RSSI, the place, no RCOIs, no MfgrId, Type. */
            len = snprintf(row, sizeof(row),
                           "%02x:%02x:%02x:%02x:%02x:%02x,%s,%s,%04u-%02u-%02u %02u:%02u:%02u,%u,%u,%d,%s,,,WIFI\n",
                           (unsigned)network->bssid[0], (unsigned)network->bssid[1], (unsigned)network->bssid[2],
                           (unsigned)network->bssid[3], (unsigned)network->bssid[4], (unsigned)network->bssid[5],
                           name, security_flags(network->auth), (unsigned)fix->year, (unsigned)fix->month,
                           (unsigned)fix->day, (unsigned)fix->hour, (unsigned)fix->min, (unsigned)fix->sec,
                           (unsigned)network->channel, channel_mhz(network->band, network->channel),
                           (int)network->rssi, place);
        }
        given = give_row(out, cap, row, len, sizeof(row));
    }
    return given;
}

size_t wardriving_wigle_ble_row(char *out, size_t cap, const fck_ble_device_t *device, const uint16_t *company,
                                const fck_gnss_fix_t *fix)
{
    size_t given = 0U;

    if ( (NULL != out) && (NULL != device) && fix_is_position(fix) )
    {
        char name[NAME_QUOTED_MAX];
        char place[PLACE_TEXT_MAX];
        char maker[COMPANY_TEXT_MAX] = "";
        char row[WARDRIVING_WIGLE_ROW_MAX];
        int  len = 0;

        quote_name(name, device->name, device->name_len);
        if ( NULL != company )
        {
            (void)snprintf(maker, sizeof(maker), "%u", (unsigned)*company);
        }
        if ( place_columns(place, sizeof(place), fix) )
        {
            /* A Bluetooth row has channel 0, no frequency and no RCOIs; "Misc [LE]" is what WiGLE's
             * app writes for a device heard by an LE scan and of no known kind. */
            len = snprintf(row, sizeof(row),
                           "%02x:%02x:%02x:%02x:%02x:%02x,%s,Misc [LE],%04u-%02u-%02u %02u:%02u:%02u,0,,%d,%s,,%s,BLE\n",
                           (unsigned)device->addr[0], (unsigned)device->addr[1], (unsigned)device->addr[2],
                           (unsigned)device->addr[3], (unsigned)device->addr[4], (unsigned)device->addr[5],
                           name, (unsigned)fix->year, (unsigned)fix->month, (unsigned)fix->day,
                           (unsigned)fix->hour, (unsigned)fix->min, (unsigned)fix->sec, (int)device->rssi, place,
                           maker);
        }
        given = give_row(out, cap, row, len, sizeof(row));
    }
    return given;
}
