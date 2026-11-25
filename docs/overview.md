# Overview of the F\*CK SDK

What the SDK drives, how it works and what it costs an app. The [README](../README.md) has the quick start, and the two headers, [`fck.h`](../components/fck/include/fck.h) and [`fck_sys.h`](../components/fck_sys/include/fck_sys.h), are the contract: this page only gathers what is spread over them.

## The module and its firmware

The F\*CK (KDFCK001) plugs into the expansion header J3 of the Kode Dot. It carries an ESP32-C5 (Wi-Fi and Bluetooth), an nRF54L15 (2.4 GHz), a CC1101 (sub-GHz) and a GNSS receiver. Every F\*CK leaves the factory with firmware that this SDK talks to: a firmware on the C5 and a firmware on the nRF. An app can write its own firmware into either chip with `fck_flash()`. Both chips stay reachable from the Kode - the C5 through its ROM loader, the nRF through MCUboot - so a firmware that does not start is never the end of the module, and `fck_rescue_probe()` says what either chip carries: the boot code `fck_flash()` writes through (the C5's ROM loader; the nRF's MCUboot and the image in its slot 0), and whether the image starts. Only fuses close a chip for good - on the C5, Secure Boot, flash encryption or the ROM loader off, which [`c5/`](../c5/)'s template refuses to build; on the nRF, its UICR - and nothing this SDK sends to a firmware burns them.

## The two layers

Two layers reach the same module, as kode-sdk's `kode.h` and `kode_sys.h` do. A **task** (`app_main()` or one of the app's own) uses `fck.h`: a call waits and answers `true` or `false`, and `fck_error()` says why.

```c
#include "kode.h"
#include "fck.h"

void app_main(void)
{
    kode_init();
    fck_begin();                                  // J3, its controllers, its supply
    while (!fck_wait_present(5000)) {
        KLOG_W("app", "no F*CK on J3 yet: %s", fck_error());   // J3's supply may take a while
    }
    fck_wifi_network_t networks[16];
    size_t             n = 0;
    if (!fck_wifi_scan(FCK_BAND_ALL, networks, 16, &n)) {    // TRANSMITS: the scan sends probe requests
        KLOG_W("app", "scan: %s", fck_error());
    }
}
```

An **Active Object** uses `fck_sys.h`: nothing waits. The module's coming is news, a call is a request made for the Active Object, and its answer is an event of that Active Object's own.

```c
#include "kode_sys.h"
#include "fck_sys.h"

// in the Active Object's initial transition
fck_sys_subscribe(&me->super);                    // FCK_MODULE_FOUND_SIG ... FCK_SEND_CANCELLED_SIG
fck_sys_begin();                                  // J3, its controllers, its supply

// when FCK_MODULE_FOUND_SIG arrives
fck_sys_wifi_scan(&me->super, FCK_BAND_ALL);      // FCK_WIFI_SCAN_ENTRY_SIG per network, then
                                                  // FCK_WIFI_SCAN_RSP_SIG (fck_sys_answer_t: rc, why, count)
```

## What it drives

| Part | Calls | Notes |
|---|---|---|
| The module | `fck_begin()`, `fck_end()`, `fck_is_present()`, `fck_wait_present()`, `fck_status()`, `fck_header_position()`, `fck_error()`, `fck_cancel()` | Found by its expander and the power-loss mark, set up, watched once a second. `fck_header_position()` says where on J3 one of the module's lines is, as kode-sdk's `kode_sys_exp_*()` takes it; 0 with no module. `fck_cancel()` ends what the app waits for, in every task. |
| Events | `fck_event_add_listener()`, `fck_event_remove_listener()`, `fck_event_wait()`; in an Active Object `fck_sys_subscribe()` | Seven named events: `module_found`, `module_lost`, `wrong_module` (not an F*CK on J3, ruled out after two looks and told once per module; the value says whether it holds the I2C pair with nothing at the expander's address, or a chip answers there that is not the expander), `setting_lost` and `setting_restored` (what the app had set that the module forgot on leaving, and what the SDK set again: only what cannot emit - the GNSS receiver and the sub-GHz listening), `packet_received` (the listening heard a remote) and `send_cancelled`. A listener runs on a task of the SDK's own, not the owner's: it may log and call `fck.h`, but not `fck_event_wait()` nor `fck_end()`. An Active Object gets the same as `FCK_*_SIG`. |
| The chips | `fck_command()`, `fck_firmware()`, `fck_reset()`, `fck_power()`, `fck_flash()`, `fck_rescue_probe()` | A line to either firmware and its answer; what a chip runs; a chip held in reset or let run; a firmware written into it; what a chip carries, with nothing written. |
| Wi-Fi, BLE | `fck_wifi_scan()`, `fck_wifi_channels()`, `fck_wifi_frames()`, `fck_ble_scan()`, `fck_ble_manufacturer_data()`, `fck_ble_service_data()`, `fck_ble_services()`, `fck_ble_appearance()` | On the C5's firmware, within the limits it reports. A BLE scan only listens (passive: it never sends a scan request), so a device that names itself only in its scan response comes with no name: the Kode's own Bluetooth asks for it (kode-sdk's `kode_ble_scan()`), and an app matches the two lists by address. With a `fck-c5` that says `bleadv` in its `caps=` (0.2.0) each device comes with what it advertised - its bytes as sent, how and when - which the four `fck_ble_*` helpers read without the module. What a device is, is the app's to tell. With one that says `frames` (0.2.0 too), `fck_wifi_frames()` listens on one channel and hands over a digest of the frames it hears - one line per sender, the rest counted - with no payload and no name past a network's. It never transmits, and has no `fck_sys_` twin yet (an Active Object cannot call it). |
| GNSS | `fck_gnss_begin()`, `fck_gnss_read()`, `fck_gnss_end()` | The receiver's sentences, relayed by the C5, read into a fix: the position, its UTC date and time, and how long ago the Kode read it (`age_ms`). The C5's firmware goes on relaying while it scans Wi-Fi or Bluetooth, so a scan's results are paired with the fix read as the scan answers (not yet run on a module). A position is never logged. |
| 2.4 GHz | `fck_radio_spectrum()`, `fck_radio_sniff()`, `fck_radio_sniff_tune()`, `fck_radio_ping()`, `fck_radio_echo_start()`, `fck_radio_echo_stop()` | On the nRF's firmware. |
| Sub-GHz | `fck_subghz_level()`, `fck_subghz_spectrum()`, `fck_subghz_find()`, `fck_subghz_capture()`, `fck_subghz_decode()`, `fck_subghz_decode_rolling()`, `fck_subghz_listen_start()`, `fck_subghz_listen_read()`, `fck_subghz_listen_stop()`, `fck_subghz_frequencies()`, `fck_subghz_encode()`, `fck_subghz_protocol_info()`, `fck_subghz_send_raw()`, `fck_subghz_send_code()`, `fck_subghz_packet_receive()`, `fck_subghz_packet_send()`, `fck_subghz_may_transmit()`, `fck_subghz_air_time_left()`, `fck_subghz_to_file()`, `fck_subghz_code_to_file()`, `fck_subghz_from_file()` | The CC1101: listens on 300-348, 387-464 and 779-928 MHz, sends only on 433.05-434.79 and 868.0-868.6 MHz, OOK or 2-FSK/GFSK packets. Each band goes through its own antenna leg (`fck_bsp.h`): 315 MHz the top one, 433 the middle one and 868/915 the bottom one. While the GNSS receiver holds the module's I2C bus the expander is out of reach, and with it 315's leg: 433 and 868/915 still work. Sends are limited to an air time in each band: 10 % of any hour at 433 (360 s) and 1 % at 868 (36 s); `fck_subghz_air_time_left()` says what is left, and a send over budget is refused. The hour's account is kept in RAM: it starts empty each time the app starts, and survives `fck_end()` within one run. Fixed codes (EV1527, PT2262, HT12E) are read, written, sent and kept as `.ksub` files; KeeLoq and Somfy RTS are only read (`fck_subghz_decode_rolling()`). `fck_subghz_send_raw()` refuses, with nothing sent, any capture shaped like a rolling code, decoded or not (`fck_drv_ook_rolling()`): a KeeLoq-like preamble, a Somfy-like sync, or more than 60 runs in a row that are nearly all a short pulse or a long one (1.55 to 3.75 times the short). The cost: fixed codes of more than 30 bits cannot be cloned until they have a decoder. |

`fck.h` is the contract: every call's comment opens with `Blocks.` or `Returns at once.`, then says what it does, what it refuses and when it answers `false`; the table of the causes of a `false` is at the top of the header, and `fck_error()` gives the exact reason of the last failure. A refusal is `false` in `fck.h`; in `fck_sys.h` it is the answer's `rc` (`KODE_DENIED` and the other codes of kode-sdk) with its class in `why`.

## How it works

`fck_begin()` (or `fck_sys_begin()`) lends J3's 14 positions under `"fck"`, takes from kode-sdk's books the controllers the module's drivers run on (`kode_sys_exp_ctrl()`: I2C1, SPI3 and the two lowest free UARTs), asks J3's supply on and starts one Active Object, the **owner** (`AO_Fck`, of kode-sdk's own kind), and its worker.

The owner's step is short. Every 10 ms it reads what the two UARTs hold and hands each line to whoever waits for it; a request is a line written to a firmware, or an expander pin. What waits - finding the module, getting the I2C bus back from the GNSS receiver, writing a chip's flash, a sub-GHz capture - runs on the worker, a task of the owner's pinned to core 1 because the module's I2C bus has to be created there (`fck_drv_tca6408a.h`). While a job runs the owner leaves the module alone, and the requests that would touch it wait for the job's end. A sub-GHz job touches neither chip, so the owner goes on reading their UARTs through it: the GNSS fix keeps coming, and what each chip said it runs stands; after a write or a rescue it is all read again.

The code is in three layers over the owner. `fck_core.h` (`components/fck/core`, `kode_status_t`) is the module's single entry; nothing outside the SDK includes it. `fck.h` is the **standard layer**: each call asks the owner and waits for the answer on the caller's task, so it **blocks** - call it from a task of the app's own. Called from an Active Object it refuses at once (`false`, and `fck_error()` says to call `fck_sys_*`). `fck_sys.h` is the **extended layer**, in the component `fck_sys`: it never waits. A request takes the asking Active Object, goes to two executor tasks of the SDK's that make the core's blocking call, and the answer is posted back as an `fck_sys_answer_t` (`rc` the code the call would have returned, `why` the class of the failure, `count` what it wrote into a buffer lent). `fck_is_present()`, `fck_status()` and `fck_header_position()` (and their `fck_sys_` twins) return at once in both.

Each asking Active Object has one **door** per kind of request (COMMAND, FLASH, SCAN, GNSS, RADIO, LISTEN, SEND); a request holds its door until its answer, and a second of the same kind before then is refused at once. The queue of the asking Active Object has to hold what can wait in it: one slot per door open, the entries of a scan (posted with a margin), the news (a margin of 2) and its own events; `fck_sys.h` has the arithmetic, and says 16 slots is enough for most and 32 for one that holds all seven doors open. An answer that finds the queue full is retried for about a second, then logged, counted in `fck_status().answers_dropped` and dropped - never an assert.

## Things that surprise

- **J3's supply is not promised until `fck_begin()`.** kode-sdk does not promise +3.3V_EXT's state when an app starts. `fck_begin()` asks it on over the link to the Dot's C5 (kode-sdk's `EXP_POWER_SIG`, so it is asynchronous: it does not fail the start, and `fck_status().j3_supply` says what came of it), which a Wi-Fi scan of the Dot's own can hold for up to some 40 s: `fck_wait_present()` waits for that too.
- **A module that was powered before the app looked may not be found.** The GNSS receiver's standard firmware takes the module's I2C bus about 420 ms after it powers up, and the owner drives nothing on a module it has not identified: it says `wrong_module` ("something holds the I2C pair and nothing answers at 0x21") and asks again every 3 s. Plugging the module with the app running finds it ([`firmware/README.md`](../firmware/README.md), "Known gaps").
- **`CONFIG_KODEOS_RADIO=y`.** The supply's switch is a pin of the Dot's C5. An image without the radio (`CONFIG_KODEOS_RADIO=n`) keeps the C5 in reset and J3 always powered: the module is found as soon as it is plugged, `fck_status().j3_supply` says `FCK_J3_ALWAYS`, and `fck_end()` cannot switch it off.
- **Going home parks the module, and sleep puts it down.** Before the P4 restarts on purpose - every way home in kode-sdk, `HOME_SIG` from the bottom button - the owner puts the GNSS receiver in reset and the CC1101 to sleep, resets the nRF and holds the C5 in reset. With `CONFIG_KODE_LOWPOWER` it does the same when the board is going to rest (kode-sdk's `LOWPOWER_*` signals; a send in flight is answered as cancelled), and looks for the module again when the board is awake, giving back only what listens. A panic, a watchdog or a brownout cannot: the next app that opens the module finds it as it was, and sets it up again.
- **`fck_end()` leaves on purpose, with the same cut as going home.** It sets the abort flag first (a sub-GHz send on the air stops at once, the chip in IDLE), puts the module down as for a way home, asks J3's supply off, gives the 14 positions and the controllers back and leaves the owner idle until `fck_begin()` starts it again; every call but `fck_begin()` answers `false` meanwhile. Never from a listener or an Active Object: `fck_sys_end()` is the Active Object's, and returns at once (the end is `FCK_MODULE_LOST_SIG`). `fck_sys_begin()` is refused while that end is under way.
- **One module's SDK per app.** `fck_begin()` holds all 14 positions for as long as the app runs, module or not, and its Active Object sits at `KODE_PRIO_USER_HIGH`, where another module's SDK puts its own.
- **The nRF is written one way, and asked which MCUboot it carries first.** The module's MCUboot has one slot and no key: it takes an image by its SHA-256, through its serial recovery and nothing else, so `fck_flash()` takes one image at `FCK_BSP_NRF_SLOT0`. It answers SMP's echo, and `fck_flash()` asks before it uploads a byte: an image the MCUboot that answered would not boot is refused with nothing written, since with one slot there is no old image to go back to. An image that carries an `FCKMAP` tag for another address than 0x0000E800 is refused too, and one with no tag that never names that address is written with a warning in the note. After the write the image must say something within 9 s, or the call fails at `FCK_FLASH_BOOT`.
- **The first modules carry another MCUboot** (TEMPORARY in the code, until the production modules ship): two slots, or one slot at 0x10000. They answer no echo and take only an image signed with Nordic's public development key, so the firmware 0.1.0 is refused there; `fck_rescue_probe()` says which the nRF carries (`one_slot`). Written into the one at 0x10000, an image built for `FCK_BSP_NRF_SLOT0` hangs, and `fck_rescue_probe()` then says `FCK_RESCUE_QUIET`.
- **Nothing transmits or records by itself.** No call emits unless the app asks it to; the module coming back or the board waking restores only what listens (the GNSS receiver, the sub-GHz listening). The radio limits (the bands, the hour's air time) and the console's `fck_arm` stay as they were. `fck_command()` is a GATEWAY: it hands a line to the chip's firmware as written. With the module's own firmware the SDK lets through only that firmware's own commands, inside its limits; `fck.h` says what it does with another firmware.
- **ESP-IDF's log says `gpio: conflict found for GPIO[n]` while the C5 is reset, written or probed.** `gpio_config()` claims a pin the first time it makes it an output, and warns on every later call for that pin. The owner drives the C5's EN and BOOT lines low and lets them go again and again (`fck_flash()`, `fck_rescue_probe()`, a reset), so the warning repeats for those two pins. The lines do what they should: it is a warning about the claim, not about the pin.
- **Bytes are lost while flash is written**, unless the app sets `CONFIG_UART_ISR_IN_IRAM=y`, as `tests/target` does: kode-sdk's board settings leave the UART interrupt out of IRAM. It costs about 8 KB of internal RAM.

## Examples

One ESP-IDF project per part, each an app for kodeOS. Three are tasks (`fck.h`) and four are Active Objects (`fck_sys.h`); the Active Objects have no worker and no task of the app's own: `<name>_main.c` starts the example's own Active Object, and `<name>_ao.c` is the example. Two transmit. `subghz` does only when asked from the console (`fck_sg_send`), after `fck_arm 1` (60 s; `fck_sg_stop`, the module leaving or `fck_arm 0` take it back). `wardriving` does once its top button is pressed and the receiver has a position: its 2.4 GHz scan sends probe requests, inside the radio limits, lap after lap until the bottom button is pressed. No example records or emits without a keypress or a typed row.

| Example | Layer | What it shows |
|---|---|---|
| [`examples/module`](../examples/module) | task | The module found on J3, what the owner knows of it and what each chip runs (`fck_status()`, `fck_firmware()`), noticed with listeners when it leaves. Run it first |
| [`examples/wifi`](../examples/wifi) | Active Object | The C5 listening: how busy each 2.4 GHz channel is (`fck_sys_wifi_channels()`), and the BLE devices around, counted passively (`fck_sys_ble_scan()`), with how many carried a manufacturer's data (`fck_ble_manufacturer_data()`); the two requests share a door, so the second is asked from the first one's answer |
| [`examples/radio`](../examples/radio) | Active Object | The nRF listening: the band's level MHz by MHz (`fck_sys_radio_spectrum()`), then the frames heard where it was busiest (`fck_sys_radio_sniff()`), with the buffers lent to the requests |
| [`examples/subghz`](../examples/subghz) | task | The CC1101: the level at 433.92 MHz, the band, a remote found, captured and decoded (`fck_subghz_level()`, `_spectrum()`, `_find()`, `_capture()`, `_decode()`); from the console, a listen hopping or on one band (`fck_subghz_listen_*()`, reading only what is newer than the last), packets received and sent with fixed GFSK settings (`fck_subghz_packet_receive()`, `_packet_send()`), and the remote kept sent back, cloned or emulated (`fck_subghz_send_raw()`, `_send_code()`), which the console refuses until `fck_arm 1` has been given (it lasts 60 s; `fck_sg_stop` cuts it and what is on the air, as does the module leaving) |
| [`examples/gnss`](../examples/gnss) | Active Object | The receiver started (`fck_sys_gnss_begin()`), a fix read every second and its quality logged (`fck_sys_gnss_read()`); the position itself is never logged |
| [`examples/wardriving`](../examples/wardriving) | Task | Wi-Fi, Bluetooth and the GNSS receiver at once: every network and device heard, with the position it was heard at, written to the microSD card as a WiGLE CSV file (`WigleWifi-1.6`); the log says how long each scan of a lap took. **Transmits** once the top button is pressed: the 2.4 GHz scan sends probe requests, inside the radio limits |
| [`examples/console_actions`](../examples/console_actions) | Active Object | The module on kodeOS's console: six functions and six variables, each row making its request and answering the console with what the SDK said |

Run every one with the module on J3 and the Dot on a USB-C cable. Their `sdkconfig.defaults` set `CONFIG_KODEOS_RADIO=y` with WiFi off and `CONFIG_UART_ISR_IN_IRAM=y`. Copy those lines: an app's own default leaves the radio out.

## Options

| Kconfig | Default | What it does |
|---|---|---|
| `CONFIG_FCK_AO_PRIORITY` | 0 (`KODE_PRIO_USER_HIGH`) | The QP/C priority of `AO_Fck`. Two Active Objects cannot share a level, and the owner refuses to start at one that is taken: an app whose own AO needs the high level moves this one. |
| `CONFIG_KODEOS_RADIO` | `y` in the examples | J3's supply is switched by a pin of the Dot's C5, which the SDK asks through kode-sdk's radio; `fck_status().j3_supply` says if the link to the C5 failed. With `n` the SDK builds and works too: J3 is always powered (`FCK_J3_ALWAYS`). |
| `CONFIG_FCK_SIG_OFFSET` | 1300 | Where the signals of `fck_sys.h` start, above `MAX_SIG`. Mesh takes 1000, Robotics 1100, Camera 1200; this SDK takes 1300 to 1389 for the app and keeps 1390 to 1399 for its owner. |
| `CONFIG_UART_ISR_IN_IRAM` | `y` recommended | The firmwares' bytes survive a flash write. The build warns without it. |

## The console

[`examples/console_actions`](../examples/console_actions) puts the module on kodeOS's console as rows a person or kode-cli can type: `function name="fck_firmware" arg=c5`, `variable name="fck_present" watch=500`. As in kode-sdk, the rows are the app's: one that wants them copies the example's tables, or the rows it needs. A function's `run` may not block, and no call of `fck_sys.h` does: the row makes the request for the example's Active Object and answers the console with what the SDK said (`>ok` is "taken"; a bad argument, a door busy or no module is the refusal itself), and the outcome goes to the log from the Active Object when the answer arrives. `fck_arm` stays the key to emit from the console.

| Functions | Variables |
|---|---|
| `fck_firmware`, `fck_power`, `fck_reset`, `fck_gnss`, `fck_ble_scan`, `fck_rescue` | `fck_present`, `fck_generation`, `fck_c5`, `fck_nrf`, `fck_cc1101`, `fck_gnss_state` |

## What it costs an app

| Resource | Taken |
|---|---|
| J3 | All 14 positions, lent once under `"fck"`. The console's `exp`, typed before `fck_begin()`, takes J3 by hand, and `fck_begin()` answers `false`. |
| Controllers | I2C1, SPI3 (open only while the CC1101 is) and two UARTs, from kode-sdk's books under the same tag. |
| The owner | `AO_Fck`: an 8 KB stack in internal RAM, a queue of 16 events, one pool event per request. |
| Its worker | `fck_work`, 6 KB of internal RAM, `KODE_PRIO_USER_LOW`, core 1. |
| The standard layer | A task for the listeners (`fck_events`) the first time one is added, and nothing else. |
| The extended layer | On the first request: two executor tasks (8 KB each, from the heap) and a queue of 8 requests; a task of 4 KB for `fck_sys_end()` while it runs. At most 8 askers and 4 subscribers. |
| A restart hook | One of ESP-IDF's five. |
| Interrupts | One on GPIO: the CC1101's GDO0, only while a capture or a listen stamps its edges, through ESP-IDF's GPIO ISR service, which the SDK installs if nobody has. The UART, I2C, SPI and RMT drivers' own, on core 1. |

`tests/target/ram_budget.txt` holds what an app that uses the module starts with in internal RAM, and `tools/check_ram.sh` keeps it there.

## Layout

The tree is kode-sdk's: one component per layer, each part in a folder of its own with its header in `include/` and its source in `src/`.

| Path | What it holds |
|---|---|
| `components/fck` | The standard layer (`kode` in kode-sdk): `include/fck.h`, what a task calls, and `src/`: `fck_standard.c` (the layer: tasks only, `bool` and `fck_error()`) and `fck_events.c` (listeners and `fck_event_wait()`) over the core, `fck.c`, `fck_flash.c`, `fck_rescue.c`, `fck_wifi.c`, `fck_gnss.c`, `fck_radio.c`, `fck_subghz.c`. The core's header, `core/include/fck_core.h` (`kode_status_t`), is for `fck` and `fck_sys` only. |
| `components/fck_sys` | The extended layer (`kode_sys` in kode-sdk): `include/fck_sys.h`, what an Active Object calls, and `src/fck_sys.c`: the requests, the doors, the executors, the news. The only place besides the owner that includes `kode_sys.h`. |
| `components/fck_aos` | The owner (`kaos` in kode-sdk): what the facade includes in `core/include`, and in `fck/src` the one Active Object - `AO_Fck` and its worker (`fck_ao.c`) - what its signals do (`fck_aos.c`), the seam between them (`fck_ao.h`) and the owner's rules in plain C (`fck_plan.c`: which sightings rule a chip out, when the module is gone, which fix is a verdict). |
| `components/fck_drv` | The drivers (`kdrv` in kode-sdk), a folder each: `link` (the firmwares' line protocol), `c5`, `c5_rom`, `nrf`, `nrf_dfu`, `smp`, `cc1101`, `ook`, `subghz_scan`, `subghz_file`, `subghz_modem`, `subghz_duty`, `nmea`, `tca6408a`, `uart`, `pins`, `system`. |
| `components/fck_bsp` | The module described (`kbsp` in kode-sdk), in `board/include`: J3 positions, expander bits, antenna legs, flash maps. Only describes. |
| `firmware/` | The module's own firmware: `fck-c5`, `fck-nrf` and the nRF's MCUboot, with their build and what has run ([`README.md`](../firmware/README.md)). |
| `c5/` | A firmware of your own for the module's C5, to start from (as kode-sdk's `c5/` is the Dot's): its pins, the flash map, the greeting the SDK reads, and the radio limits, always on. |
| `examples/` | One project per part: `module`, `subghz` and `wardriving` are tasks (`fck.h`), `wifi`, `radio`, `gnss` and `console_actions` are Active Objects (`fck_sys.h`). Run `module` first; `subghz` transmits, on request, and `wardriving` whenever it scans 2.4 GHz, after its top button. |
| `tests/host` | The rules in plain C, with Unity and nothing else, built for ESP-IDF's `linux` target: `fck_plan` and the drivers that need no ESP-IDF, one function at a time. |
| `tests/host_drv` | The drivers that do talk to ESP-IDF - pins, the expander, the UARTs, the restart hook, the CC1101 and the write of an image into the nRF over SMP - against a fake bus: GPIO, I2C with a TCA6408A on it, UART with a fake MCUboot behind it, SPI with a register-level CC1101, and an RMT that is never played (`main/fakes/`). |
| `tests/host_owner` | The owner (`fck_aos.c`) on FreeRTOS (`linux` target) against a model of the module - its expander, its two firmwares, its CC1101 - with a stand-in for `AO_Fck` behind `fck_ao.h` (`main/fakes/`). Finding the module, wrong module, the chips, flashing, rescue, sub-GHz, the events. `cases/test_15_any_call.c` makes every call of `fck.h` in a random order from a fixed seed (`FCK_ANY_SEED` and `FCK_ANY_ITERATIONS` replay or lengthen it), with good, NULL and out-of-range arguments, while the world changes (module out and in, a flicker of its power, a chip that says nothing, HOME and sleep, a flash in another task, a cancel at any moment): no code outside the call's `@return`, a refusal records nothing, and the module model never records an emission or a chip write that no call asked for. |
| `tests/host_ao` | `AO_Fck` itself on kode-sdk's real QP/C, at the pinned release v0.1.5 (a clone beside it, in `.gitignore`), with its own worker: its queue's and pool's margins, the tick, the signals the system publishes (`HOME_SIG`, `EXP_POWER_SIG`, `LOWPOWER_*`), its jobs and their answers. |
| `tests/host_facade` | The core (`fck.c`, `fck_core.h`) over an owner that writes down what it is asked (`main/fakes/fake_fck_aos.c`): every call gets to the owner's call it is named for, with its arguments and its refusals, one case per return value. `cases/test_any_call.c` makes every call it links in a random order, from a fixed seed, with good, NULL and out-of-range arguments over an owner that answers anything: no code outside the call's `@return`, no argument out of range reaches the owner, no line the gate keeps back reaches a chip. |
| `tests/host_standard` | The standard layer (`fck_standard.c`, `fck_events.c`) over a fake core: the `bool` and `fck_error()` of every call of `fck.h`, the refusal from an Active Object, the named events with their listeners and `fck_event_wait()`, and an any-call case in the layer. |
| `tests/host_sys` | The extended layer (`fck_sys.c`) on kode-sdk's real QP/C (the clone `host_ao` keeps), over a fake core, with stand-in Active Objects: every request answered once with its `rc` and `why`, the doors, the lent buffers, the queue-depth rule, the news with their margin, `fck_cancel`, `fck_sys_end` and an any-call case in the layer. |
| `tests/host_console` | `examples/console_actions` over an `fck_sys.h` that writes down what it is asked, with kodeOS's console registry and its rules: every row taken, every function making the request its name says - from its row, for the example's Active Object - and answering the console with what the SDK said, every answer and every piece of news a line on the log, and every variable a word the console sends, in every state. |
| `tests/target` | The SDK built for the esp32p4 against the pinned `kode-sdk`, every call of `fck.h` and `fck_sys.h` in `ALL[]`, and its RAM budget (`ram_budget.txt`). |
| `tests/sanitize.cmake` | ASan and UBSan for the host suites, under `tools/host_tests.sh --sanitize`. |
| `tools/` | `host_tests.sh` (the eight suites, in this order, checking that every declared case ran), `check_layers.sh` (fifteen checks: rules 1 to 14, and 13b), `check_misra.sh` (against a baseline the project keeps, with `build_cppcheck.sh`), `check_ram.sh`, `check_example_pins.sh`, and the checks' own tests. |

## The checks

The SDK builds with ESP-IDF v6.1 against `kode-sdk` v0.1.5 (API v4). The components never pin kode-sdk: the app does.

```sh
. <path-to-esp-idf>/export.sh
tools/host_tests.sh                    # eight suites, a few minutes; --sanitize for ASan and UBSan
tools/check_layers.sh                  # fifteen checks
tools/check_example_pins.sh
idf.py -C tests/target build           # about four minutes on a clean checkout
tools/check_ram.sh tests/target/build tests/target/ram_budget.txt
tools/check_misra.sh tests/target/build <baseline>   # cppcheck 2.22 on PATH, see tools/build_cppcheck.sh
```
