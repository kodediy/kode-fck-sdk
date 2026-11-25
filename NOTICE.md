# Licences of kode-fck-sdk

This file says which licence applies to each part of this repository. Read it before you distribute the SDK, or a firmware that you built with it.

| Part | Owner | Licence |
|---|---|---|
| everything in this repository | Kode | GPL-3.0-or-later, with the additional permission below |

This repository holds no code of another owner. The end of this file lists what a build downloads.

The SDK links [`kode-sdk`](https://github.com/kodediy/kode-sdk), which is GPL-3.0-or-later, so a firmware built with both is a covered work of that licence. `kode-sdk`'s own `NOTICE.md` says what applies to its parts.

## The code of Kode

Copyright (C) 2026 Kode.

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

The file `LICENSE` holds the full text of the GNU General Public License, version 3.

### Additional permission under GNU GPL version 3 section 7

An app built with this SDK also links `kode-sdk`, and `kode-sdk` holds RFAL, the NFC library of STMicroelectronics. The licence of RFAL, SLA0051, is not an open source licence. This repository holds no code of STMicroelectronics. The permission below is the one that `kode-sdk` gives for its own code, so that an app can be built with both.

If you modify this Program, or any covered work, by linking or combining it with the RF Abstraction Layer (RFAL) and the ST25R3916 driver of STMicroelectronics (or a modified version of those libraries), containing parts covered by the terms of the STMicroelectronics SLA0051 licence, the licensors of this Program grant you additional permission to convey the resulting work. Corresponding Source for a non-source form of such a combination shall include the source code for the parts of RFAL and of the ST25R3916 driver used as well as that of the covered work.

If you change the code of Kode, you can remove this permission from your version. Section 7 of the GNU General Public License allows this.

### Trademarks, under GNU GPL version 3 section 7(e)

This licence gives no right to use the names Kode, Kode Dot, kodeOS and Kody, or the character Kody, as trademarks. `TRADEMARKS.md` in the repository `kodediy/.github` says how you can use them.

## The firmware of the module

`firmware/` and `c5/` hold the firmware of the module's two chips. The code in them is code of Kode, under the GNU General Public License like the rest. Neither folder holds a library or a built image. Each project builds with a toolkit that you install, and the image that it builds contains code of that toolkit, under the licences of that code:

| Project | Toolkit | What the build takes from it | Licences |
|---|---|---|---|
| `firmware/fck-c5` and `c5/` | ESP-IDF v6.1, of Espressif Systems | ESP-IDF and the libraries it brings: FreeRTOS, the Wi-Fi libraries of Espressif and others. `firmware/fck-c5` also takes the Bluetooth controller and NimBLE | Apache-2.0 for ESP-IDF and for NimBLE. The other libraries keep their own licences, which the "Copyrights and Licenses" page of ESP-IDF lists |
| `firmware/fck-nrf` | nRF Connect SDK v3.4.2, of Nordic Semiconductor | Zephyr, with its shell and mcumgr; zcbor; nrfx; MCUboot, as the bootloader; and the build files of the nRF Connect SDK itself | Apache-2.0 for Zephyr, MCUboot and zcbor. BSD-3-Clause for nrfx. LicenseRef-Nordic-5-Clause for the code of Nordic Semiconductor in the nRF Connect SDK, which allows its use only with a chip of Nordic Semiconductor |

This repository distributes no image built from these folders. Read those licences before you distribute one.

## The components that the build downloads

The ESP-IDF component manager downloads other components when you build an app. They are not in this repository. Each one has its own licence, in its folder under `managed_components/`.

| Component | Owner | Licence | Used for |
|---|---|---|---|
| `kode-sdk` (`kodediy/kode` and the layers below it) | Kode, Quantum Leaps and STMicroelectronics | its own `NOTICE.md` says which applies where | the Kode Dot under the module |
| `espressif/esp-serial-flasher` | Espressif Systems | Apache-2.0 | writing the module's ESP32-C5 through its ROM loader |
