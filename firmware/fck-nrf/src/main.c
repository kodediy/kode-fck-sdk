/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fck-nrf - the firmware of the nRF54L15 (U701, Minew ME54BS61) on the F*CK module (KDFCK001
 * rev 1.0).
 *
 * Board facts (KiCad netlist, Sept 2026 board):
 *   nRF P2.02 TX -> P4 GPIO21, nRF P2.00 RX <- P4 GPIO20, 115200 8N1 (uart00, 8x baud fix in DT)
 *   P0.00 = NRF_WP_HOST  -> TCA6408A P5 (host input)  : 1 Hz heartbeat toggle (liveness)
 *   P0.02 = HOST_WP_NRF  <- TCA6408A P4 (pull-up R703): not used yet
 *   RST from TCA6408A P6 (pull-up). SWD only on TP703 SWDIO / TP704 SWCLK.
 *
 * The UART carries the Zephyr shell (no prompt, no echo, machine lines start with '#', every
 * command line ends with "#DONE <verb> rc=<n>") and mcumgr SMP over the shell transport, so text
 * commands and smpmgr DFU share the one wire. Radio commands live in rf.c.
 *
 * Zephyr's shell only reads the line. It is given one command, `fck`, set as the shell's root
 * command (CONFIG_SHELL_CMD_ROOT) and taking the rest of the line raw: so every line comes to
 * line_run() below as it was typed, and the words are split and the command is found here. That
 * way no line ends in a text of Zephyr's and without #DONE: not one with a wrong number of
 * arguments, with `-h`, with a word that is no command, or with a quote in it.
 */
#include <ctype.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_wdt.h>
#include <helpers/nrfx_reset_reason.h>
#include <nrfx.h>

#include "fck_nrf.h"

static uint32_t s_resetreas;

atomic_t fck_alive_mark;       /* set by fck_alive(), taken by main() */
static atomic_t s_in_command;  /* the shell's thread is inside line_run() */

uint8_t fck_node_id(void)
{
	return (uint8_t)(NRF_FICR->INFO.DEVICEID[0] & 0xFF);
}

/* ===== #NRFINFO ================================================================== */

/* Where this image was linked to run (fck_nrf.h describes the tag). The address is the
 * linker's own, so it is right whatever placed the image there. It is volatile so that the
 * compiler keeps the object and reads it, and the linker does not drop it. */
static const volatile struct fck_nrf_map_tag s_map_tag = {
	.magic = FCK_NRF_MAP_MAGIC,
	.link_address = (uint32_t)_vector_start,
};

BUILD_ASSERT(sizeof(struct fck_nrf_map_tag) == 12, "the map tag is 8 bytes and an address");

/* The line the firmware says at boot and answers `info` with. The host compares two of these
 * to know whether the chip still runs the same image and has not restarted, leaving out only
 * uptime_ms: every other value must stay the same from boot to boot's end. No counter, no state.
 * It also keeps at most 191 characters of what follows the tag. */
#define INFO_FORMAT                                                                                \
	"#NRFINFO fw=%s ver=%s devid=%08X%08X reset=0x%08X uptime_ms=%u map=0x%08X caps=%s"

BUILD_ASSERT(sizeof("fw=" FCK_NRF_FW " ver=" FCK_NRF_VERSION " devid=0123456789ABCDEF"
		    " reset=0x00000000 uptime_ms=4294967295 map=0x00000000 caps=" FCK_NRF_CAPS) <= 192,
	     "#NRFINFO is longer than the host keeps");

static void info_line(const struct shell *sh)
{
	uint32_t hi = NRF_FICR->INFO.DEVICEID[1];
	uint32_t lo = NRF_FICR->INFO.DEVICEID[0];
	uint32_t up = k_uptime_get_32();
	uint32_t map = s_map_tag.link_address;

	if (sh) {
		shell_print(sh, INFO_FORMAT, FCK_NRF_FW, FCK_NRF_VERSION, hi, lo, s_resetreas, up,
			    map, FCK_NRF_CAPS);
	} else {
		printk(INFO_FORMAT "\n", FCK_NRF_FW, FCK_NRF_VERSION, hi, lo, s_resetreas, up, map,
		       FCK_NRF_CAPS);
	}
}

/* ===== Command lines ============================================================= */

/* `rf`, its sub-command, the arguments of that, and one more word: enough to see that a line
 * has too many. */
#define WORDS_CAP (2 + FCK_ARGS_MAX + 1)

/* Cut `line` into its words, in place, at the spaces. At most `cap` words are taken. */
static int split(char *line, char **words, int cap)
{
	int n = 0;

	while (n < cap) {
		while (*line == ' ') {
			line++;
		}
		if (*line == '\0') {
			break;
		}
		words[n++] = line;
		while (*line != '\0' && *line != ' ') {
			line++;
		}
		if (*line != '\0') {
			*line++ = '\0';
		}
	}
	return n;
}

/* The #DONE verb of a line whose first word is no command: that word, as one short token. */
static void word_verb(const char *word, char *verb, size_t size)
{
	size_t i = 0;

	for (; word[i] != '\0' && i < size - 1; i++) {
		verb[i] = (isalnum((unsigned char)word[i]) || word[i] == '_') ? word[i] : '_';
	}
	verb[i] = '\0';
}

/* One command line: `first` when the shell already took the first word (NULL otherwise), and
 * the rest of the line as it was typed. Whatever the line holds, it ends with one #DONE; only a
 * line of nothing but spaces is answered with nothing, like an empty one. */
static int line_do(const struct shell *sh, const char *first, char *rest)
{
	char *words[WORDS_CAP];
	char verb[16]; /* 15 characters, what the host's line reader takes for a verb */
	int n = 0;

	if (first != NULL) {
		words[n++] = (char *)first;
	}
	n += split(rest, &words[n], WORDS_CAP - n);
	if (n == 0) {
		return 0;
	}
	if (!strcmp(words[0], "info")) {
		if (n - 1 > FCK_ARGS_MAX || fck_help_asked(n - 1, &words[1])) {
			shell_print(sh, "usage: info");
			return fck_done(sh, "info", RC_USAGE);
		}
		info_line(sh);
		return fck_done(sh, "info", RC_OK);
	}
	if (!strcmp(words[0], "rf")) {
		return rf_command(sh, n - 1, &words[1]);
	}
	word_verb(words[0], verb, sizeof(verb));
	shell_print(sh, "usage: info | rf <sub-command> ...");
	return fck_done(sh, verb, RC_USAGE);
}

/* From here to its return the shell's thread is inside a command, and the only proof that the
 * firmware is alive is the command's own fck_alive() (see "Liveness" below). */
static int line_run(const struct shell *sh, const char *first, char *rest)
{
	int rc;

	atomic_set(&s_in_command, 1);
	fck_alive();
	rc = line_do(sh, first, rest);
	fck_alive();
	atomic_set(&s_in_command, 0);
	return rc;
}

/* The three commands Zephyr's shell knows. All take the rest of the line raw, in argv[1] when
 * there is any (SHELL_OPT_ARG_RAW): the shell splits nothing and refuses nothing. `fck` is the
 * root command, so it is given every line. `info` and `rf` are the same two commands for a shell
 * that has no root command set, and are not reached while it has one. */
static char s_no_rest[1];

static int cmd_line(const struct shell *sh, size_t argc, char **argv)
{
	return line_run(sh, NULL, argc > 1 ? argv[1] : s_no_rest);
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	return line_run(sh, "info", argc > 1 ? argv[1] : s_no_rest);
}

static int cmd_rf(const struct shell *sh, size_t argc, char **argv)
{
	return line_run(sh, "rf", argc > 1 ? argv[1] : s_no_rest);
}

SHELL_CMD_ARG_REGISTER(fck, NULL, "info | rf <sub-command> ...", cmd_line, 1, SHELL_OPT_ARG_RAW);
SHELL_CMD_ARG_REGISTER(info, NULL, "#NRFINFO", cmd_info, 1, SHELL_OPT_ARG_RAW);
SHELL_CMD_ARG_REGISTER(rf, NULL, "2.4 GHz radio (nRF54L15 RADIO)", cmd_rf, 1, SHELL_OPT_ARG_RAW);

/* ===== Liveness: watchdog and heartbeat ==========================================
 *
 * A hardware watchdog, WDT31, resets a firmware that stops serving commands. Only main() feeds
 * it, twice a second, and only on proof that commands are still served:
 *
 *  - While a command runs, the proof is the command's own: fck_alive(), called from the shell's
 *    thread at the start and the end of every command and inside every loop that takes long. A
 *    command that hangs stops calling it.
 *  - While none runs, the proof is that the shell is parked waiting for a line: its lock, which
 *    it holds while it handles input, is free. The lock can be held for long with nothing wrong:
 *    the shell's thread has a low priority, and during an image upload mcumgr's thread, which
 *    writes the flash, runs ahead of it while it has the lock in hand. So a lock that stays
 *    taken counts as a hang only after SHELL_BUSY_GRACE_MS.
 *
 * Without proof main() stops feeding, and WATCHDOG_MS later the chip resets with the watchdog's
 * bit in RESETREAS (reset= in #NRFINFO). A main() that itself stops (a fatal error halts the
 * kernel; a thread above it spins) ends the same way.
 *
 * The heartbeat pin toggles on the same proof and at the same moment, so it stops at once when
 * the commands hang, seconds before the reset: it is no longer a sign that only main() runs.
 *
 * The watchdog must not be running when the firmware resets itself, as mcumgr does after an
 * upload. On the nRF54L15 a soft reset does not reset the WDT (datasheet, "Reset behavior":
 * only pin reset, the watchdog's own reset, power-on, brownout and the like do), and MCUboot as
 * NCS builds it for this SoC does not feed it (CONFIG_BOOT_WATCHDOG_FEED=n in its
 * socs/nrf54l15_cpuapp.conf): it would bite inside MCUboot's 3 s wait for serial recovery, or in
 * the middle of an image swap. So sys_arch_reboot() is wrapped (CMakeLists.txt) and stops it
 * first. A CPU lockup is the one reset left that keeps it running: MCUboot may then be reset by
 * it once, and after a watchdog reset it starts with the watchdog stopped.
 */
#define WDT_NODE            DT_NODELABEL(wdt31)
#define WATCHDOG_MS         4000
#define KEEPER_TICK_MS      500   /* one feed and one heartbeat edge: a 1 Hz square wave */
#define SHELL_BUSY_GRACE_MS 60000
#define WDT_STOP_SPINS      200000U /* reads of RUNSTATUS; the stop takes a few 32 kHz ticks */

static const struct device *const s_wdt = DEVICE_DT_GET(WDT_NODE);
static int s_wdt_channel = -1;

static int watchdog_start(void)
{
	const struct wdt_timeout_cfg cfg = {
		.window = {.min = 0, .max = WATCHDOG_MS},
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};
	int rc;

	if (!device_is_ready(s_wdt)) {
		return -ENODEV;
	}
	rc = wdt_install_timeout(s_wdt, &cfg);
	if (rc < 0) {
		return rc;
	}
	/* It runs while the CPU sleeps (an idle hang is a sleeping CPU) and stops only while a
	 * debugger holds the CPU. */
	if (wdt_setup(s_wdt, WDT_OPT_PAUSE_HALTED_BY_DBG) < 0) {
		return -EIO;
	}
	s_wdt_channel = rc;
	return 0;
}

/* Every reboot the firmware asks for comes through here (-Wl,--wrap=sys_arch_reboot), with
 * interrupts already locked by sys_reboot(): no kernel call, only the WDT's registers. If the
 * watchdog does not stop, the reset goes ahead all the same, with a full period ahead of it. */
void __real_sys_arch_reboot(int type);

void __wrap_sys_arch_reboot(int type)
{
	NRF_WDT_Type *wdt = (NRF_WDT_Type *)DT_REG_ADDR(WDT_NODE);

	if (nrf_wdt_started_check(wdt)) {
		for (int i = 0; i < NRF_WDT_CHANNEL_NUMBER; i++) {
			nrf_wdt_reload_request_set(wdt, (nrf_wdt_rr_register_t)(NRF_WDT_RR0 + i));
		}
		nrf_wdt_task_stop_enable_set(wdt, true);
		nrf_wdt_task_trigger(wdt, NRF_WDT_TASK_STOP);
		for (uint32_t i = 0; i < WDT_STOP_SPINS && nrf_wdt_started_check(wdt); i++) {
		}
		nrf_wdt_task_stop_enable_set(wdt, false);
	}
	__real_sys_arch_reboot(type);
}

/* Whether commands are being served, as said above. Called once per tick, by main() only. */
static bool commands_alive(int64_t now)
{
	static int64_t lock_fine_at; /* when the shell's lock was last free, or a command ran */
	const struct shell *sh = shell_backend_uart_get_ptr();
	bool marked = atomic_clear(&fck_alive_mark) != 0;

	if (atomic_get(&s_in_command)) {
		/* The shell gives its lock back for as long as a command runs: the grace below
		 * starts when the command ends, however long it took. */
		lock_fine_at = now;
		return marked;
	}
	if (k_sem_take(&sh->ctx->lock_sem, K_NO_WAIT) == 0) {
		k_sem_give(&sh->ctx->lock_sem);
		lock_fine_at = now;
		return true;
	}
	return marked || (now - lock_fine_at) < SHELL_BUSY_GRACE_MS;
}

int main(void)
{
	/* Latch the reset cause first, then clear it so the next boot reports only its own. */
	s_resetreas = nrfx_reset_reason_get();
	nrfx_reset_reason_clear(s_resetreas);

	int wdt_rc = watchdog_start();
	int rc = rf_init();

	/* One machine line at boot (after MCUboot's ~3 s serial-recovery window) so the host sees
	 * the firmware come up without asking. printk goes to the same uart00 (zephyr,console). */
	info_line(NULL);
	if (wdt_rc) {
		printk("#ERR what=watchdog rc=%d\n", wdt_rc);
	}
	if (rc) {
		printk("#ERR what=rf_init rc=%d\n", rc);
	}

	/* 1 Hz heartbeat on P0.00 (pad C1 -> NRF_WP_HOST -> TCA6408A P5 input):
	 * each edge makes the expander assert ~INT, so the host has a liveness check
	 * that does not depend on the UART. */
	const struct device *p0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	bool p0_ok = device_is_ready(p0);

	if (p0_ok) {
		gpio_pin_configure(p0, 0, GPIO_OUTPUT_INACTIVE);
	}

	int v = 0;

	while (1) {
		if (commands_alive(k_uptime_get())) {
			if (s_wdt_channel >= 0) {
				(void)wdt_feed(s_wdt, s_wdt_channel);
			}
			if (p0_ok) {
				gpio_pin_set(p0, 0, v);
				v = !v;
			}
		}
		k_sleep(K_MSEC(KEEPER_TICK_MS));
	}
	return 0;
}
