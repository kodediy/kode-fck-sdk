# firmware

The F\*CK module's own firmware, which this SDK talks to: `fck-c5` for the ESP32-C5 (U501) and `fck-nrf` for the nRF54L15 (U701), with the nRF's bootloader. An app on the Kode writes them into the module with `fck_flash()`.

`fck-c5` is at **0.2.0** and `fck-nrf` at **0.1.0**. Everything public starts at 0.x and becomes 1.0.0 when production is frozen.

Known gaps:

- **A module whose GNSS receiver already holds the I2C bus may not be found.** An app that starts within a second of the Kode's reset finds it; one started later can say "something holds the I2C pair and nothing answers at 0x21". Plugging the module in with the app running finds it.
- The log of an app may say `i2c.master I2C hardware timeout detected` and then "the F\*CK lost power for a moment: it is set up again" when it starts. The module is found all the same.
- A Bluetooth scan that is cut short hands what it heard through `fck.h`, not through `fck_sys.h`: the extended layer sends no entries when the call fails.
- **`fck-nrf` answers the nRF54L15's anomaly 20 ("RADIO payload is not transmitted") the old way.** It starts the clock's PLL when it takes the crystal (`TASKS_PLLSTART`), as Nordic's `radio_test` sample did; Nordic's samples have since changed to constant latency for that anomaly, and this firmware has not. If the anomaly showed on a module, `rf ping` and `rf echo` would send a packet's preamble and not its payload, and every ping would end `ok=0`.

## What both keep

- **The radio limits, in one table per chip**: `fck-c5/main/fck_c5_limits.h` and `fck-nrf/src/fck_nrf_limits.h`. No command, option or build flag lifts them.
- **A version from one place** (`PROJECT_VER` in `fck-c5/CMakeLists.txt`, `fck-nrf/VERSION`), which says which build runs and nothing about what it does.
- **What they do, in `caps=`** of their info line: a word per command or ability. The SDK asks a firmware only for what it lists (`fck_drv_c5_has()`, `fck_drv_nrf_has()`), never by its version. A new ability is a new word; a word never changes its meaning.
- **A watchdog** that restarts a firmware whose command loop hangs, and the cause of the last start in the info line (`rst=`, `reset=`).
- **`#DONE <verb> rc=<n>` at the end of every line they are sent**, whatever it held: 0 done, 1 failed, 2 not understood. An empty line is answered with nothing.

| | `fck-c5` | `fck-nrf` |
|---|---|---|
| Info line | `#C5INFO fw=fck-c5 ver=0.2 chip= rev= mac= reg=EU lim= caps= uptime_ms= rst=` | `#NRFINFO fw=fck-nrf ver=0.1.0 devid= reset= uptime_ms= map= caps=` |
| It keeps the limits | `reg=EU` and `lim=ok`. `lim=` is anything else while Wi-Fi is not up inside them (`wifi-loop`, `wifi-init`, `wifi-mode`, `wifi-start`) or names the limit the driver refused, and `scan` then refuses | the word `eu` in `caps=` |
| `caps=` | `scan,chan,ble,bleadv,frames,gnss,wp,gtx,health` (0.1.0: without `bleadv` and `frames`) | `spec,off,power,echo,ping,sniff,scfg,saddr,eu,done,wdt,echoidle,smp` |
| What transmits | `scan`, 2.4 GHz only, channels 1 to 13 at 15 dBm; 5 GHz is only listened to | `rf ping` and `rf echo on`, 2402 to 2480 MHz at 6 dBm at most; a packet the radio does not end is cut 0.3 ms after it; the responder stops after 60 s with no ping |

Within a start the info line says the same but for `uptime_ms=`, which is how the SDK tells a restart. What moves while the C5 runs is in `health` (`#C5HEALTH uptime_ms= rst= heap= heap_min=`).

The SDK also takes them as `c5_fck_agent` and `nrf_fck_agent`. A firmware of those names that prints no `caps=` is asked by its name and version; one that keeps no radio limits itself is sent nothing that transmits.

## A firmware of your own

The module is open: either chip takes a firmware of yours (`fck_flash()`), and the SDK works with it as far as it speaks the same words.

- **Compatible** is a `caps=` in the info line, whatever the `fw=`: `#C5INFO fw=<its name> caps=<words>` or `#NRFINFO fw=<its name> caps=<words>`, printed at boot and for `info`, every command ended with `#DONE <command> rc=<n>`. The typed calls are sent for the words it lists, and expect the answers of the firmware here. With no `caps=` only `fck_command()` reaches it, the line as written.
- **The nRF has to say a line in the 9 s after it starts**, its `#NRFINFO` for one: `fck_flash()` listens for it to call the write good.
- **The limits are the firmware's.** The SDK reads none of `reg=`, `lim=` or `eu` to decide what it sends: the firmware here keeps its limits itself, and a firmware of yours keeps its own.

## fck-c5

ESP-IDF v6.1.

```
cd firmware/fck-c5
rm -f sdkconfig          # it would override sdkconfig.defaults
idf.py set-target esp32c5
idf.py build
```

| Image | Address |
|---|---|
| `build/bootloader/bootloader.bin` | 0x2000 |
| `build/partition_table/partition-table.bin` | 0x8000 |
| `build/fck-c5.bin` | 0x10000 |

The image declares the 4 MB of flash the module's ESP32-C5-MINI-1U-N4T2 has. The bootloader declares secure version 4, the highest there is, and burns nothing: ESP-IDF v6.1 only writes that number into the bootloader's header (read in its source). No eFuse is burnt by this firmware.

From 0.2.0 (`bleadv` in `caps=`), `ble scan` says what each advertiser sent: every `#BLE` line ends with `evt=` (the kind of its latest report), `first=` and `last=` (ms from the start of the scan) and, when a report carried any, `data=` (its bytes as sent, in hex). They are keys added to the line, so an SDK that does not know them reads the rest as before, and this SDK reads a `#BLE` with or without them (`fck_ble_device_t`). The scan is passive.

From 0.2.0 (`frames` in `caps=`), `frames ch=<n> [ms=250]` listens on one channel in promiscuous mode (receive only) and tells the frames it hears, one `#F <base64>` line each, then a `#FRAMES` summary. A run tells each sender once (and every deauthentication and disassociation) and counts the rest; no payload ever leaves the C5: each line carries the frame control, the channel, the RSSI, the length and the three addresses, and a short head - a beacon's or probe response's 12 fixed bytes and network name, a probe request's network name, a deauthentication's or disassociation's reason. A data frame has no head, nor does a protected management frame (802.11w).

## fck-nrf

nRF Connect SDK v3.4.2, with its Zephyr SDK (1.0.1). From the NCS workspace:

```
west build -p always -b nrf54l15dk/nrf54l15/cpuapp --sysbuild -d build-fck-nrf <sdk>/firmware/fck-nrf
```

| Output | What |
|---|---|
| `build-fck-nrf/fck-nrf/zephyr/zephyr.signed.bin` | The image `fck_flash()` writes, at `FCK_BSP_NRF_SLOT0` |
| `build-fck-nrf/mcuboot/zephyr/zephyr.hex` and `build-fck-nrf/fck-nrf/zephyr/zephyr.signed.hex` | MCUboot and the image, for SWD |

**The bootloader** is MCUboot with one slot and no key (`sysbuild.conf`, `sysbuild/mcuboot.conf`, `pm_static.yml`):

| | |
|---|---|
| MCUboot | 0x0 to 0x7C00, the 31 KB the nRF54L15 can close. It closes its own region before it starts the image |
| The image | header at 0xE000, code at 0xE800, up to 0x165000 |
| How an image is taken | by its SHA-256 alone: "signed" is imgtool's word, and the file carries no signature. Anyone builds one |
| How it is written | through MCUboot's serial recovery, for 3 s after a reset, and by nothing else. MCUboot itself changes over SWD only |
| How the Kode tells it | it answers SMP's echo |

The image carries where it was linked in a tag, `FCKMAP` (twelve bytes: the magic `46 43 4B 4D 41 50 00 01` and the address, 0x0000E800), and prints it as `map=`. `fck_flash()` refuses an image whose tag names another address, since MCUboot checks a hash and not a map, and such an image would start and hang.

An nRF may carry another MCUboot (two slots, or one slot at 0x10000), which takes only an image signed with Nordic's public development key (ED25519 over SHA-512) and so does not take this one. `fck_flash()` asks which MCUboot answers before it uploads a byte, and refuses an image that one would not boot.
