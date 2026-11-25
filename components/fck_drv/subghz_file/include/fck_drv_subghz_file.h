/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_subghz_file.h
 * @brief A capture or a code as a Kode .ksub file, and back: the text an app
 *        keeps on the SD card.
 *
 * .ksub is Kode's own format, specified in docs/ksub-format.md. A file is
 * text: its first line - blank lines and comments apart - is `kode-subghz 1`,
 * and each line after it is a lowercase `key: value`.
 *
 *     kode-subghz 1                      kode-subghz 1
 *     kind: raw                          kind: code
 *     frequency: 433920000               frequency: 433920000
 *     modulation: ook                    modulation: ook
 *     pulses: 6                          protocol: pt2262
 *     raw: 436 -1308 1308 -436 436 -9000 bits: 24
 *                                        code: 5155c3
 *                                        pulse: 436
 *
 * `raw` lines are the runs fck_drv_cc1101_record() gives, > 0 the carrier on
 * for that many microseconds, < 0 off, 512 to a line; `pulses` says how many
 * there are in all, and a file that has another number is refused.
 *
 * A code file is any protocol's: this driver keeps its name as text, its bit
 * count, its code (up to 32 bits) and its pulse when it has one, and says
 * nothing of what they mean - fck_drv_ook knows which names are codes it
 * reads (ev1527, pt2262, ht12e) and their bits.
 *
 * Only on-off keying is read: the one modulation a line can name. Lines end
 * in \n or \r\n; blank lines and lines that start with # are skipped, and so
 * is a key the reader does not know, so that a later version may add some.
 * The reader never trusts the text: it reads exactly @p length bytes, takes
 * no number it cannot hold, and refuses what is repeated, contradicts itself
 * or is cut short.
 *
 * Plain C, like fck_drv_ook. Nothing here touches a file: the app writes and
 * reads the text with kode_fs.
 */
#ifndef FCK_DRV_SUBGHZ_FILE_H_
#define FCK_DRV_SUBGHZ_FILE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Runs on one `raw` line. */
#define FCK_DRV_SUBGHZ_FILE_RUNS_A_LINE  512U
/** Bytes that always hold a RAW file of @p n runs, its NUL included. */
#define FCK_DRV_SUBGHZ_FILE_RAW_MAX(n)   (136U + 13U * (n))
/** Bytes that always hold a code file of a protocol name of 31 characters or
 *  fewer, its NUL included. */
#define FCK_DRV_SUBGHZ_FILE_CODE_MAX     192U
/** The bytes of a protocol name, its NUL included. */
#define FCK_DRV_SUBGHZ_FILE_NAME_MAX     32U
/** The bits of the longest code. */
#define FCK_DRV_SUBGHZ_FILE_CODE_BITS    32U

/** @brief What a .ksub file holds. */
typedef enum {
    FCK_DRV_SUBGHZ_FILE_RAW  = 0,   /**< Runs.                                  */
    FCK_DRV_SUBGHZ_FILE_CODE = 1,   /**< A code: protocol, bits, code and pulse. */
} fck_drv_subghz_file_kind_t;

/** @brief A .ksub file, read. */
typedef struct {
    fck_drv_subghz_file_kind_t kind;
    uint32_t                   hz;
    size_t                     runs;      /**< RAW: the runs the file has.         */
    bool                       cut;       /**< RAW: there were more than fit.      */
    char                       protocol[FCK_DRV_SUBGHZ_FILE_NAME_MAX]; /**< CODE: its protocol line. */
    uint32_t                   code;      /**< CODE: the code, its @c bits bits.   */
    uint8_t                    bits;      /**< CODE: 1 to 32.                      */
    uint16_t                   pulse_us;  /**< CODE: its pulse line; 0 when it has none. */
} fck_drv_subghz_file_t;

/**
 * @brief The RAW file of @p n runs heard on @p hz, NUL-terminated.
 * @return Its length, the NUL not counted; 0, with @p text an empty string
 *         when @p cap is not 0, for no runs, a run of 0, no frequency or too
 *         small a @p cap (FCK_DRV_SUBGHZ_FILE_RAW_MAX(n) always does).
 */
size_t fck_drv_subghz_file_write_raw(uint32_t hz, const int32_t *runs, size_t n, char *text, size_t cap);

/**
 * @brief The code file of a code of @p protocol sent on @p hz, NUL-terminated:
 *        bits, and the code in as many hexadecimal digits as the bits take. A
 *        pulse line only when @p pulse_us is not 0.
 * @param protocol The protocol line: 1 to 31 characters, each a lowercase
 *                 letter, a digit, an underscore or a hyphen.
 * @param bits     1 to 32.
 * @return Its length, the NUL not counted; 0, with @p text an empty string
 *         when @p cap is not 0, for a bad @p protocol or @p bits, a code over
 *         its bits, no frequency or too small a @p cap
 *         (FCK_DRV_SUBGHZ_FILE_CODE_MAX always does).
 */
size_t fck_drv_subghz_file_write_code(uint32_t hz, const char *protocol, uint8_t bits, uint32_t code,
                                      uint16_t pulse_us, char *text, size_t cap);

/**
 * @brief Read the first @p length bytes of @p text as a .ksub file.
 *
 * A RAW file's runs go to @p runs, the first @p cap of them; a code file's
 * code to @p out. Refused: a first line (blank lines and comments apart) that
 * is not `kode-subghz 1`, a NUL
 * byte, a line with no colon, a known key twice, no kind, frequency or
 * modulation, a frequency of 0, a modulation that is not ook, a kind that is
 * none of raw and code, a key that belongs to the other kind, a RAW file with
 * no pulses line, no runs, a run of 0 or a count that is not its pulses'
 * number, a code file with no protocol, bits or code, a protocol name that
 * is not 1 to 31 lowercase letters, digits, underscores or hyphens, bits of
 * 0 or over 32, a code over its bits, a pulse of 0 or over 65535, and a
 * number that does not parse or does not fit.
 *
 * @param runs May be NULL with @p cap 0 for a code file.
 * @return false when refused; @p out is then left as it was and the contents
 *         of @p runs are not to be used.
 */
bool fck_drv_subghz_file_read(const char *text, size_t length, fck_drv_subghz_file_t *out, int32_t *runs,
                              size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* FCK_DRV_SUBGHZ_FILE_H_ */
