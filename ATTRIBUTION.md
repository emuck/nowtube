# Attribution

This project is derived from **previoustube** by Ian Levesque.

- Original repository: https://github.com/previoustube/previoustube
- Original author: Ian Levesque <ian@ianlevesque.org>
- Original license: MIT

All files carrying the SPDX header `SPDX-FileCopyrightText: 2023 Ian Levesque` are
reproduced and modified under the terms of Ian's original MIT license, which is
reproduced in full in the `LICENSE` file.

New files added in this project carry their own copyright notices and are also
released under the MIT License.

## Microphone implementation

Spectrum microphone support is adapted from the open-source Nextube-Remaster
project by MrToast99. In particular, that project identified the onboard
microphone path as an analog electret capsule with LMV321 preamp on
GPIO35 / ADC1_CH7 and demonstrated the working `adc_continuous` sampling plus
Goertzel-band spectrum approach used here. The optional SHT30 indoor
temperature/humidity service follows the same sensor identification and
measurement command/CRC handling from Nextube-Remaster.

## Third-party components

| Component | Source | License |
|---|---|---|
| LVGL | https://lvgl.io | MIT |
| PCF8563 driver | ESP-IDF component | MIT |
| fpm (fixed-point math) | https://github.com/MikeLankamp/fpm | MIT |
| Oswald font | Google Fonts | OFL |
| Phosphor Icons | https://phosphoricons.com | MIT |
| inih | https://github.com/benhoyt/inih | BSD-3-Clause |
| toml++ | https://github.com/marzer/tomlplusplus | MIT |
| NeoPixel / LED strip encoder | Espressif Systems (Shanghai) CO LTD, 2021–2022 | Apache-2.0 |
| timegm | Kungliga Tekniska Högskolan (KTH), 1997 | BSD-3-Clause (license in source header) |
| touchpad | Espressif Systems (Shanghai) PTE LTD, 2015–2016 | Apache-2.0 |
| Nextube-Remaster microphone work | https://github.com/MrToast99/Nextube-Remaster | MIT |
