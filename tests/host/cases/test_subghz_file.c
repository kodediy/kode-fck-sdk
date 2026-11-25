/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_subghz_file.c
 * @brief fck_drv_subghz_file: a capture and a code written as Kode's .ksub
 *        files (docs/ksub-format.md), read back from them, and the reader's
 *        answer to files that are broken, cut short or hostile.
 *
 * The main case is a PT2262 remote on 433.92 MHz, code 0x5155C3, pulse
 * 436 us. What must never happen: a run read from a number that is not one,
 * runs written for a file that is then refused, a read past the length it was
 * given, a count that the file does not keep, and an output changed by a file
 * that was refused.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fck_drv_ook.h"
#include "fck_drv_subghz_file.h"

#define REMOTE_HZ   433920000U
#define REMOTE_CODE 0x5155C3U
#define REMOTE_TE   436U

#define UNTOUCHED   0x7EADBEEF

static char    s_text[FCK_DRV_SUBGHZ_FILE_RAW_MAX(1100U)];
static int32_t s_runs[1100];
static int32_t s_back[1100];

static const char REMOTE_FILE[] = "kode-subghz 1\n"
                                  "kind: code\n"
                                  "frequency: 433920000\n"
                                  "modulation: ook\n"
                                  "protocol: pt2262\n"
                                  "bits: 24\n"
                                  "code: 5155c3\n"
                                  "pulse: 436\n";

static const char RAW_HEAD[] = "kode-subghz 1\n"
                               "kind: raw\n"
                               "frequency: 433920000\n"
                               "modulation: ook\n";

static void fill_untouched(int32_t *runs, size_t n)
{
    for (size_t i = 0U; i < n; i++) {
        runs[i] = UNTOUCHED;
    }
}

static bool read_text(const char *t, fck_drv_subghz_file_t *f, int32_t *runs, size_t cap)
{
    return fck_drv_subghz_file_read(t, strlen(t), f, runs, cap);
}

/* ---- Writing ------------------------------------------------------------------- */

TEST_CASE("a RAW file is the header, the count and the runs on raw lines", "[subghz_file]")
{
    static const int32_t runs[] = { 436, -1308, 1308, -436, -2147483647 - 1, 2147483647 };
    size_t const         n      = fck_drv_subghz_file_write_raw(433920000U, runs, 6U, s_text, sizeof(s_text));
    static const char    want[] = "kode-subghz 1\n"
                                  "kind: raw\n"
                                  "frequency: 433920000\n"
                                  "modulation: ook\n"
                                  "pulses: 6\n"
                                  "raw: 436 -1308 1308 -436 -2147483648 2147483647\n";
    TEST_ASSERT_EQUAL_UINT32(sizeof(want) - 1U, n);
    TEST_ASSERT_EQUAL_STRING(want, s_text);
}

TEST_CASE("a RAW file starts a raw line every 512 runs", "[subghz_file]")
{
    for (size_t i = 0U; i < 1100U; i++) {
        s_runs[i] = (i & 1U) ? -(int32_t)(300U + i) : (int32_t)(300U + i);
    }
    size_t const n = fck_drv_subghz_file_write_raw(REMOTE_HZ, s_runs, 1100U, s_text, sizeof(s_text));
    TEST_ASSERT_GREATER_THAN_UINT32(0U, n);
    TEST_ASSERT_NOT_NULL(strstr(s_text, "pulses: 1100\n"));
    unsigned lines = 0U;
    for (const char *p = s_text; (p = strstr(p, "\nraw: ")) != NULL; p++) {
        lines++;
    }
    TEST_ASSERT_EQUAL_UINT32(3U, lines);   /* 512, 512 and 76 */
    fck_drv_subghz_file_t f;
    TEST_ASSERT_TRUE(fck_drv_subghz_file_read(s_text, n, &f, s_back, 1100U));
    TEST_ASSERT_EQUAL_UINT32(1100U, f.runs);
    TEST_ASSERT_FALSE(f.cut);
    TEST_ASSERT_EQUAL_INT32_ARRAY(s_runs, s_back, 1100U);
}

TEST_CASE("FCK_DRV_SUBGHZ_FILE_RAW_MAX holds the longest runs, and a byte less is refused whole",
          "[subghz_file]")
{
    for (size_t i = 0U; i < 1100U; i++) {
        s_runs[i] = (i & 1U) ? -2147483647 - 1 : 2147483647;
    }
    size_t const n = fck_drv_subghz_file_write_raw(4294967295U, s_runs, 1100U, s_text, sizeof(s_text));
    TEST_ASSERT_GREATER_THAN_UINT32(0U, n);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(FCK_DRV_SUBGHZ_FILE_RAW_MAX(1100U) - 1U, n);
    static char small[FCK_DRV_SUBGHZ_FILE_RAW_MAX(1100U)];
    memset(small, 'x', sizeof(small));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_raw(4294967295U, s_runs, 1100U, small, n));
    TEST_ASSERT_EQUAL_STRING("", small);
    TEST_ASSERT_EQUAL_UINT32(n, fck_drv_subghz_file_write_raw(4294967295U, s_runs, 1100U, small, n + 1U));
}

TEST_CASE("a RAW file of no runs, of a run of 0, of no frequency or of no room is not written", "[subghz_file]")
{
    static const int32_t good[] = { 100, -100 };
    static const int32_t zero[] = { 100, 0, -100 };
    memset(s_text, 'x', 8U);
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_raw(REMOTE_HZ, good, 0U, s_text, sizeof(s_text)));
    TEST_ASSERT_EQUAL_STRING("", s_text);
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_raw(REMOTE_HZ, NULL, 2U, s_text, sizeof(s_text)));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_raw(REMOTE_HZ, zero, 3U, s_text, sizeof(s_text)));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_raw(0U, good, 2U, s_text, sizeof(s_text)));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_raw(REMOTE_HZ, good, 2U, s_text, 30U));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_raw(REMOTE_HZ, good, 2U, NULL, 100U));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_raw(REMOTE_HZ, good, 2U, s_text, 0U));
}

TEST_CASE("A PT2262 remote as a code file: its protocol, 24 bits, the code in hex, its pulse", "[subghz_file]")
{
    char         text[FCK_DRV_SUBGHZ_FILE_CODE_MAX];
    size_t const n = fck_drv_subghz_file_write_code(REMOTE_HZ, "pt2262", 24U, REMOTE_CODE, REMOTE_TE, text,
                                                    sizeof(text));
    TEST_ASSERT_EQUAL_UINT32(sizeof(REMOTE_FILE) - 1U, n);
    TEST_ASSERT_EQUAL_STRING(REMOTE_FILE, text);
    fck_drv_subghz_file_t f;
    TEST_ASSERT_TRUE(fck_drv_subghz_file_read(text, n, &f, NULL, 0U));
    TEST_ASSERT_EQUAL(FCK_DRV_SUBGHZ_FILE_CODE, f.kind);
    TEST_ASSERT_EQUAL_UINT32(REMOTE_HZ, f.hz);
    TEST_ASSERT_EQUAL_STRING("pt2262", f.protocol);
    TEST_ASSERT_EQUAL_UINT8(24U, f.bits);
    TEST_ASSERT_EQUAL_UINT32(REMOTE_CODE, f.code);
    TEST_ASSERT_EQUAL_UINT16(REMOTE_TE, f.pulse_us);
}

TEST_CASE("a code file has as many hex digits as its bits take, and a pulse line only if there is a pulse",
          "[subghz_file]")
{
    char text[FCK_DRV_SUBGHZ_FILE_CODE_MAX];
    TEST_ASSERT_GREATER_THAN_UINT32(0U, fck_drv_subghz_file_write_code(868300000U, "ht12e", 12U, 0x0A5U, 0U, text, sizeof(text)));
    TEST_ASSERT_NOT_NULL(strstr(text, "bits: 12\ncode: 0a5\n"));
    TEST_ASSERT_NULL(strstr(text, "pulse"));
    TEST_ASSERT_GREATER_THAN_UINT32(0U, fck_drv_subghz_file_write_code(433920000U, "a-b_9", 32U, 0xFFFFFFFFU, 65535U, text, sizeof(text)));
    TEST_ASSERT_NOT_NULL(strstr(text, "code: ffffffff\npulse: 65535\n"));
    fck_drv_subghz_file_t f;
    TEST_ASSERT_TRUE(read_text(text, &f, NULL, 0U));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFU, f.code);
    TEST_ASSERT_EQUAL_UINT16(65535U, f.pulse_us);
    TEST_ASSERT_EQUAL_STRING("a-b_9", f.protocol);
}

TEST_CASE("a code file is not written for a bad protocol, bits, code, frequency or room", "[subghz_file]")
{
    char text[FCK_DRV_SUBGHZ_FILE_CODE_MAX];
    memset(text, 'x', sizeof(text));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_code(REMOTE_HZ, NULL, 24U, 1U, 0U, text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("", text);
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_code(REMOTE_HZ, "", 24U, 1U, 0U, text, sizeof(text)));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_code(REMOTE_HZ, "PT2262", 24U, 1U, 0U, text, sizeof(text)));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_code(REMOTE_HZ, "pt 2262", 24U, 1U, 0U, text, sizeof(text)));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_code(REMOTE_HZ, "pt2262\n", 24U, 1U, 0U, text, sizeof(text)));
    TEST_ASSERT_GREATER_THAN_UINT32(0U, fck_drv_subghz_file_write_code(REMOTE_HZ, "abcdefghijklmnopqrstuvwxyz01234", 24U, 1U, 0U, text, sizeof(text)));   /* 31 characters: written */
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_code(REMOTE_HZ, "abcdefghijklmnopqrstuvwxyz012345", 24U, 1U, 0U, text, sizeof(text)));   /* 32: not */
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_code(REMOTE_HZ, "pt2262", 0U, 0U, 0U, text, sizeof(text)));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_code(REMOTE_HZ, "pt2262", 33U, 1U, 0U, text, sizeof(text)));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_code(REMOTE_HZ, "pt2262", 12U, 0x1000U, 0U, text, sizeof(text)));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_code(0U, "pt2262", 24U, 1U, 0U, text, sizeof(text)));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_code(REMOTE_HZ, "pt2262", 24U, 1U, 0U, text, 100U));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_subghz_file_write_code(REMOTE_HZ, "pt2262", 24U, 1U, 0U, NULL, 100U));
}

TEST_CASE("A PT2262 remote, encoded, written as RAW and read back run for run", "[subghz_file]")
{
    size_t const n = fck_drv_ook_encode(FCK_DRV_OOK_PT2262, 24U, REMOTE_CODE, REMOTE_TE, 3U, s_runs, 1100U);
    TEST_ASSERT_EQUAL_UINT32(150U, n);
    size_t const length = fck_drv_subghz_file_write_raw(REMOTE_HZ, s_runs, n, s_text, sizeof(s_text));
    TEST_ASSERT_GREATER_THAN_UINT32(0U, length);
    fck_drv_subghz_file_t f;
    fill_untouched(s_back, 1100U);
    TEST_ASSERT_TRUE(fck_drv_subghz_file_read(s_text, length, &f, s_back, 1100U));
    TEST_ASSERT_EQUAL(FCK_DRV_SUBGHZ_FILE_RAW, f.kind);
    TEST_ASSERT_EQUAL_UINT32(REMOTE_HZ, f.hz);
    TEST_ASSERT_EQUAL_UINT32(n, f.runs);
    TEST_ASSERT_FALSE(f.cut);
    TEST_ASSERT_EQUAL_INT32_ARRAY(s_runs, s_back, n);
    TEST_ASSERT_EQUAL_INT32(UNTOUCHED, s_back[n]);
    fck_drv_ook_code_t code;
    TEST_ASSERT_EQUAL_UINT32(1U, fck_drv_ook_decode(s_back, f.runs, &code, 1U));
    TEST_ASSERT_EQUAL_UINT32(REMOTE_CODE, code.key);
}

/* ---- Reading what other tools and people write -------------------------------------- */

TEST_CASE("a file as a person writes it: CRLF, comments, blank lines, odd spacing, capitals, a key to come",
          "[subghz_file]")
{
    static const char text[] = "# my garage remote\r\n"
                               "\r\n"
                               "  kode-subghz 1  \r\n"
                               "Kind:\tCODE\r\n"
                               "frequency:433920000\r\n"
                               "# a key a later version adds\r\n"
                               "guard_time: 30\r\n"
                               "Modulation: OOK\r\n"
                               "protocol:   pt2262   \r\n"
                               "bits: 24\r\n"
                               "code: 5155C3\r\n"
                               "pulse: 436";   /* no end of line at the end */
    fck_drv_subghz_file_t f;
    TEST_ASSERT_TRUE(read_text(text, &f, NULL, 0U));
    TEST_ASSERT_EQUAL_UINT32(REMOTE_HZ, f.hz);
    TEST_ASSERT_EQUAL_STRING("pt2262", f.protocol);
    TEST_ASSERT_EQUAL_UINT32(REMOTE_CODE, f.code);
    TEST_ASSERT_EQUAL_UINT16(REMOTE_TE, f.pulse_us);
}

TEST_CASE("a RAW file over several lines into a smaller buffer keeps the first runs and says so", "[subghz_file]")
{
    static const char text[] = "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\n"
                               "pulses: 6\nraw: 100 -200 300\nraw: -400 500 -600\n";
    fck_drv_subghz_file_t f;
    fill_untouched(s_back, 8U);
    TEST_ASSERT_TRUE(read_text(text, &f, s_back, 4U));
    TEST_ASSERT_EQUAL_UINT32(4U, f.runs);
    TEST_ASSERT_TRUE(f.cut);
    TEST_ASSERT_EQUAL_INT32(100, s_back[0]);
    TEST_ASSERT_EQUAL_INT32(-400, s_back[3]);
    TEST_ASSERT_EQUAL_INT32(UNTOUCHED, s_back[4]);
    TEST_ASSERT_TRUE(read_text(text, &f, s_back, 6U));
    TEST_ASSERT_FALSE(f.cut);
    TEST_ASSERT_EQUAL_INT32(-600, s_back[5]);
    TEST_ASSERT_TRUE(read_text(text, &f, NULL, 0U));   /* counting only */
    TEST_ASSERT_EQUAL_UINT32(0U, f.runs);
    TEST_ASSERT_TRUE(f.cut);
}

TEST_CASE("only the first length bytes are read: a buffer from the SD has no NUL, and bytes after it are not seen",
          "[subghz_file]")
{
    char buffer[sizeof(REMOTE_FILE) + 40U];
    memset(buffer, 'Z', sizeof(buffer));
    memcpy(buffer, REMOTE_FILE, sizeof(REMOTE_FILE) - 1U);   /* no NUL after it */
    fck_drv_subghz_file_t f;
    TEST_ASSERT_TRUE(fck_drv_subghz_file_read(buffer, sizeof(REMOTE_FILE) - 1U, &f, NULL, 0U));
    /* The same bytes with the buffer's next bytes counted in: a line that has no colon. */
    TEST_ASSERT_FALSE(fck_drv_subghz_file_read(buffer, sizeof(REMOTE_FILE) + 3U, &f, NULL, 0U));
    /* Cut inside the last line: the number it has so far is the number it has. */
    TEST_ASSERT_TRUE(fck_drv_subghz_file_read(buffer, sizeof(REMOTE_FILE) - 3U, &f, NULL, 0U));
    TEST_ASSERT_EQUAL_UINT16(43U, f.pulse_us);
    /* Cut inside the header or a key: refused. */
    TEST_ASSERT_FALSE(fck_drv_subghz_file_read(buffer, 12U, &f, NULL, 0U));
    TEST_ASSERT_FALSE(fck_drv_subghz_file_read(buffer, 0U, &f, NULL, 0U));
}

/* ---- Files that are broken, or hostile ---------------------------------------------- */

typedef struct {
    const char *why;
    const char *text;
} refusal_t;

TEST_CASE("a file that is broken is refused, and the caller's outputs stay as they were", "[subghz_file]")
{
    static const refusal_t BAD[] = {
        { "no header", "kind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nbits: 1\ncode: 1\n" },
        { "another version", "kode-subghz 2\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nbits: 1\ncode: 1\n" },
        { "version 1.0", "kode-subghz 1.0\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nbits: 1\ncode: 1\n" },
        { "a header that is not first", "kind: code\nkode-subghz 1\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nbits: 1\ncode: 1\n" },
        { "a text that is not a capture", "[core]\nname=x\n" },
        { "a file of another kind", "hello\n" },
        { "nothing but the header", "kode-subghz 1\n" },
        { "no kind", "kode-subghz 1\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nbits: 1\ncode: 1\n" },
        { "a kind that is neither", "kode-subghz 1\nkind: packet\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nbits: 1\ncode: 1\n" },
        { "kind twice", "kode-subghz 1\nkind: code\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nbits: 1\ncode: 1\n" },
        { "no frequency", "kode-subghz 1\nkind: code\nmodulation: ook\nprotocol: a\nbits: 1\ncode: 1\n" },
        { "a frequency of 0", "kode-subghz 1\nkind: code\nfrequency: 0\nmodulation: ook\nprotocol: a\nbits: 1\ncode: 1\n" },
        { "a frequency over 32 bits", "kode-subghz 1\nkind: code\nfrequency: 4294967296\nmodulation: ook\nprotocol: a\nbits: 1\ncode: 1\n" },
        { "a frequency with a sign", "kode-subghz 1\nkind: code\nfrequency: +433920000\nmodulation: ook\nprotocol: a\nbits: 1\ncode: 1\n" },
        { "a frequency with a unit", "kode-subghz 1\nkind: code\nfrequency: 433.92\nmodulation: ook\nprotocol: a\nbits: 1\ncode: 1\n" },
        { "no modulation", "kode-subghz 1\nkind: code\nfrequency: 433920000\nprotocol: a\nbits: 1\ncode: 1\n" },
        { "a modulation that is not ook", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: 2fsk\nprotocol: a\nbits: 1\ncode: 1\n" },
        { "a line with no colon", "kode-subghz 1\nkind: code\nfrequency 433920000\nmodulation: ook\nprotocol: a\nbits: 1\ncode: 1\n" },
        { "no protocol", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\nbits: 1\ncode: 1\n" },
        { "an empty protocol", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol:\nbits: 1\ncode: 1\n" },
        { "a protocol in capitals", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: PT2262\nbits: 1\ncode: 1\n" },
        { "a protocol with a space", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: pt 2262\nbits: 1\ncode: 1\n" },
        { "a protocol of 32 characters", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: abcdefghijklmnopqrstuvwxyz012345\nbits: 1\ncode: 1\n" },
        { "protocol twice", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nprotocol: b\nbits: 1\ncode: 1\n" },
        { "no bits", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: a\ncode: 1\n" },
        { "bits of 0", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nbits: 0\ncode: 0\n" },
        { "bits of 33", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nbits: 33\ncode: 1\n" },
        { "bits that are not a number", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nbits: 2x\ncode: 1\n" },
        { "no code", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nbits: 8\n" },
        { "a code over its bits", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nbits: 8\ncode: 100\n" },
        { "a code of 9 digits", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nbits: 32\ncode: 000000001\n" },
        { "a code with 0x", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nbits: 8\ncode: 0x10\n" },
        { "a code that is not hex", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nbits: 8\ncode: 1g\n" },
        { "a pulse of 0", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nbits: 8\ncode: 1\npulse: 0\n" },
        { "a pulse of 65536", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nbits: 8\ncode: 1\npulse: 65536\n" },
        { "a negative pulse", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nbits: 8\ncode: 1\npulse: -436\n" },
        { "a code file with runs", "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\nprotocol: a\nbits: 8\ncode: 1\npulses: 2\nraw: 1 -1\n" },
        { "a RAW file with a code", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\npulses: 2\nraw: 1 -1\ncode: 1\n" },
        { "a RAW file with a protocol", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\nprotocol: a\npulses: 2\nraw: 1 -1\n" },
        { "a RAW file with no pulses line", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\nraw: 1 -1\n" },
        { "a RAW file with no raw line", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\npulses: 2\n" },
        { "raw before pulses", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\nraw: 1 -1\npulses: 2\n" },
        { "pulses twice", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\npulses: 2\npulses: 2\nraw: 1 -1\n" },
        { "pulses of 0", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\npulses: 0\nraw: 1 -1\n" },
        { "fewer runs than pulses", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\npulses: 3\nraw: 1 -1\n" },
        { "more runs than pulses", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\npulses: 1\nraw: 1 -1\n" },
        { "a pulses count that is huge", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\npulses: 4294967295\nraw: 1 -1\n" },
        { "a pulses count over 32 bits", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\npulses: 99999999999999999999\nraw: 1 -1\n" },
        { "a run of 0", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\npulses: 3\nraw: 1 0 -1\n" },
        { "a run of -0", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\npulses: 2\nraw: 1 -0\n" },
        { "a run over 32 bits", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\npulses: 2\nraw: 1 -2147483649\n" },
        { "a run over 32 bits, positive", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\npulses: 2\nraw: 2147483648 -1\n" },
        { "a run that is not a number", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\npulses: 2\nraw: 1 -a\n" },
        { "a run with a plus", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\npulses: 2\nraw: +1 -1\n" },
        { "a run that is a lone minus", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\npulses: 2\nraw: 1 -\n" },
        { "a run that is a decimal", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\npulses: 2\nraw: 1 -1.5\n" },
        { "a run in hex", "kode-subghz 1\nkind: raw\nfrequency: 433920000\nmodulation: ook\npulses: 2\nraw: 1 -0x10\n" },
    };
    fck_drv_subghz_file_t f = { .kind = FCK_DRV_SUBGHZ_FILE_CODE, .hz = 12345U, .bits = 7U };
    for (size_t i = 0U; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        fck_drv_subghz_file_t const before = f;
        TEST_ASSERT_FALSE_MESSAGE(read_text(BAD[i].text, &f, s_back, 1100U), BAD[i].why);
        TEST_ASSERT_EQUAL_MESSAGE(0, memcmp(&before, &f, sizeof(f)), BAD[i].why);
    }
}

TEST_CASE("a file with a NUL byte in it is refused, wherever the NUL is", "[subghz_file]")
{
    char text[sizeof(REMOTE_FILE)];
    memcpy(text, REMOTE_FILE, sizeof(REMOTE_FILE));
    fck_drv_subghz_file_t f;
    TEST_ASSERT_TRUE(fck_drv_subghz_file_read(text, sizeof(REMOTE_FILE) - 1U, &f, NULL, 0U));
    for (size_t at = 0U; at < sizeof(REMOTE_FILE) - 1U; at += 7U) {
        char saved = text[at];
        text[at]   = '\0';
        TEST_ASSERT_FALSE(fck_drv_subghz_file_read(text, sizeof(REMOTE_FILE) - 1U, &f, NULL, 0U));
        text[at] = saved;
    }
}

TEST_CASE("the reader's arguments: NULL text, NULL out, a buffer with no room and no cap", "[subghz_file]")
{
    fck_drv_subghz_file_t f;
    TEST_ASSERT_FALSE(fck_drv_subghz_file_read(NULL, 10U, &f, NULL, 0U));
    TEST_ASSERT_FALSE(fck_drv_subghz_file_read(REMOTE_FILE, sizeof(REMOTE_FILE) - 1U, NULL, NULL, 0U));
    TEST_ASSERT_FALSE(fck_drv_subghz_file_read(REMOTE_FILE, sizeof(REMOTE_FILE) - 1U, &f, NULL, 4U));   /* a cap and no buffer */
    TEST_ASSERT_TRUE(fck_drv_subghz_file_read(REMOTE_FILE, sizeof(REMOTE_FILE) - 1U, &f, NULL, 0U));
}

TEST_CASE("a long line is read where it stands: no copy, no limit but the length", "[subghz_file]")
{
    /* 100 000 runs on one line, with the count that says so. */
    static char big[800000];
    size_t      n = 0U;
    n += (size_t)snprintf(big + n, sizeof(big) - n, "%spulses: 100000\nraw:", RAW_HEAD);
    for (unsigned i = 0U; i < 100000U; i++) {
        n += (size_t)snprintf(big + n, sizeof(big) - n, " %s%u", (i & 1U) ? "-" : "", 100U + (i % 900U));
    }
    big[n++] = '\n';
    fck_drv_subghz_file_t f;
    TEST_ASSERT_TRUE(fck_drv_subghz_file_read(big, n, &f, s_back, 1100U));
    TEST_ASSERT_EQUAL_UINT32(1100U, f.runs);
    TEST_ASSERT_TRUE(f.cut);
    TEST_ASSERT_EQUAL_INT32(100, s_back[0]);
    TEST_ASSERT_EQUAL_INT32(-101, s_back[1]);
    /* The same, one run short of what the file says. */
    char *count = strstr(big, "pulses: 100000");
    TEST_ASSERT_NOT_NULL(count);
    count[13] = '1';   /* pulses: 100001 */
    TEST_ASSERT_FALSE(fck_drv_subghz_file_read(big, n, &f, s_back, 1100U));
}
