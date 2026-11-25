/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_world.h
 * @brief The model of the module and its header, for the simulation suite.
 *
 * The owner (fck_aos.c) and the facade (fck.c, fck_flash.c, fck_wifi.c,
 * fck_gnss.c) run as they are linked into an app, on ESP-IDF's FreeRTOS for
 * the host. Under them, fake_world.c answers for the hardware: J3's lines, the
 * I2C pair and what sits on it, the two UARTs, the CC1101, the C5's ROM
 * loader and the nRF's DFU. A case plugs a module in, scripts what its chips
 * say, and looks at what the owner did to it: the wire log lists every I2C
 * transaction that reached a chip, so "nothing was written" is a thing the
 * suite can check.
 *
 * The model is a few hundred lines of C, not a simulation of electronics: it
 * knows the facts fck_bsp.h states and nothing else.
 */
#ifndef SIM_H_
#define SIM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kode_defs.h"

#include "fck_bsp.h"
#include "fck_aos.h"
#include "fck_drv_c5_rom.h"
#include "fck_drv_nrf_dfu.h"
#include "fck_drv_subghz_modem.h"
#include "fck_drv_system.h"

/* ---- What sits at 0x21 ---------------------------------------------------------- */

typedef enum {
    SIM_DEV_NONE = 0,    /**< Nothing: the address is NAKed, cleanly.            */
    SIM_DEV_STUCK,       /**< Something holds the pair and nothing acknowledges. */
    SIM_DEV_PCF8574,     /**< A register-less 8-bit port: it takes bytes as pins. */
    SIM_DEV_LOOKALIKE,   /**< Reads its pins like the expander, keeps no POLARITY. */
    SIM_DEV_EXPANDER,    /**< The F*CK's TCA6408A.                               */
} sim_dev_t;

/** One transaction that reached a chip (or the bus). */
typedef enum {
    SIM_W_PROBE = 0,     /**< An address and a stop. val: 0 acknowledged, 1 NAK, 2 failed. */
    SIM_W_READ,          /**< reg is the byte sent first; val what came back.      */
    SIM_W_WRITE,         /**< reg and val.                                         */
    SIM_W_BLIND,         /**< A bit-banged write, push-pull: reg and val.          */
} sim_wire_kind_t;

typedef struct {
    sim_wire_kind_t kind;
    sim_dev_t       dev;     /**< What was there when it happened.                 */
    uint8_t         reg;
    uint8_t         val;
} sim_wire_t;

/* ---- The world ------------------------------------------------------------------- */

/** Once, from the task that runs the cases, before anything else. */
void sim_init(void);

/** Everything unplugged, and every log, script and counter cleared. */
void sim_reset(void);

/** What is plugged in. */
typedef struct {
    sim_dev_t dev;               /**< What sits at 0x21.                            */
    bool      gnss_standard_fw;  /**< The receiver takes SDA 420 ms after it starts. */
    uint32_t  gnss_age_ms;       /**< It has been running this long already.        */
    uint8_t   pins;              /**< What the module holds the expander's pins at
                                  *   when nothing drives them; 0xF7 on the F*CK.   */
} sim_plug_t;

/**
 * Something on the header with the I2C pair held. For SIM_DEV_EXPANDER, that
 * is an F*CK that has just powered up: the expander at its power-on values, the
 * GNSS receiver running, and the two chips about to say what they run.
 */
void sim_plug_ex(const sim_plug_t *p);

/** sim_plug_ex() with a receiver that has just started and the F*CK's pins. */
void sim_plug(sim_dev_t dev, bool gnss_standard_fw);

/** The header empty again. */
void sim_unplug(void);

/** The F*CK lost power for a moment: the expander is at its power-on values,
 *  the receiver runs from now, the chips start again. */
void sim_blip(void);

/** The receiver is out of reset and has long since taken SDA. */
void sim_gnss_take_bus(void);

/** The receiver's firmware is the standard one: it takes SDA 420 ms after it
 *  leaves reset (from the next time it does, if it is in reset now). */
void sim_gnss_standard_fw(bool on);

bool    sim_gnss_running(void);       /**< Out of reset.                         */
bool    sim_bus_dead(void);           /**< The receiver holds SDA.               */
uint8_t sim_exp_out(void);
uint8_t sim_exp_cfg(void);
uint8_t sim_exp_pol(void);
uint8_t sim_pcf_port(void);
uint32_t sim_pcf_writes(void);        /**< Bytes a PCF8574 was sent.             */

/** fck_drv_tca6408a_open() answers KODE_NO_RESOURCES while @p none. */
void sim_no_i2c_controller(bool none);

/* ---- The chips ------------------------------------------------------------------- */

/** The chips the model has. */
#define SIM_C5   0
#define SIM_NRF  1

/**
 * What the chip prints when it starts, and answers `info` with: a whole line
 * without its ending (`#C5INFO fw=...`). NULL: it prints nothing and answers
 * nothing. It prints it now if it is running.
 */
void sim_firmware(int chip, const char *banner);

/**
 * Like sim_firmware(), but the chip does not print its banner when it starts: it
 * only answers `info` with it. A firmware whose banner was lost, or that the owner
 * came to after it started.
 */
void sim_firmware_quiet(int chip, const char *banner);

/** The 2.0 firmware's banner, EU limits in force. */
#define SIM_C5_BANNER_20 \
    "#C5INFO fw=c5_fck_agent ver=2.0 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:01 reg=EU lim=ok"
/** The 2.4 firmware's banner, under its own name: `frames`. */
#define SIM_C5_BANNER_24 \
    "#C5INFO fw=fck-c5 ver=2.4 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:03 reg=EU lim=ok"
/** The 2.3 firmware's: no `frames` yet (that name of 2.3 is another branch's). */
#define SIM_C5_BANNER_23 \
    "#C5INFO fw=fck-c5 ver=2.3 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:03 reg=EU lim=ok"
/** The 2.2 firmware's: the BLE advertisement, and no `frames` yet. */
#define SIM_C5_BANNER_22 \
    "#C5INFO fw=fck-c5 ver=2.2 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:03 reg=EU lim=ok"
/** The nRF's firmware 2.0. */
#define SIM_NRF_BANNER_20 \
    "#NRFINFO fw=nrf_fck_agent ver=2.0 devid=0123456789ABCDEF reset=0x0 uptime_ms=1"
/** The nRF's firmware 1.0: no EU limits of its own. */
#define SIM_NRF_BANNER_10 "#NRFINFO fw=nrf_fck_agent ver=1.0 devid=0123456789ABCD15"
/** A firmware before 2.0: no market, no limits. */
#define SIM_C5_BANNER_15 \
    "#C5INFO fw=c5_fck_agent ver=1.5 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:02"
/** A firmware before 1.1: no `chan`, no `ble scan`. */
#define SIM_C5_BANNER_10 "#C5INFO fw=c5_fck_agent ver=1.0"
/** fck-c5 0.1.0: what it does is in caps=, and its version decides nothing. */
#define SIM_C5_BANNER_010 \
    "#C5INFO fw=fck-c5 ver=0.1 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:01 reg=EU lim=ok " \
    "caps=scan,chan,ble,gnss,wp,gtx,health uptime_ms=812 rst=poweron"
/** fck-c5 0.2.0: 0.1.0 with what each advertiser sent and `frames`. */
#define SIM_C5_BANNER_020 \
    "#C5INFO fw=fck-c5 ver=0.2 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:01 reg=EU lim=ok " \
    "caps=scan,chan,ble,bleadv,frames,gnss,wp,gtx,health uptime_ms=812 rst=poweron"
/** fck-nrf 0.1.0. */
#define SIM_NRF_BANNER_010 \
    "#NRFINFO fw=fck-nrf ver=0.1.0 devid=0123456789ABCDEF reset=0x00000001 uptime_ms=3100 map=0x0000E800 " \
    "caps=spec,off,power,echo,ping,sniff,scfg,saddr,eu,done,wdt,echoidle,smp"
/** Another firmware. */
#define SIM_C5_BANNER_OWN "#C5INFO fw=my_firmware ver=0.3"

/**
 * A reply: a command that begins with @p prefix is answered with @p response
 * (lines ended with \r\n, the #DONE among them) @p delay_ms later. The first
 * script that matches answers. A command no script matches is answered
 * `#DONE <verb> rc=2` - not understood - and `info` with the banner.
 */
void sim_script(int chip, const char *prefix, const char *response, uint32_t delay_ms);

/** Bytes the chip prints @p delay_ms from now, as if by itself. */
void sim_inject(int chip, uint32_t delay_ms, const char *text);

/** Lines the owner sent the chip that begin with @p prefix ("" for all). */
size_t sim_sent(int chip, const char *prefix);
/** The nth line the owner sent (0-based), "" if there is none. */
const char *sim_sent_line(int chip, size_t n);
void        sim_sent_clear(int chip);
/** Empty lines the chip heard since sim_sent_clear(): one goes before every command. */
size_t sim_blank_lines(int chip);
/** Leave @p text in the chip as a line that never ended, as a Dot that reset half way through
 *  writing one does. */
void sim_half_line(int chip, const char *text);

/** The chip is in reset: it hears and says nothing. */
bool sim_chip_running(int chip);

/** The chip starts afresh and says what it runs @p banner_ms from now. */
void sim_chip_boot(int chip, uint32_t banner_ms);

/** True in the task that runs the cases, false in the owner's. */
bool sim_in_case_task(void);

/** Times the owner took a chip's lines to a reset. */
uint32_t sim_c5_resets(void);

/* ---- What the owner did ----------------------------------------------------------- */

void              sim_wire_clear(void);
size_t            sim_wire_n(void);
const sim_wire_t *sim_wire_at(size_t i);
size_t            sim_wire_count(sim_wire_kind_t kind);
/** Index of the first write (SIM_W_WRITE or SIM_W_BLIND) to anything, or -1. */
int               sim_wire_first_write(void);

void   sim_log_clear(void);

/**
 * The app's side of the events sink, for the cases: @p fn is told every event the owner raises, on
 * the task that raised it (the owner's), like fck.h's events task is. NULL stops it; it returns
 * once a call in flight has returned, so the context can be freed. sim_reset() clears it.
 */
typedef void (*sim_event_fn)(fck_aos_event_t ev, uint32_t arg, void *ctx);
void   sim_on_event(sim_event_fn fn, void *ctx);
bool   sim_log_has(const char *needle);
size_t sim_log_count(const char *needle);

/* ---- The writers ------------------------------------------------------------------ */

/** What fck_drv_c5_rom_write() does, and what it was given. */
typedef struct {
    kode_status_t       st;
    fck_drv_c5_rom_report_t rep;      /**< Returned as it is.                          */
    uint32_t            ms;       /**< How long it takes.                          */
    uint32_t            boot_ms;  /**< The C5 says what it runs this long after, if
                                   *   rep.reset.                                  */
    /* Recorded: */
    uint32_t            calls;
    size_t              n;
    fck_drv_c5_rom_part_t   parts[FCK_DRV_C5_ROM_PARTS_MAX];
    uint32_t            baud;
    bool                in_owner; /**< It ran in the owner's task, not the caller's. */
    bool                ok_args;  /**< What fck_drv_c5_rom_parts_ok() says.            */
    kode_status_t       probe_st; /**< What fck_drv_c5_rom_probe() returns,       */
    fck_drv_c5_rom_report_t probe; /**< ...and what it says, as it is.          */
    fck_drv_c5_boot_t   heard;    /**< What fck_drv_c5_rom_listen() hears. It
                                   *   resets the C5, which then answers as the
                                   *   model has it.                            */
    uint32_t            heard_ms; /**< How long it listens.                     */
    uint32_t            probes;
    uint32_t            listens;
    uint32_t            listen_limit_ms; /**< The most it was told to wait.     */
} sim_rom_t;

typedef struct {
    kode_status_t        st;
    fck_drv_nrf_dfu_report_t rep;
    uint32_t             ms;
    uint32_t             boot_ms;
    kode_status_t        probe_st; /**< What fck_drv_nrf_dfu_probe() returns,  */
    fck_drv_nrf_dfu_probe_t probe; /**< ...and what it says, as it is. With
                                    *   probe.reset the nRF starts again, and
                                    *   has printed its banner by the time it
                                    *   returns, as the real probe leaves it. */
    /* Recorded: */
    uint32_t             recovers;
    uint32_t             probes;
    const uint8_t       *img;
    size_t               len;
    bool                 had_tca;
    bool                 in_owner;
} sim_dfu_t;

extern sim_rom_t sim_rom;
extern sim_dfu_t sim_dfu;

/** Both writers back to doing what works: OK, in 50 ms, the chip saying what
 *  it runs 300 ms later, an image that names where it starts in slot 0; and
 *  the probe resetting the nRF and finding nothing. */
void sim_writers_reset(void);

/* ---- The CC1101's sends, and the clock ------------------------------------------ */

/** The runs of the last fck_drv_cc1101_send() kept, the first of them. */
#define SIM_CC1101_RUNS_MAX 1024U

/** What reached the radio, and what it answers. Cleared with sim_cc1101_reset(). */
typedef struct {
    kode_status_t st;            /**< What the next send answers; KODE_OK.           */
    uint32_t      sends;         /**< fck_drv_cc1101_send() calls.                   */
    uint32_t      packet_sends;  /**< fck_drv_cc1101_packet_send() calls.            */
    uint32_t      hz;            /**< The last of either, and what it was given:     */
    uint8_t       repeats;
    size_t        runs;          /**< OOK: how many ...                              */
    int32_t       run[SIM_CC1101_RUNS_MAX]; /**< ... and they, as given.          */
    uint8_t       length;        /**< Packet: its bytes, length and baud.            */
    uint8_t       bytes[FCK_DRV_SUBGHZ_MODEM_LENGTH_MAX];
    uint32_t      baud;
    /* How long a send keeps the carrier on, on the worker, in 5 ms steps; 0
     * for none. fck_drv_cc1101_abort() ends it within a step, as the driver's. */
    uint32_t      play_ms;
    bool          reads_ok;      /**< The reads (level, spectrum, find, record, packets) work and
                                  *   hear nothing, in at most 50 ms; else they refuse (default). */
    bool          on_air;        /**< A send is in its carrier now.                 */
    uint32_t      cut;           /**< Sends fck_drv_cc1101_abort() cut short.       */
    uint32_t      idle_at_ms;    /**< When the carrier last ended (sim_now_ms()).   */
    uint32_t      began_at_ms;   /**< When the last send keyed it.                  */
    uint32_t      listen_starts; /**< fck_drv_cc1101_listen_start() calls.           */
    uint32_t      listens;       /**< fck_drv_cc1101_listen() calls (they hear nothing). */
    uint32_t      listen_hz;     /**< The frequency the last listen_start() was given. */
} sim_cc1101_t;

extern sim_cc1101_t sim_cc1101;

void sim_cc1101_reset(void);

/** The most runs sim_cc1101_hear() keeps. */
#define SIM_CC1101_HEAR_RUNS_MAX 64U

/** The listening's next slice hears this remote (once), on @p hz. */
void sim_cc1101_hear(const int32_t *us, size_t n, uint32_t hz);

/** kode_sys_time_ms() reads this much later from now on: an hour passes at once. */
void sim_clock_skip_ms(uint32_t ms);

/** J3's supply as the owner last asked for it, how many times it was asked, and how many
 *  times the loan was given back. */
bool     sim_exp_power_on(void);
/** EXP_POWER_SIG for an "on" carries @p st from now on (KODE_OK for the model's usual); switching off always works. */
void     sim_exp_power_answers(kode_status_t st);
/** kode_sys_exp_power() answers KODE_NO_RESOURCES the next @p n times (the radio owner's queue is short). */
void     sim_exp_power_busy(uint32_t n);
uint32_t sim_exp_power_calls(void);
/** kode_sys_exp_lend() answers @p st (KODE_EXISTS: the console took J3; KODE_OK for the usual). */
void     sim_exp_lend_answers(kode_status_t st);
/** kode_sys_exp_power() answers KODE_DENIED while @p none: no radio in the image, J3 always powered. */
void     sim_exp_no_radio(bool none);
uint32_t sim_exp_returns(void);

/** When the C5 was last held for a way home and the CC1101 last put to sleep (sim_now_ms()),
 *  and how many times each was since sim_parks_clear(). The CC1101 is also put to sleep when a
 *  module is set up. */
void     sim_parks_clear(void);
uint32_t sim_c5_parks(void);
/** How many times the C5 was held in reset (a rest does that, not a park for good), and when the last was. */
uint32_t sim_c5_holds(void);
uint32_t sim_c5_hold_ms(void);
uint32_t sim_c5_park_ms(void);
uint32_t sim_cc1101_sleeps(void);
uint32_t sim_cc1101_sleep_ms(void);

/** The restart hook the owner registered with fck_drv_system_on_restart(), or NULL. A case calls it
 *  from a task of its own, as esp_restart() does from the task that asks. */
fck_drv_system_fn sim_restart_hook(void);

/** HOME_SIG, EXP_POWER_SIG and the lowpower rounds, delivered on the owner's task as QP/C would.
 *  sim_ao_rest(false) is a round to ASLEEP, (true) a round to OFF; sim_ao_sleep() is the first. */
void sim_ao_home(void);
void sim_ao_sleep(void);
void sim_ao_rest(bool off);
void sim_ao_wake(void);
void sim_ao_exp_power(bool on, uint8_t rc);
/** What fck_ao_kode_is_up() answers (true to begin with). */
void sim_ao_kode_up(bool up);

/** The task of an app's Active Object: fck_ao_is_any_ao_task() is true on it (NULL to clear). */
void sim_ao_mark_task(TaskHandle_t task);
/** While true the owner's task takes no event and runs no tick: an owner that is stuck. */
void sim_ao_stall(bool stalled);
/** While true the owner's queue takes no request: fck_ao_post() answers KODE_NO_RESOURCES. */
void sim_ao_full(bool full);

/* ---- Waiting ----------------------------------------------------------------------- */

uint32_t sim_now_ms(void);

/** Runs @p fn in a task of its own @p ms from now. */
void sim_later(uint32_t ms, void (*fn)(void));
bool     sim_wait_present(uint32_t ms);
bool     sim_wait_absent(uint32_t ms);
bool     sim_wait_for(bool (*cond)(void), uint32_t ms);
/** Until the owner has set up @p gen modules or more (fck_status().generation). */
bool     sim_wait_generation(uint32_t gen, uint32_t ms);
void     sim_sleep(uint32_t ms);

/** The owner started, the world empty and the owner has noticed: the state a
 *  case starts from when it needs nothing plugged. */
void sim_begin_empty(void);

/** An F*CK plugged in and set up, its chips saying what they run (banners given,
 *  or the 2.0 firmware's and the nRF's). The state a case starts from when it
 *  needs a module. */
void sim_begin_fck(const char *c5_banner, const char *nrf_banner);

/** "$<body>*HH" into @p out. */
void sim_nmea(char *out, size_t cap, const char *body);

#endif /* SIM_H_ */
