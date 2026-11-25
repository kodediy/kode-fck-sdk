/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_c5_rom.h
 * @brief The C5's flash written, read or asked about through its ROM loader,
 *        whatever the C5 runs.
 *
 * The ESP32-C5's ROM cannot be erased, and BOOT held low through a reset
 * always brings it up in its serial loader. So whatever somebody wrote into
 * the C5, the P4 can write its flash again from here, unless it burned the
 * eFuses that close the loader: Secure Boot or flash encryption (with the
 * secure download mode they bring by default), or the download mode off;
 * c5/'s template refuses to build with them. That is how the module stays
 * open (the official firmware contract).
 * Espressif's esp-serial-flasher speaks the loader's protocol. This file lends
 * it the C5's UART and its two control lines, which are only ever pulled low
 * or let go (fck_drv_pins.h). The ROM loader never starts the radio, and neither
 * does the flasher stub below: writing or reading the C5 transmits nothing
 * over the air.
 *
 * The write goes through the ROM loader with no stub, at 460800 baud, in
 * 1 KB blocks, the images deflated, and each region checked afterwards
 * against the MD5 the ROM computes over the flash.
 *
 * It runs inside fck_aos_run_job(), which hands it the lines and the C5's UART and
 * keeps everything else off them until it returns. That takes seconds to tens
 * of seconds: the C5 is reset into its loader, each region is erased, written
 * and checked, and the C5 is reset into its flash again, whether the write
 * worked or not.
 *
 * Reading takes more than the ROM: the C5's has no flash read (esptool 5.4.0
 * reads through a ROM only on the original ESP32, and on any other chip says
 * "ROM does not support function read_flash_slow"). So fck_drv_c5_rom_read(),
 * once the ROM has said the C5 is open, loads the flasher stub into its RAM:
 * Espressif's esp-flasher-stub, which esp-serial-flasher 2.1.0 carries for
 * the C5 (Apache-2.0 or MIT). A reset takes it away. Writes keep to the
 * ROM.
 *
 * Nothing is erased or read in a C5 that has secure boot or flash encryption
 * on: a plain image would not boot there, and what it holds now does. Those
 * two eFuses are read every time.
 */
#ifndef FCK_DRV_C5_ROM_H_
#define FCK_DRV_C5_ROM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"
#include "fck_drv_c5.h"
#include "fck_drv_pins.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The block the ROM loader takes per command, as esptool sends it. */
#define FCK_DRV_C5_ROM_BLOCK        1024U

/** The speed the regions go at once the loader answers. */
#define FCK_DRV_C5_ROM_BAUD         460800U

/** The fastest a write may ask for. The P4's UART goes higher; the ROM
 *  loader on the other end has not been tried above FCK_DRV_C5_ROM_BAUD. */
#define FCK_DRV_C5_ROM_BAUD_MAX     2000000U

/** The most regions one write takes: an ESP-IDF image is three. */
#define FCK_DRV_C5_ROM_PARTS_MAX    8U

/** The ROM erases whole sectors, so every region starts on one. */
#define FCK_DRV_C5_ROM_SECTOR       4096U

/** How far the ROM loader reaches: it addresses the flash in 24 bits. Where
 *  the C5's flash really ends, its flash ID says, once the loader answers. */
#define FCK_DRV_C5_ROM_REACH        (16U * 1024U * 1024U)

/** An MD5 as the ROM gives it: 32 hex digits. */
#define FCK_DRV_C5_ROM_MD5_LENGTH      32U

/**
 * @brief One region of the C5's flash and what goes into it.
 *
 * Raw, or deflated the way esptool compresses (a zlib
 * stream):
 * - raw: @c data holds the @c size bytes and @c zsize is 0. @c md5, when
 *   given, must be theirs: they are checked against it before the C5 is
 *   touched. When NULL it is worked out here.
 * - deflated: @c data holds a zlib stream of @c zsize bytes that inflates to
 *   the @c size bytes, and @c md5 is theirs and required. The loader keeps no
 *   digest of a compressed write, so the flash can only be checked against
 *   the one given.
 */
typedef struct {
    const char    *name;   /**< For the caller's own report; not used here.   */
    uint32_t       addr;   /**< Where it goes in the C5's flash: a multiple
                            *   of FCK_DRV_C5_ROM_SECTOR.                         */
    const uint8_t *data;
    uint32_t       size;   /**< What the region holds once written: more than
                            *   0, a multiple of 4, and ending within
                            *   FCK_DRV_C5_ROM_REACH.                             */
    uint32_t       zsize;  /**< The zlib stream's length, under
                            *   FCK_DRV_C5_ROM_REACH; 0 for raw data.             */
    const char    *md5;    /**< FCK_DRV_C5_ROM_MD5_LENGTH hex digits, either case,
                            *   of the @c size bytes; or NULL for raw data.   */
} fck_drv_c5_rom_part_t;

/**
 * @brief How far a write has gone: bytes sent out of all of them, a deflated
 *        region counted by its stream. Called at most once per percent, in
 *        the owner's task: short, no blocking, no fck_* calls.
 */
typedef void (*fck_drv_c5_rom_progress_fn)(void *ctx, uint32_t done, uint32_t total);

/** @brief What a write, a read or a probe found and did, filled whether it
 *         worked or not. */
typedef struct {
    bool     reset;            /**< The C5 went through its loader and was reset
                                *   into its flash at the end: out of reset now,
                                *   whatever came of it. false: its lines were
                                *   not touched.                               */
    bool     connected;        /**< The ROM loader answered.                     */
    uint8_t  mac[6];           /**< The C5's factory MAC; zeros if not read.     */
    bool     secure_boot;      /**< SECURE_BOOT_EN is burned: nothing written.   */
    bool     flash_encryption; /**< SPI_BOOT_CRYPT_CNT is odd: nothing written.  */
    uint32_t flash_size;       /**< The C5's flash, as its ID gives it, or 2 MB
                                *   when the ID names no size (esp-serial-flasher's
                                *   own guess); 0 if not read.                   */
    uint32_t baud;             /**< What the regions or the read went at;
                                *   FCK_BSP_C5_BAUD for a probe.               */
    int      failed;           /**< The region that failed, -1 if none did. A
                                *   read is region 0.                          */
    int      loader_error;     /**< esp-serial-flasher's code for the first
                                *   failure (esp_loader_error_t), 0 for none.    */
    uint32_t ms;               /**< From the first reset to the last one.        */
    char     note[112];        /**< The worst seen, in esp-serial-flasher's own
                                *   words (an MD5 mismatch, a block sent again)
                                *   or, for a UART left behind by the loader's
                                *   speed change, in this driver's; "" if
                                *   nothing. Nothing here logs: the caller
                                *   does, if it wants.                         */
    char     md5[FCK_DRV_C5_ROM_MD5_LENGTH + 1U]; /**< fck_drv_c5_rom_read(): the MD5 of
                                *   all it read, each block of which the stub
                                *   checked as it came; "" otherwise.          */
} fck_drv_c5_rom_report_t;

/**
 * @brief Whether fck_drv_c5_rom_write() would take these regions. Touches nothing
 *        and reads only what the parts point to.
 *
 * One to FCK_DRV_C5_ROM_PARTS_MAX of them, each as fck_drv_c5_rom_part_t says, and
 * no two sharing a byte of the flash. A raw region's data is not hashed here;
 * fck_drv_c5_rom_write() does that.
 */
bool fck_drv_c5_rom_parts_ok(const fck_drv_c5_rom_part_t *parts, size_t n);

/**
 * @brief Write @p n regions into the C5's flash, check each one, and boot
 *        the C5 into them.
 *
 * In this order: the raw regions are hashed and checked against their MD5,
 * if given; the C5 is reset into its ROM loader; the loader says what chip it
 * is, what its security eFuses hold and how big its flash is, which every
 * region must fit before the first is erased; the speed goes up to @p baud; each
 * region is erased, written and checked against the flash's MD5 in turn; and
 * the C5 is reset into its flash at FCK_BSP_C5_BAUD with BOOT let go, whether
 * the write worked or not (report.reset). Only a bad argument, a raw region
 * whose bytes are not its MD5 and no memory for the block leave the C5 as it
 * was. The UART is back at FCK_BSP_C5_BAUD on return, and whatever arrived on
 * it meanwhile has been dropped.
 *
 * @param hw     The module's lines (fck_aos_job_context_t's hw).
 * @param uart   The C5's UART, as the owner opened it.
 * @param parts  The regions, written in this order.
 * @param n      How many.
 * @param baud   The loader's speed for the writes, from FCK_BSP_C5_BAUD,
 *               where the ROM starts, to FCK_DRV_C5_ROM_BAUD_MAX: normally
 *               FCK_DRV_C5_ROM_BAUD.
 *               If the loader refuses a change, the write goes on at
 *               FCK_BSP_C5_BAUD and the report says so. If it takes the
 *               change and the UART cannot, nothing is erased: KODE_IO_ERROR,
 *               and the report's note says so.
 * @param cb     Progress; may be NULL.
 * @param ctx    Handed back to @p cb.
 * @param report What happened; may be NULL.
 * @return KODE_OK when every region was written and its MD5 matched: the
 *         flash holds them, not that they start;
 *         KODE_DENIED for a bad argument, a raw region whose bytes are not
 *         its MD5, a chip that is not an ESP32-C5, a C5 with secure boot or
 *         flash encryption on, or a region past the end of its flash - in
 *         all of them nothing was erased;
 *         KODE_TIMEOUT when the ROM loader never answered;
 *         KODE_IO_ERROR when it answered but a command or a check failed
 *         (report.failed says which region, -1 if none was started yet);
 *         KODE_NO_RESOURCES when the block buffer could not be had.
 */
kode_status_t fck_drv_c5_rom_write(const fck_drv_pins_t *hw, int uart,
                               const fck_drv_c5_rom_part_t *parts, size_t n,
                               uint32_t baud, fck_drv_c5_rom_progress_fn cb,
                               void *ctx, fck_drv_c5_rom_report_t *report);

/**
 * @brief Ask the C5's ROM loader what the C5 is, and write nothing.
 *
 * The C5 is reset into its loader, which says what chip it is, its MAC, what
 * its security eFuses hold and, for an open C5, how big its flash is; then it
 * is reset into its flash, as after a write. Nothing is erased, written or
 * read from its flash.
 *
 * @param hw     The module's lines (fck_aos_job_context_t's hw).
 * @param uart   The C5's UART, as the owner opened it.
 * @param report What the loader said; may be NULL.
 * @return KODE_OK when the loader answered every question and a write would
 *         go ahead; KODE_DENIED for a bad argument, a chip that is not an
 *         ESP32-C5, or a C5 with secure boot or flash encryption on (the
 *         report says which); KODE_TIMEOUT when the loader never answered;
 *         KODE_IO_ERROR when it answered but a question failed.
 */
kode_status_t fck_drv_c5_rom_probe(const fck_drv_pins_t *hw, int uart, fck_drv_c5_rom_report_t *report);

/**
 * @brief Where fck_drv_c5_rom_read() hands what it read: @p n bytes of the flash
 *        from @p addr on, in order and checked. Called in the owner's task:
 *        the loader waits while it runs, but no fck_* calls.
 * @return false to stop the read there.
 */
typedef bool (*fck_drv_c5_rom_sink_fn)(void *ctx, uint32_t addr, const uint8_t *data, uint32_t n);

/** The most fck_drv_c5_rom_read() hands @p sink at a time, from the heap. */
#define FCK_DRV_C5_ROM_READ_CHUNK   (16U * 1024U)

/**
 * @brief Read @p size bytes of the C5's flash from @p addr through its
 *        loader, for a copy before a write. Nothing is erased or written.
 *
 * The C5 is reset into its ROM loader, which says what the C5 is, as for
 * fck_drv_c5_rom_probe(). Only an open C5 whose flash holds the whole range goes
 * on: it is reset into its loader again and the flasher stub is loaded into
 * its RAM and run. The speed goes up to @p baud, the flash comes back in
 * blocks of up to FCK_DRV_C5_ROM_READ_CHUNK, each checked against the MD5 the
 * stub sends with it, and the C5 is reset into its flash again whatever came
 * of it. A 4 MB flash at 460800 takes about 100 s.
 *
 * @param hw     The module's lines (fck_aos_job_context_t's hw).
 * @param uart   The C5's UART, as the owner opened it.
 * @param addr   Where to start: a multiple of 4, within FCK_DRV_C5_ROM_REACH.
 * @param size   How much: a multiple of 4, more than 0, and ending within
 *               the C5's flash. Whole words only, as for a write: an odd
 *               byte is read with the word around it.
 * @param baud   As for fck_drv_c5_rom_write().
 * @param sink   Takes each block; must not be NULL.
 * @param ctx    Handed back to @p sink.
 * @param report What happened, with the MD5 of all it read; may be NULL.
 * @return KODE_OK when every byte reached @p sink, checked;
 *         KODE_DENIED for a bad argument, a chip that is not an ESP32-C5, a
 *         C5 with secure boot or flash encryption on, or a range past the end
 *         of its flash - in all of them nothing was read;
 *         KODE_TIMEOUT when the loader, or the stub, never answered;
 *         KODE_IO_ERROR when it answered but a command or a check failed;
 *         KODE_ERROR when @p sink stopped it;
 *         KODE_NO_RESOURCES when the block buffer could not be had.
 */
kode_status_t fck_drv_c5_rom_read(const fck_drv_pins_t *hw, int uart, uint32_t addr, uint32_t size,
                              uint32_t baud, fck_drv_c5_rom_sink_fn sink, void *ctx,
                              fck_drv_c5_rom_report_t *report);

/** How long fck_drv_c5_rom_listen() waits for the image's first line, at
 *  most: the module's firmware prints its own about 0.8 s after the ROM's
 *  last. */
#define FCK_DRV_C5_ROM_LISTEN_MS    5000U

/**
 * @brief Reset the C5 into its flash and read what it prints
 *        (fck_drv_c5_boot_t), sending it nothing.
 *
 * The C5 is held in reset while the UART is emptied, so that all that is read
 * comes from the reset on; BOOT is let go, as after a write, and the ROM
 * loader is not reached. It stops at the image's first line, once
 * fck_drv_c5_boot_done() says the C5 starts over and over or finds nothing
 * to start, or after @p ms.
 *
 * @param hw          The module's lines (fck_aos_job_context_t's hw).
 * @param uart        The C5's UART, as the owner opened it, at FCK_BSP_C5_BAUD.
 * @param ms          How long to wait at most: normally FCK_DRV_C5_ROM_LISTEN_MS.
 * @param out         What it printed; written whatever this returns.
 * @param listened_ms How long it listened; may be NULL.
 * @return KODE_OK once it listened, whatever came; KODE_DENIED for a bad
 *         argument, with the C5 left as it was.
 */
kode_status_t fck_drv_c5_rom_listen(const fck_drv_pins_t *hw, int uart, uint32_t ms,
                                    fck_drv_c5_boot_t *out, uint32_t *listened_ms);

#ifdef __cplusplus
}
#endif

#endif /* FCK_DRV_C5_ROM_H_ */
