# c5_firmware

**A firmware of your own for the F\*CK's ESP32-C5, to start from.**

Every F\*CK leaves the factory with a firmware on its C5 that the SDK talks to. The module is open: an app can write any firmware into the C5 from the Kode with `fck_flash()`, and put the official one back the same way. This project is where such a firmware starts. It already has:

| What | Where |
|---|---|
| The C5's pins: the link to the Kode, the GNSS receiver, its 1PPS, the wake lines | `main/fck_c5_board.h` |
| The factory's flash map, with the app taking the whole 4 MB | `partitions.csv` |
| The line protocol the SDK speaks, and a greeting it reads | `main/main.c`, `main/fck_c5_hello.h` |
| The radio inside the module's limits, always | `main/fck_c5_limits.c` |
| One command to copy: `scan` | `main/main.c` |

The C5 is an ESP32-C5-MINI-1U-N4T2: 4 MB of flash, no PSRAM.

## Build

```
. <path-to-esp-idf>/export.sh
idf.py set-target esp32c5
idf.py build
```

Three images come out, and each goes where the firmware's go:

| Image | Address |
|---|---|
| `build/bootloader/bootloader.bin` | 0x2000 |
| `build/partition_table/partition-table.bin` | 0x8000 |
| `build/c5_firmware.bin` | 0x10000 |

Before you give it to anyone, name it in `idf.py menuconfig`, under *F\*CK C5 firmware*: 1 to 23 letters, digits, `_`, `.` or `-`, and neither `fck-c5` nor `c5_fck_agent`, the official firmware's names. The build checks both. Its version is `CONFIG_APP_PROJECT_VER` in `sdkconfig.defaults`, digits and dots.

## Write it into the module

From an app on the Kode, with this SDK's `fck_flash()`. The C5 is written through its ROM loader, which no firmware can take away, so one that does not boot is never the end of the module: write it again, or restore the firmware. Only eFuses can take the ROM loader away, and they never come back: Secure Boot, flash encryption and the ROM's log off each burn some the first time the firmware starts, so `main.c` stops the build if one of them is turned on. It stops it for the bootloader's anti-rollback too: that option burns nothing by itself in ESP-IDF v6.1 (it writes a secure version into the bootloader's header, read in its source), but it is one step from the eFuses that make the ROM refuse an older bootloader, and a firmware of your own has no use for it.

## Talk to it

UART0 at 115200: the C5's IO11 sends and IO12 receives. One command per line, no prompt and no echo, and every command ends with `#DONE <command> rc=<n>`: 0 done, 1 failed, 2 not understood. Lines that start with `#` carry what a program reads; the rest is for people. A command that fails for a reason a program should know says it in a `#ERR cmd=<command> why=<reason>` line before its `#DONE`, as the nRF firmware does.

At boot and for `info` it greets:

```
#C5INFO fw=c5_firmware ver=0.1.0 chip=esp32c5 rev=1.0 mac=<its MAC> reg=EU lim=ok caps=
```

The SDK reads that line to know what the C5 runs. The `caps=` is what makes a firmware compatible: the SDK's typed calls (`fck_wifi_scan()`, `fck_ble_scan()`...) are sent to any firmware that prints one, each only if its word is listed (`scan`, `chan`, `ble`, `bleadv`, `frames`, `gnss`, `wp`, `gtx`, `health`), and the answers must be the ones `firmware/fck-c5` gives. This one lists none, so it is sent none; add a word when you write its command. Anything else an app sends it, one line at a time, goes through `fck_command()`: its vocabulary is yours.

## The radio limits

They are the same limits the official firmware keeps:

- Wi-Fi as a station only, country ES with 802.11d off, 20 MHz on both bands, no 802.11b.
- 15 dBm on 2.4 GHz and 13 dBm on 5 GHz.
- 5 GHz only on channels 100-140, and only listening. Anything that transmits stays on 2.4 GHz: after `fck_c5_limits_wifi_start()` the driver rests there.
- Bluetooth LE only as an observer that scans passively, and no 802.15.4.

How they are kept:

| When | What |
|---|---|
| The firmware builds | A build that turns on the SoftAP, Bluedroid, a NimBLE role other than the observer, a Bluetooth controller without a host, or 802.15.4 stops with an error. `sdkconfig.defaults` leaves the SoftAP and 802.15.4 off. |
| It runs | If the Wi-Fi driver refuses one of them, the radio stays off until the C5 restarts. The greeting names the limit (`lim=no11b`, say), `fck_c5_limits_wifi_start()` and `fck_c5_limits_scan_pass()` return `ESP_ERR_NOT_ALLOWED`, and `scan` answers `#ERR cmd=scan why=lim_no11b` with `rc=1`. |
| Your code | Scan Bluetooth LE passively: in NimBLE, `passive = 1` in the `ble_gap_disc_params` you pass to `ble_gap_disc()`. Use neither ESP-NOW nor raw 802.11 frames (`esp_wifi_80211_tx()`): they pick their own rate, 1 Mbps 802.11b by default for ESP-NOW, and these limits do not set it. |

They are always on: no option turns them off. The greeting says `reg=EU lim=ok`, or names the limit the driver refused.

## Power

`scan` starts the Wi-Fi driver, scans and stops it, so the radio is off between commands. The C5 itself stays awake between commands; sleeping between them is the next thing to add if yours waits for long.

## Licence

As the rest of this repository: GPL-3.0-or-later, see `NOTICE.md`.
