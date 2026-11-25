/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_smp.h
 * @brief mcumgr's SMP over the nRF's UART, as the P4 writes and reads it:
 *        how a new image goes into the nRF54L15.
 *
 * Two servers speak it on the same wire: MCUboot's serial recovery, for the
 * 3 s after each reset (FCK_BSP_NRF_RECOVERY_MS), and the firmware's mcumgr,
 * through its shell. Both take an image upload, and answer the same way.
 *
 * One packet is an 8-byte header and a CBOR map:
 *
 *     op | flags | len (BE) | group (BE) | seq | id | CBOR
 *
 * and on the wire (Zephyr's mcumgr serial transport) it goes as
 *
 *     len+2 (BE) | packet | CRC16-XMODEM of the packet (BE)
 *
 * base64-encoded and cut into lines of at most FCK_DRV_SMP_LINE_MAX bytes:
 * 0x06 0x09, base64, '\n' for the first; 0x04 0x14, base64, '\n' for the
 * rest; each line a whole number of base64 quads. A line without those two
 * bytes is the firmware's own text, and is skipped; so is any text in front of
 * them on their line.
 *
 * Plain C, like fck_drv_link and fck_drv_nrf: no UART and no waiting, and
 * tests/host runs it as it is.
 */
#ifndef FCK_DRV_SMP_H_
#define FCK_DRV_SMP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One line on the wire: its two start bytes, base64 and '\n'. */
#define FCK_DRV_SMP_LINE_MAX    127U
/** The base64 characters on one line: whole quads, 2 + 120 + 1 <= 127. */
#define FCK_DRV_SMP_BASE64_PER     120U
/** The longest packet, header and CBOR, either way. */
#define FCK_DRV_SMP_PACKET_MAX     384U
/** The image bytes one upload request carries: the firmware's SMP buffer is
 *  256 bytes. */
#define FCK_DRV_SMP_CHUNK       128U
/** The bytes one request takes on the wire, at most. */
#define FCK_DRV_SMP_WIRE_MAX    544U
/** Image slots one state answer is read for. */
#define FCK_DRV_SMP_IMAGES_MAX  4U
/** The longest image hash: SHA-512. */
#define FCK_DRV_SMP_HASH_MAX    64U

/** @brief A request, built and checked, ready for fck_drv_smp_frame(). */
typedef struct {
    uint8_t pkt[FCK_DRV_SMP_PACKET_MAX];
    size_t  len;
} fck_drv_smp_request_t;

/* ---- Requests ---------------------------------------------------------------
 *
 * Each builder writes the header with @p seq, which the answer carries back,
 * and returns KODE_DENIED, with nothing written, when an argument is bad or
 * the packet would not fit. */

/** @brief OS echo: the server sends @p text back. Up to 64 bytes. */
kode_status_t fck_drv_smp_request_echo(fck_drv_smp_request_t *r, uint8_t seq, const char *text);

/** @brief Image state: what each slot holds. */
kode_status_t fck_drv_smp_request_state(fck_drv_smp_request_t *r, uint8_t seq);

/**
 * @brief Image upload: the next piece of @p img, from @p off.
 *
 * The first, at offset 0, also says the whole length, and the server erases
 * the slot for it: it takes seconds to answer. In MCUboot's serial recovery
 * the image goes straight to slot 0; to the firmware, to slot 1.
 *
 * @param took Out: the image bytes this request carries, up to
 *             FCK_DRV_SMP_CHUNK.
 * @return KODE_DENIED for no image, or @p off at or past its end.
 */
kode_status_t fck_drv_smp_request_upload(fck_drv_smp_request_t *r, uint8_t seq, const uint8_t *img, size_t len,
                                 size_t off, size_t *took);

/** @brief Mark the image whose hash is @p hash for one boot on trial: the
 *         next reset swaps it in, and the one after swaps it back unless it
 *         was confirmed. */
kode_status_t fck_drv_smp_request_test(fck_drv_smp_request_t *r, uint8_t seq, const uint8_t *hash,
                               size_t hash_len);

/** @brief Make the running image the one that stays. */
kode_status_t fck_drv_smp_request_confirm(fck_drv_smp_request_t *r, uint8_t seq);

/** @brief OS reset. */
kode_status_t fck_drv_smp_request_reset(fck_drv_smp_request_t *r, uint8_t seq);

/**
 * @brief The request as it goes on the wire.
 * @return The bytes written to @p out, every line ended; 0 when they do not
 *         fit in @p cap (FCK_DRV_SMP_WIRE_MAX always does).
 */
size_t fck_drv_smp_frame(const fck_drv_smp_request_t *r, uint8_t *out, size_t cap);

/* ---- Answers ---------------------------------------------------------------- */

/** @brief What reads the wire back into packets. Zero it, or use
 *         fck_drv_smp_receive_init(), before the first byte. */
typedef struct {
    uint8_t  line[FCK_DRV_SMP_LINE_MAX + 1U];
    size_t   ln;
    bool     over;       /* the line is longer than any frame: skipped     */
    uint8_t  prev;       /* the byte before, for a piece's two start bytes */
    uint8_t  acc[FCK_DRV_SMP_PACKET_MAX + 4U];
    size_t   an;
    bool     in_pkt;
    uint32_t dropped;    /**< Frames dropped: bad base64, a CRC that does
                          *   not match, a packet too long, a piece out of
                          *   order.                                        */
} fck_drv_smp_receive_t;

/** @brief Start reading, with nothing kept. */
void fck_drv_smp_receive_init(fck_drv_smp_receive_t *rx);

/**
 * @brief Read one byte of the wire.
 * @return The packet's length, header included, when this byte ended one
 *         whose CRC checks: it is copied to @p out. 0 otherwise.
 */
size_t fck_drv_smp_receive_byte(fck_drv_smp_receive_t *rx, uint8_t b, uint8_t *out, size_t cap);

/* ---- The text between the packets ------------------------------------------------ */

/** The longest line of text kept; the rest of a longer one is dropped. It holds an image's
 *  `#NRFINFO` up to its `devid=`, `reset=` and `uptime_ms=` (a 63-character line cut the
 *  `devid=` short). */
#define FCK_DRV_SMP_TEXT_MAX    95U

/** @brief What reads the wire back into the text a console prints around
 *         the packets. Zero it, or use fck_drv_smp_text_init(), before the
 *         first byte. */
typedef struct {
    char    line[FCK_DRV_SMP_TEXT_MAX + 1U];
    size_t  n;
    uint8_t prev;     /* the byte before, for a frame's two start bytes */
    uint8_t esc;      /* 1 after ESC, 2 inside an ESC [ sequence        */
    bool    frame;    /* the line holds a packet's piece                */
} fck_drv_smp_text_t;

/** @brief What a line of text was, to an nRF that has just been reset. */
typedef enum {
    FCK_DRV_SMP_LINE_NONE = 0,    /**< No line ended here, or one with nothing
                                   *   to say: empty, a packet's piece, or
                                   *   Zephyr's "*** Using", which MCUboot and
                                   *   an image both print.                 */
    FCK_DRV_SMP_LINE_TEXT,        /**< A line the image printed.            */
    FCK_DRV_SMP_LINE_BOOTLOADER,  /**< One of MCUboot's: it names it, in any
                                   *   case ("*** Booting MCUboot",
                                   *   "<inf> mcuboot:").                   */
    FCK_DRV_SMP_LINE_BANNER,      /**< Zephyr's "*** Booting" from an image:
                                   *   one started.                         */
} fck_drv_smp_line_t;

/** @brief Start reading, with nothing kept. */
void fck_drv_smp_text_init(fck_drv_smp_text_t *t);

/**
 * @brief Read one byte of the wire as text.
 *
 * ANSI's escapes, control bytes and bytes past ASCII are dropped one by one,
 * so a stray byte from a reset does not spoil the line it falls in; so are a
 * shell's "uart:~$ " prompt at the start of a line and spaces at both ends.
 * A line that holds a packet's two start bytes is a packet's piece, and says
 * nothing.
 *
 * @param out Where a line the image printed goes, cut to @p cap - 1 bytes;
 *            written for FCK_DRV_SMP_LINE_TEXT only.
 * @return What the line this byte ended was; FCK_DRV_SMP_LINE_NONE when it
 *         ended none.
 */
fck_drv_smp_line_t fck_drv_smp_text_byte(fck_drv_smp_text_t *t, uint8_t b, char *out, size_t cap);

/** @brief One slot of an image state answer. */
typedef struct {
    int8_t  slot;                      /**< -1 when the entry had none.     */
    char    version[24];               /**< "1.0.0".                        */
    uint8_t hash[FCK_DRV_SMP_HASH_MAX];
    uint8_t hash_len;                  /**< 0 when it had none.             */
    bool    active;                    /**< Running now.                    */
    bool    confirmed;                 /**< Stays across resets.            */
    bool    pending;                   /**< Swapped in at the next reset.   */
    bool    bootable;
} fck_drv_smp_image_t;

/** @brief An answer, read. */
typedef struct {
    uint8_t         op;          /**< The request's op + 1.                 */
    uint16_t        group;
    uint8_t         seq;
    uint8_t         id;
    bool            has_rc;
    int32_t         rc;          /**< 0 done; mcumgr's error otherwise. Both
                                  *   rc and err.rc (SMP version 2) are read. */
    bool            has_off;
    uint32_t        off;         /**< Upload: where the next piece goes.    */
    char            echo[65];    /**< Echo: what came back.                 */
    uint8_t         images;      /**< State: the entries read.              */
    fck_drv_smp_image_t image[FCK_DRV_SMP_IMAGES_MAX];
} fck_drv_smp_response_t;

/**
 * @brief Read an answer packet.
 *
 * Only what this file names is kept; any other key is skipped, whatever it
 * holds. The indefinite-length maps zcbor writes are read, and so is the
 * outermost one MCUboot's serial recovery leaves without its end.
 *
 * @return false for a packet shorter than its header, a header whose length
 *         is not the CBOR's, or CBOR that does not parse; @p out is then
 *         left as it was.
 */
bool fck_drv_smp_parse(const uint8_t *pkt, size_t n, fck_drv_smp_response_t *out);

/** @brief Whether @p rsp is the answer to @p req: op + 1, and the same
 *         group, sequence number and id. */
bool fck_drv_smp_answers(const fck_drv_smp_response_t *rsp, const fck_drv_smp_request_t *req);

/** @brief The entry a state answer gives for @p slot; NULL when it gives
 *         none. */
const fck_drv_smp_image_t *fck_drv_smp_slot(const fck_drv_smp_response_t *rsp, int slot);

/* ---- The image ---------------------------------------------------------------- */

/**
 * @brief The hash an MCUboot image carries for itself, as an image state
 *        answer names its slots: its SHA-512, SHA-384 or SHA-256 TLV.
 * @return false for what is not an MCUboot image, or carries no hash.
 */
bool fck_drv_smp_image_hash(const uint8_t *img, size_t len, const uint8_t **hash, size_t *hash_len);

/** @brief How an MCUboot decides whether to boot an image. */
typedef enum {
    FCK_DRV_SMP_LOADER_HASH = 0,  /**< By its SHA-256 alone, no key: the
                                   *   module's one-slot MCUboot (fck_bsp.h).
                                   *   It answers SMP's echo.                */
    FCK_DRV_SMP_LOADER_SIGNED,    /**< By an ED25519 signature over its
                                   *   SHA-512: a loader that answers no
                                   *   echo (fck_bsp.h, the two-slot map).   */
} fck_drv_smp_loader_t;

/**
 * @brief Whether @p loader would boot the image, by the TLVs it carries:
 *        the hash that loader computes, and its signature or none.
 *
 * An image for one loader uploads into the other and is then ignored: the
 * one-slot MCUboot computes a SHA-256 an image signed for a signing loader
 * does not carry, and that one wants a signature an image taken by its hash
 * lacks. With one slot there is no old image to fall back on, so the nRF
 * would stay in recovery: asked before a byte is uploaded. No signature is
 * verified here.
 *
 * @return false for what is not an MCUboot image, one with no hash, or one
 *         the loader would not take.
 */
bool fck_drv_smp_image_suits(const uint8_t *img, size_t len, fck_drv_smp_loader_t loader);

/** @brief What fck_drv_smp_image_check() found that keeps an image from
 *         starting in a slot, or that it found nothing. */
typedef enum {
    FCK_DRV_SMP_IMAGE_FITS = 0,      /**< Nothing it says of itself rules it out.  */
    FCK_DRV_SMP_IMAGE_NOT_MCUBOOT,   /**< Shorter than MCUboot's 32-byte header, or
                                      *   another magic.                         */
    FCK_DRV_SMP_IMAGE_SIZES,         /**< The header's sizes run past the bytes
                                      *   given, or leave no vector table.       */
    FCK_DRV_SMP_IMAGE_LARGER,        /**< Larger than MCUboot boots from the slot. */
    FCK_DRV_SMP_IMAGE_STACK,         /**< Its first stack pointer is not in the
                                      *   chip's RAM.                            */
    FCK_DRV_SMP_IMAGE_ADDRESS,       /**< Its reset handler, its NMI or its hard
                                      *   fault handler is not inside its own code
                                      *   once in the slot: built for another
                                      *   place in the flash.                    */
    FCK_DRV_SMP_IMAGE_MAP,           /**< Its FCKMAP tag names another address
                                      *   than where it would lie
                                      *   (fck_drv_smp_image_map()): the caller's
                                      *   to say, fck_drv_smp_image_check() never
                                      *   returns it.                            */
    FCK_DRV_SMP_IMAGE_LOADER,        /**< The MCUboot that answered would not boot
                                      *   it (fck_drv_smp_image_suits()): the
                                      *   caller's to say, too.                  */
} fck_drv_smp_image_check_t;

/**
 * @brief Whether an MCUboot image can start from a slot, by what it says of
 *        itself.
 *
 * MCUboot jumps to the slot's start plus the image's header, where an Arm
 * image keeps its vector table: the first word is its stack pointer, the
 * next ones its handlers, which point inside its own code when it was built
 * for that address. One built further from the slot than its own code is
 * long fails here: an image linked at 0, with no MCUboot in mind, or for
 * another chip's flash. One built near the slot passes, its handlers still
 * inside its code: the five images looked at, 68 to 320 KB of code
 * built for 0xE800 or 0x10800, pass at 0xC000, 0xE000 and 0x10000 alike.
 * fck_drv_smp_image_holds() tells those apart, and only running one says for
 * sure. No signature is checked here.
 *
 * @param img       The image, header first.
 * @param len       Its length.
 * @param slot      Where the slot starts in the chip's flash.
 * @param image_max The largest image MCUboot boots from it: less than the
 *                  slot, which keeps MCUboot's trailer (FCK_BSP_NRF_IMAGE_MAX).
 * @param ram       Where the chip's RAM starts.
 * @param ram_size  Its size.
 * @return The first thing found, in the enum's order; FCK_DRV_SMP_IMAGE_FITS
 *         when there is none.
 */
fck_drv_smp_image_check_t fck_drv_smp_image_check(const uint8_t *img, size_t len, uint32_t slot,
                                                  uint32_t image_max, uint32_t ram,
                                                  uint32_t ram_size);

/**
 * @brief Where the image says it was linked to run: the address in its
 *        FCKMAP tag, twelve bytes fck-nrf keeps in its code (the magic
 *        46 43 4B 4D 41 50 00 01, then the address of its vector table,
 *        32 bits little endian) and prints as map= in its #NRFINFO.
 *
 * An image whose tag names another address than where it will lie
 * (FCK_BSP_NRF_IMAGE_START) was built for another map: it would be taken by
 * its hash, started, and hang without a word. One with no tag - any firmware
 * but fck-nrf from 0.1.0 - says nothing here, and
 * fck_drv_smp_image_holds() is asked instead.
 *
 * @param address Out: the tag's address, on true.
 * @return false for what is not an MCUboot image, or carries no tag.
 */
bool fck_drv_smp_image_map(const uint8_t *img, size_t len, uint32_t *address);

/**
 * @brief Whether the image's code holds @p word, as a 32-bit little-endian
 *        value at an address that is a multiple of 4.
 *
 * Code that sets the Arm vector table register writes its own table's
 * address, so an image built for a slot holds the slot's start plus its
 * header (Zephyr's relocate_vector_table(), CMSIS's SystemInit): 0x0000E800
 * for the module's slot, 0x00010800 for a slot at 0x10000. An image that
 * sets the register another way holds neither, so this warns and does not
 * refuse.
 *
 * @return false for what is not an MCUboot image, or does not hold it.
 */
bool fck_drv_smp_image_holds(const uint8_t *img, size_t len, uint32_t word);

#ifdef __cplusplus
}
#endif

#endif /* FCK_DRV_SMP_H_ */
