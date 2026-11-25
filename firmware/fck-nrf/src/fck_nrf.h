/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fck-nrf - shared bits between main.c (board, info, command lines, watchdog, heartbeat) and
 * rf.c (2.4 GHz radio).
 */
#ifndef FCK_NRF_H
#define FCK_NRF_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <app_version.h>

#define FCK_NRF_FW "fck-nrf"

/* The version has one source, the VERSION file next to CMakeLists.txt: Zephyr turns it into
 * app_version.h, and into the version imgtool signs the MCUboot image with. `ver=` in #NRFINFO is
 * always major.minor.patch in plain digits, the form the host driver reads. */
#define FCK_NRF_VERSION                                                                            \
	STRINGIFY(APP_VERSION_MAJOR) "." STRINGIFY(APP_VERSION_MINOR) "." STRINGIFY(APP_PATCHLEVEL)

/* `caps=` in #NRFINFO: what this firmware does, for a host to decide by instead of by version.
 *   spec off power echo ping sniff scfg saddr   the `rf` sub-commands it has
 *   eu        it keeps the module's radio limits itself (fck_nrf_limits.h); nothing lifts them
 *   done      every command line ends with #DONE, whatever it held (see fck_done below)
 *   wdt       a watchdog resets a firmware that stops serving commands (reset= then says so)
 *   echoidle  the echo responder stops by itself when no ping reaches it for a while
 *   smp       firmware update by mcumgr SMP on this same UART
 * A new ability gets a new word; a word is never taken to mean something else. */
#define FCK_NRF_CAPS "spec,off,power,echo,ping,sniff,scfg,saddr,eu,done,wdt,echoidle,smp"

/* The link map tag: 12 bytes in .rodata that say where this image was linked to run, so that a
 * tool can tell it from a .signed.bin without running it.
 *   bytes 0..7   the magic: 'F' 'C' 'K' 'M' 'A' 'P' 0x00 0x01   (46 43 4B 4D 41 50 00 01)
 *   bytes 8..11  the address of the image's vector table, its first byte of code: 32 bits,
 *                little endian. 0x0000E800 with the partition map in pm_static.yml.
 * The magic appears once in an image. `map=` in #NRFINFO prints the same field. */
#define FCK_NRF_MAP_MAGIC                                                                          \
	{                                                                                          \
		'F', 'C', 'K', 'M', 'A', 'P', 0x00, 0x01                                           \
	}

struct fck_nrf_map_tag {
	uint8_t magic[8];
	uint32_t link_address;
};

/* Return codes of every command, as in fck-c5: 0 ok, 1 failed, 2 not understood. */
#define RC_OK    0
#define RC_FAIL  1
#define RC_USAGE 2

/* The most arguments a command or sub-command is given; a line with more is not understood. */
#define FCK_ARGS_MAX 8

/* Proof of life from the thread that runs the commands. A command that takes long calls this
 * at least every 250 ms; main.c feeds the watchdog and toggles the heartbeat pin only while it
 * keeps coming (or while no command runs and the shell waits for a line). It is one store: it
 * touches no peripheral and may sit beside a timing loop. */
extern atomic_t fck_alive_mark;

static inline void fck_alive(void)
{
	atomic_set(&fck_alive_mark, 1);
}

/* "#DONE <verb> rc=<n>" - the last line of every command line the firmware is sent: also of one
 * with too many words, with `-h`, or with a command or sub-command it does not have (rc=2).
 * <verb> is one token: `info`, `rf_<sub>`, `rf` when the sub-command is missing or unknown, and
 * for an unknown command its first word. Only an empty line is answered with nothing. */
static inline int fck_done(const struct shell *sh, const char *verb, int rc)
{
	fck_alive();
	shell_print(sh, "#DONE %s rc=%d", verb, rc);
	return 0; /* the shell's own return code stays 0: the protocol result is the rc= above */
}

/* `-h` or `--help` among the arguments: the line asks for the usage and runs nothing. */
static inline bool fck_help_asked(size_t argc, char **argv)
{
	for (size_t i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			return true;
		}
	}
	return false;
}

/* Low byte of FICR DEVICEID: node id carried in the ping/echo packets. */
uint8_t fck_node_id(void);

/* Radio module (rf.c). rf_command() runs `rf <argv[0]> <argv[1]> ...` and prints its #DONE. */
int rf_init(void);
int rf_command(const struct shell *sh, size_t argc, char **argv);

#endif /* FCK_NRF_H */
