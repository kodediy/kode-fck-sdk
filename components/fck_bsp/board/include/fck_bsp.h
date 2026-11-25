/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_bsp.h
 * @brief The F*CK module (KDFCK001 rev 1.0), described: what is wired where.
 *
 * Every number about the module lives here and nowhere else, the way every
 * number about the Kode Dot lives in kode-sdk's kbsp. This header describes;
 * it does nothing. A driver that needs a position, an address, a bit or a
 * flash offset names it from here (tools/check_layers.sh, rule 3).
 *
 * The module's J401 meets the Dot's J3 contact for contact, so its fourteen
 * signals are the header's fourteen positions in order. The P4 GPIO behind a
 * position belongs to the Dot, and an app learns it from kode_sys_exp_lend(),
 * never from here: these are the only numbers the module's own wiring adds.
 *
 * The values come from the KDFCK001 rev 1.0 schematic, the chips'
 * datasheets and the module's own firmware.
 */
#ifndef FCK_BSP_H_
#define FCK_BSP_H_

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The header: J3 positions, 1 to 14 ------------------------------------
 *
 * The KDFCK001 rev 1.0 schematic, sheets 3 and 5. After its reset the
 * CC1101 puts its default 135 kHz clock on GDO0. */
#define FCK_BSP_POSITION_C5_EN       1U   /**< The C5's reset, active low. Pulled
                                      *   down or let go, never driven high:
                                      *   R502 (10 k to 3.3 V) and C503 (1 uF)
                                      *   bring it up on their own.            */
#define FCK_BSP_POSITION_C5_BOOT     2U   /**< The C5's boot strap: low at reset is
                                      *   the ROM loader. Pulled down or let
                                      *   go, like EN.                          */
#define FCK_BSP_POSITION_C5_RX       3U   /**< The C5's UART0 RXD: the P4 talks here. */
#define FCK_BSP_POSITION_CC_MISO     4U   /**< The CC1101's SO.                       */
#define FCK_BSP_POSITION_C5_TX       5U   /**< The C5's UART0 TXD: the P4 listens here. */
#define FCK_BSP_POSITION_CC_CLK      6U   /**< The CC1101's SCLK.                     */
#define FCK_BSP_POSITION_CC_MOSI     7U   /**< The CC1101's SI.                       */
#define FCK_BSP_POSITION_NRF_TX      8U   /**< The nRF's UART out: the P4 listens here. */
#define FCK_BSP_POSITION_CC_CS       9U   /**< The CC1101's CSn.                      */
#define FCK_BSP_POSITION_NRF_RX      10U  /**< The nRF's UART in: the P4 talks here.  */
#define FCK_BSP_POSITION_CC_GDO0     11U  /**< The CC1101's GDO0.                     */
#define FCK_BSP_POSITION_IO_INT      12U  /**< The expander's interrupt, open drain:
                                      *   an input, never driven.               */
#define FCK_BSP_POSITION_I2C_SCL     13U  /**< Pulled up on the module, 2.2 k.        */
#define FCK_BSP_POSITION_I2C_SDA     14U  /**< Pulled up on the module, 2.2 k.        */
#define FCK_BSP_POSITIONS       14U  /**< The module takes the whole header.     */

/** The name the module's positions are lent under (kode_sys_exp_lend()). */
#define FCK_BSP_LOAN_TAG        "fck"

/* ---- The expander: a TCA6408A on the module's I2C pair ---------------------
 *
 * It is the module's identity as well as its switchboard: a Mesh module has
 * nothing at 0x21, so an address probe there tells the two apart and is
 * harmless to both. */
#define FCK_BSP_EXPANDER_ADDRESS        0x21U    /**< ADDR tied high on the module.      */
#define FCK_BSP_I2C_HZ          400000U

#define FCK_BSP_EXPANDER_REGISTER_INPUT    0x00U   /**< The level of the eight pins.       */
#define FCK_BSP_EXPANDER_REGISTER_OUTPUT   0x01U   /**< What an output drives. Power-on 0xFF. */
#define FCK_BSP_EXPANDER_REGISTER_POLARITY 0x02U   /**< A 1 inverts how that input reads.
                                          *   Power-on 0x00.                     */
#define FCK_BSP_EXPANDER_REGISTER_CONFIG   0x03U   /**< A 1 makes the pin an input.
                                          *   Power-on 0xFF.                     */

/** @name The expander's pins, as the schematic names them (bit n = Pn) */
/** @{ */
#define FCK_BSP_EXPANDER_C5_WP_HOST  0U   /**< From the C5: a wake line, an input.    */
#define FCK_BSP_EXPANDER_HOST_WP_C5  1U   /**< To the C5: a wake line.                */
#define FCK_BSP_EXPANDER_GNSS_RST    2U   /**< The GNSS receiver's reset, active low. */
#define FCK_BSP_EXPANDER_CC_SW0      3U   /**< SW0 of the sub-GHz antenna switches.   */
#define FCK_BSP_EXPANDER_HOST_WP_NRF 4U   /**< To the nRF: a wake line, pulled up by
                                      *   R703. Left an input.                  */
#define FCK_BSP_EXPANDER_NRF_WP_HOST 5U   /**< From the nRF: a wake line, an input.   */
#define FCK_BSP_EXPANDER_NRF_RST     6U   /**< The nRF's reset, active low, pulled up
                                      *   by R701.                              */
#define FCK_BSP_EXPANDER_CANARY      7U   /**< No function, only R405's pull-up. Its
                                      *   polarity bit is the power-loss mark,
                                      *   see below.                            */
/** @} */

/**
 * @brief The pins the module holds high with a 10 k resistor to its 3V3: P0
 *        (R504), P1 (R501), P4 (R703), P5 (R702) and P6 (R701), bit n for Pn.
 *
 * From the KDFCK001 rev 1.0 schematic. P7 has one too
 * (R405), but its polarity bit is the power-loss mark and may invert how it
 * reads, so it is left out. On an F*CK at least one of these reads high in
 * any state the module can be left in: all five low would take the C5
 * pulling P0 low, the host driving P1 low, the nRF's wake line driven low -
 * this SDK leaves it an input - the nRF pulling P5 low and the nRF held in
 * reset, all at once. A chip at the expander's address that reads none of
 * them high is not the expander.
 */
#define FCK_BSP_EXPANDER_PULLUPS_MASK ((unsigned)((1U << FCK_BSP_EXPANDER_C5_WP_HOST) \
                                             | (1U << FCK_BSP_EXPANDER_HOST_WP_C5) \
                                             | (1U << FCK_BSP_EXPANDER_HOST_WP_NRF) \
                                             | (1U << FCK_BSP_EXPANDER_NRF_WP_HOST) \
                                             | (1U << FCK_BSP_EXPANDER_NRF_RST)))

/**
 * @brief The bit SW0's level is written to: P3.
 *
 * The rev 1.0 schematic puts SW0 on P3, and the datasheet, the BOM, the
 * footprint and the copper agree. P4 is the nRF's wake line, not a control
 * of the switches, and this SDK leaves it to R703.
 */
#define FCK_BSP_EXPANDER_SW0_MASK    ((unsigned)(1U << FCK_BSP_EXPANDER_CC_SW0))

/**
 * @brief The power-loss mark: P7's polarity bit.
 *
 * P7 drives nothing, and a polarity bit only changes how the input register
 * reads that pin. The owner sets it once it has configured the module; a
 * register read that finds it clear means the expander went back to its
 * power-on values, so the whole module restarted.
 */
#define FCK_BSP_EXPANDER_CANARY_BIT  (1U << FCK_BSP_EXPANDER_CANARY)

/* ---- The sub-GHz antenna path ----------------------------------------------
 *
 * Two switches pick one of three legs. SW1 is the CC1101's GDO2 held at a
 * static level (IOCFG2 0x6F high, 0x2F low); SW0 is the expander, as above.
 * The two BGS13SN8 are wired crossed - U603 takes V1 from SW1 and V2 from
 * SW0, U604 the other way round - so (SW1, SW0) = (1, 0) joins U603.RF1 to
 * U604.RF2 (the top leg), (0, 1) U603.RF2 to U604.RF1 (the middle one) and
 * (1, 1) the two RF3 (the bottom one); (0, 0) isolates.
 *
 * Each band has its leg, to receive and to send: 315 MHz the top one,
 * 433 MHz the middle one, with its ladder filter, and 868 and 915 MHz the
 * bottom one. */
typedef enum {
    FCK_BSP_LEG_TOP = 0,   /**< 300-348 MHz: 315.             (SW1, SW0) = (1, 0) */
    FCK_BSP_LEG_MIDDLE,    /**< 387-464 MHz: 433, the ladder. (SW1, SW0) = (0, 1) */
    FCK_BSP_LEG_BOTTOM,    /**< 779-928 MHz: 868 and 915.     (SW1, SW0) = (1, 1) */
} fck_bsp_leg_t;

/** Each leg's two switch levels, as the table above gives them. */
#define FCK_BSP_LEG_SW1(leg)       ((leg) != FCK_BSP_LEG_MIDDLE)
#define FCK_BSP_LEG_SW0(leg)       ((leg) != FCK_BSP_LEG_TOP)

/** Up to here the top leg, 315's; above it the middle or the bottom one. */
#define FCK_BSP_LEG_TOP_MAX_HZ     348000000U
/** Up to here the middle leg, 433's; above it the bottom one. No band the
 *  module is used on lies between 464 and 779 MHz, so the exact boundary is
 *  this SDK's choice, not the board's. */
#define FCK_BSP_LEG_MIDDLE_MAX_HZ  600000000U

/**
 * Where SW0 rests when no leg is in use, from the moment the module is found:
 * high. It is the level of 433's leg
 * and of 868 and 915's, so those two need only SW1, a pin of the CC1101's
 * own. 315's leg needs SW0 low, and so the expander: a GNSS receiver whose
 * firmware holds the module's bus takes the expander with it, and meanwhile
 * 433, 868 and 915 can still be had, 315 not.
 */
#define FCK_BSP_LEG_SW0_REST       1U   /**< SW0's level at rest: high. */

#define FCK_BSP_CC1101_IOCFG2_HIGH  0x6FU   /**< GDO2 static high: SW1 = 1.          */
#define FCK_BSP_CC1101_IOCFG2_LOW   0x2FU   /**< GDO2 static low:  SW1 = 0.          */

/* ---- Where the module may transmit -----------------------------------------
 *
 * Listening is legal on any frequency the CC1101 tunes; transmitting is not.
 * The module sends only in two European SRD bands: 433.05-434.79 MHz
 * through the middle leg and 868.0-868.6 MHz through the bottom one.
 * 315 MHz and 915 MHz belong to other regions and are received, never
 * sent. These are the bands' limits, not the board's: a frequency is put to fck_drv_subghz_scan_may_transmit() and the
 * facade refuses the rest. */
#define FCK_BSP_TX_433_MIN_HZ   433050000U
#define FCK_BSP_TX_433_MAX_HZ   434790000U
#define FCK_BSP_TX_868_MIN_HZ   868000000U
#define FCK_BSP_TX_868_MAX_HZ   868600000U

/* ---- The ESP32-C5 ----------------------------------------------------------- */
#define FCK_BSP_C5_BAUD         115200U  /**< Its firmware, and its ROM at reset.   */
#define FCK_BSP_C5_BOOTLOADER   0x2000U  /**< Where each image goes in its flash. */
#define FCK_BSP_C5_PARTITIONS   0x8000U
#define FCK_BSP_C5_APP          0x10000U

/* ---- The nRF54L15 ---------------------------------------------------------- *
 *
 * MCUboot (NCS v3.4.2) with one slot and no key: an image is taken by its
 * SHA-256 alone, so anyone builds one it will boot, and it is written
 * through MCUboot's serial recovery and by nothing else. MCUboot closes its
 * own region before it starts the image, and it answers SMP's echo, which is
 * how it is told from the loaders below. The map is the firmware's
 * pm_static.yml (firmware/fck-nrf); between MCUboot's end and the slot
 * nothing is kept, and 0x165000-0x17D000 is the image's own to use.
 * MCUboot is written over SWD only, in the factory.
 *
 * TEMPORARY: pre-production modules. Remove when the production modules ship.
 * The first modules carry other loaders, which answer no echo and take only
 * an image signed with Nordic's public development key (ED25519 and
 * SHA-512): NCS v3.3.0's with two slots, the first where the slot is now
 * (FCK_BSP_NRF_FIRST_*); and another with one slot at 0x10000 and its code
 * at 0x10800. That one boots any image signed with the same key, so an
 * image built for the map below is taken there and hangs, saying nothing:
 * fck_rescue_probe() tells. */
#define FCK_BSP_NRF_BAUD             115200U
#define FCK_BSP_NRF_MCUBOOT          0x0U      /**< MCUboot, up to...              */
#define FCK_BSP_NRF_MCUBOOT_END      0x7C00U   /**< ...here: the 31 KB the nRF54L15
                                                *   can close to an image.         */
#define FCK_BSP_NRF_SLOT0            0xE000U   /**< The image's header goes here. */
#define FCK_BSP_NRF_SLOT0_SIZE       0x157000U /**< Up to 0x165000.               */
/** The largest image MCUboot takes into the slot, its header and TLVs
 *  included: the slot itself. With one slot there is no swap, so no trailer
 *  is kept (MCUboot's boot_serial.c and single_loader.c, v3.4.2). */
#define FCK_BSP_NRF_IMAGE_MAX        FCK_BSP_NRF_SLOT0_SIZE
#define FCK_BSP_NRF_IMAGE_MAGIC        0x96F3B83DU
#define FCK_BSP_NRF_IMAGE_HEADER_SIZE     0x800U
/** Where an image's code starts once in the slot, the address it is linked
 *  for: what the firmware prints as map= and carries in its FCKMAP tag. */
#define FCK_BSP_NRF_IMAGE_START      (FCK_BSP_NRF_SLOT0 + FCK_BSP_NRF_IMAGE_HEADER_SIZE)
/** TEMPORARY: pre-production modules. The first modules' two-slot map: slot
 *  0 where it is now and shorter, and the largest image their MCUboot boots
 *  from it. Swap using move keeps its trailer in the slot (256 sectors'
 *  worth, 0x3050 bytes, in whole 4 KB sectors) and one sector free:
 *  0xAB000 - 0x4000 - 0x1000 (bootutil's app_max_size()). */
#define FCK_BSP_NRF_FIRST_SLOT0_SIZE 0xAB000U
#define FCK_BSP_NRF_FIRST_IMAGE_MAX  0xA6000U
/** After a reset MCUboot listens for SMP this long before it boots slot 0. */
#define FCK_BSP_NRF_RECOVERY_MS      3000U
/** How long a firmware may take from a reset to its first line, through the
 *  first modules' two-slot MCUboot. The one-slot MCUboot's window is the
 *  same 3 s. */
#define FCK_BSP_NRF_BOOT_TO_FIRMWARE_MS 6300U
/** The longest SMP upload chunk the recovery takes. */
#define FCK_BSP_NRF_SMP_CHUNK        512U
/** The nRF54L15's RAM, where an image's first stack pointer points (the
 *  datasheet: 256 KB). */
#define FCK_BSP_NRF_RAM              0x20000000U
#define FCK_BSP_NRF_RAM_SIZE         0x40000U

/* ---- The GNSS receiver ----------------------------------------------------- *
 *
 * A Minew ME31GR02 whose UART reaches the C5 only (C5 IO4 RX, IO5 TX, 115200):
 * the P4 hears it through the C5 firmware's relay. Its reset is the expander's
 * P2. A module with the standard firmware drives the I2C pair's SDA high for
 * about 420 ms after its reset is released,
 * which takes the expander - and with it every switch on the module - off
 * the bus. The owner keeps it in reset unless an app asked for a fix. */
#define FCK_BSP_GNSS_SDA_BUSY_MS     420U
/** The receiver's own I2C address (0xA8 in 8 bits), next to the expander. */
#define FCK_BSP_GNSS_I2C_ADDRESS        0x54U

#ifdef __cplusplus
}
#endif

#endif /* FCK_BSP_H_ */
