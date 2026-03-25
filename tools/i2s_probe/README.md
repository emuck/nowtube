# I2S Microphone Pin Probe

Standalone ESP32 firmware used to identify which GPIO the Nextube's microphone
is connected to. Kept as a historical record of what was tested and why the
spectrum analyzer feature was dropped from the roadmap.

---

## Testing History

### Phase 1 — ADC Analog Probe (complete, result: negative)

**Theory:** The stock firmware binary contains the string
`"I2S built-in ADC/DAC only support on I2S0"`, which is an error guard in
ESP-IDF's old I2S-ADC driver. This suggested the mic might be analog, sampled
via the I2S-ADC DMA path on one of the four input-only ADC1 pins.

**Tested:** GPIO34, GPIO35, GPIO36, GPIO39 — all four candidates — using
ESP-IDF v5 ADC continuous mode (the v5 replacement for the old I2S-ADC API).

**Result:** All pins showed **0.0 RMS** with and without audio. No analog mic
signal is present on any of these pins.

**Conclusion:** The mic is not an analog device, or the binary string was a
compile-time inclusion unrelated to runtime behavior.

---

### Phase 2 — PDM Digital Mic Probe (complete, result: negative)

**Theory:** MEMS digital microphones most commonly use PDM protocol (1 CLK
output + 1 DATA input). The probe swept all plausible CLK×DIN combinations
using ESP-IDF v5 `i2s_pdm_rx`.

**Tested:** 35 combinations — 7 CLK candidates (GPIO26, 27, 25, 33, 21, 18, 5)
× 5 DIN candidates (GPIO34, 35, 36, 39, 32).

**Result:** No combination showed signal above noise. No PDM mic found.

**Code:** `main/i2s_probe.cpp` — this is the Phase 2 firmware. Phase 1 ADC
probe code was not retained.

---

### Phase 3 — Standard I2S 3-Wire Probe (not yet attempted)

**Theory:** Standard I2S digital microphones use a 3-wire protocol:
BCK (bit clock) + WS (word select / LRCLK) + DIN (data in). This is a
different protocol from PDM and was not covered by the Phase 2 sweep.

The ESP32 I2S0 hardware-default pin assignment is:

| Signal | GPIO | Notes |
|---|---|---|
| BCK | GPIO26 | Also LCD2 CS — free in probe firmware |
| WS | GPIO25 | Also DAC out pin |
| DIN | GPIO34 | I2S0 DIN default, input-only |

This specific combination — **BCK=GPIO26, WS=GPIO25, DIN=GPIO34** — is the
most likely wiring if the board designer used ESP32 I2S0 defaults. It was
never tested as a unit because the PDM probe only sweeps CLK+DIN pairs; WS
is a third signal that PDM mode doesn't use.

**To run this test:** A new probe firmware would be needed that configures
I2S0 in standard (`I2S_COMM_FORMAT_STAND_I2S`) RX mode with the three pins
above, reads samples, and reports RMS with and without audio — same approach
as the PDM probe but using `i2s_std_rx` instead of `i2s_pdm_rx`.

**Expected outcome:** Low — standard I2S mics are uncommon in compact consumer
devices; PDM is far more typical. But it is the one remaining untested protocol
and would close the question permanently.

---

## Overall Assessment

| Protocol | Coverage | Result |
|---|---|---|
| Analog ADC (I2S-ADC mode) | All 4 ADC1 input pins | No signal |
| PDM digital | 35 CLK×DIN combinations | No signal |
| Standard I2S 3-wire | Not tested | Unknown |

The most likely explanation for the Phase 1 and 2 failures is that the
microphone is either **not populated** on hardware revision 1.31, or requires
a **MICBIAS power enable line** that was never identified or asserted during
probing. Either condition would produce flat silence across all protocol tests.

The spectrum analyzer feature is **not on the roadmap**. This directory is
kept as a reference in case the question is ever revisited.

---

## Restoring Main Firmware

After running any probe:

```bash
cd ../..
idf.py -p /dev/ttyUSB0 build flash monitor
```
