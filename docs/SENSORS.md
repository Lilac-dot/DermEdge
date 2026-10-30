# DermEdge band: sensors, validated and explained

Every part on the band as of 31 Oct 2026: what it does for DermEdge, how it's wired, what its
datasheet promises, what we've measured ourselves, and what the research says for and against
using it this way. Decisions and the full pin map are in [HARDWARE_PLAN.md](HARDWARE_PLAN.md).

**Status key:** ✅ checked on our board · 🟡 partly checked · ⬜ not checked yet

## At a glance

| Part | Job | ESP32 pins | Power | Status |
|---|---|---|---|---|
| ESP32 DevKit (CP2102 USB) | Microcontroller, Bluetooth LE | – | USB 5V | ✅ |
| ADXL345 accelerometer | Scratch detection, motion | I2C 21 (SDA), 22 (SCL) | 3V3 | ✅ |
| 1.8" ST7735 TFT | Screen | 23, 18, 5, 16, 4 (SPI) | **5V** | ✅ |
| 4-pad capacitive touch module | Navigation | 27 (OUT1) | 3V3 | ✅ |
| AD8232 ECG module | Stress, from heart rate and HRV | 32 (OUTPUT), 33 (LO+), 34 (LO−) | 3V3 | 🟡 |
| LM35 temperature sensor | Wrist skin temperature, for the stress model | 35 | **5V** | 🟡 |

All grounds are shared. Analog sensors use **ADC1** pins only (32-39), because ADC2 stops working
while Bluetooth is on.

## How the sensors become DermEdge's outputs

```
ADXL345 (800 Hz) ──► scratch detector ──────────────► scratch episodes ─┐
AD8232 ECG (250 Hz) ─► R-peaks ─► heart rate, HRV ──┐                   ├─► triggers ─► alert
LM35 (1 Hz) ─────────► wrist skin temperature ──────┴─► stress model ───┘     (with the photo
                                                                                and the journal)
```

The photo still decides whether there's eczema at all; the band only adds triggers. The
combined "flare risk" is a heuristic index, not a validated probability: no dataset anywhere
links wearable readings to eczema flares (see the research summary at the end).

---

## ESP32 DevKit

- **Job:** runs the scratch detector, the screen and touch, samples the ECG and temperature, and
  talks to the website over Bluetooth LE (encrypted, passkey-paired).
- **Specs:** dual-core 240 MHz, Bluetooth LE, 12-bit SAR ADC. The ADC is nonlinear near the
  rails and its offset varies chip to chip; the firmware uses the chip's eFuse line-fitting
  calibration.
- **Quirks we hit:** uploads only work on `/dev/cu.SLAB_USBtoUART` (Silicon Labs driver). Our
  board doesn't break out GPIO 36/39, which is why the ECG uses 32. Boot strapping pins (0, 2, 12,
  15) are left free.
- **Status:** ✅ running the band firmware and the bring-up tests.

## ADXL345 accelerometer: scratching

- **Job:** detects scratching from hand motion (strength, rhythm at 2.5-8 strokes per second, and
  where the energy sits in frequency). Also tells the ECG check whether you're still.
- **Wiring:** VCC 3V3, GND, SDA 21, SCL 22, **CS to 3V3** (I2C mode) and **SDO to GND** (address
  0x53); without those two it drops off the bus.
- **Specs:** 3 axes, ±2 to ±16 g, up to 3200 samples a second. We run it at 800 Hz.
- **Placement:** on the back of the hand you scratch with, flat and snug.
- **Our measurements (25 Oct, one wearer):** 16 clips of 20 s, 8 activities, trained on one round
  and tested on the other. The motion model found **4 of 4 scratch clips** with **0 false
  episodes** in the 12 other clips (waving, typing and so on). The band's C code and the Python
  model agree on all 284 windows. The ADXL345's noise at 800 Hz (14-22 mg) buried the skin
  vibration, so vibration features don't help on this part.
- **Research, for:** wrist-worn accelerometers measure night-time scratching well: total scratch
  duration agreed with video at r = 0.82 (Mahadevan et al. 2021, N = 25), and smartwatch scratch
  metrics tracked eczema severity in 50 children (r = 0.60-0.64, Iwai et al. 2026). A hand-mounted
  sensor with haptic feedback cut nightly scratching by 28% (Yang et al. 2025, JAMA Dermatology).
- **Research, against:** wrist actigraphy does worse in itchy patients and misses finger-only
  scratching (Yang et al. 2021 systematic review; the back-of-hand placement is our answer to
  that). Models from few people are fragile: a 7-person wrist model reached F1 0.45 (Xing et al.
  2024).
- **Status:** ✅ working, with a trained model on the band. **Limit:** one wearer so far; all three
  of us record next for a leave-one-person-out test, then the volunteer day.

## ST7735 TFT screen

- **Job:** shows pairing codes, recording state, readings and alerts.
- **Wiring (header order on the red V1.1 board):** LED 3V3, SCK 18, SDA 23, A0 16, RESET 4,
  CS 5, GND, **VCC 5V**.
- **Specs:** 1.8", 128 × 160, SPI.
- **Quirks:** VCC must be 5V: on 3V3 its regulator starves the panel and it stays white. The
  panel wants RGB order (`MADCTL 0xA0`); in BGR, pink shows as lilac. A white screen once turned
  out to be a broken SDA jumper.
- **Status:** ✅ working.

## Capacitive touch module (4 pads, OUT1 used)

- **Job:** the band's only control. Hold 2 s: start/stop recording; tap: next view; double tap:
  dismiss an alert.
- **Wiring:** VCC **3V3** (on 5V its outputs would send 5V into the ESP32), GND, OUT1 to GPIO 27.
  OUT2-4 are free for more buttons (GPIO 25, 26, 13).
- **Specs:** output goes high while a pad is touched.
- **Our measurements (31 Oct):** the bring-up test logged every press and release (4 of 4) after
  the OUT1 wire was reattached.
- **Status:** ✅ working.

## AD8232 ECG module: stress

- **Job:** records a single-lead ECG. The band finds each heartbeat (R-peak) and turns the beat
  timing into heart rate and HRV (RMSSD, SDNN), which feed the stress model.
- **Wiring (header order):** GND, 3.3v to **3V3** (its maximum is 3.6 V), OUTPUT to GPIO 32,
  LO− to 34, LO+ to 33, SDN to 3V3 (always on).
- **Electrodes, Lead I:** RA on the inside of the right wrist, LA on the inside of the left wrist
  under the band, RL on the back of a hand. Clean dry skin, fresh gel pads.
- **How it's used:** a **seated, still, 1-2 minute check**, not all-day wear. The lead runs across
  the body, and movement and forearm muscles add noise.
- **Specs:** a heart-rate front end with built-in filtering and lead-off detection: LO+ or LO−
  goes high when an electrode is off. The trace is good for beat timing, not for diagnosis; it
  is not a medical device.
- **Our measurements (31 Oct):** 🟡 the ESP32 samples it at ~245 per second (target 250), and the
  lead-off pins correctly reported "off" with no electrodes attached. The band firmware's beat
  detector and HRV maths pass host tests on a synthetic ECG with T waves, baseline wander and
  120 mV of noise (68 and 92 bpm found within 1 bpm; RMSSD drops from about 52 to 10 ms under
  simulated stress; a hand-worked window matches to 0.05 ms). **Not yet done:** an on-body trace,
  and a heart-rate check against a reference (a chest strap, or a 60-second pulse count).
- **Research, for:**
  - Lead I is enough for beat timing: an Apple Watch's Lead I ECG matched a 3-lead reference
    within 1.15% for beat-to-beat intervals and heart rate at rest (Bonneval et al. 2025).
  - Short checks are valid: RMSSD from 10 s already agreed with a 5-minute recording at r ≈ 0.85,
    and 30-120 s does better (Munoz et al. 2015, 3,387 adults).
  - HRV features carried over to a different stress dataset better than deep models trained on
    WESAD (Prajod & André 2022).
  - Acute stress increases scratching in people with eczema but not in healthy people (Mochizuki
    et al. 2019).
- **Research, against:**
  - HRV is more fragile than heart rate: the same Apple Watch's HRV agreed only moderately (31%
    error at rest).
  - Eczema changes the autonomic system itself: patients had higher RMSSD than controls
    (Boettger et al. 2008), and a higher heart rate with a 200% jump in sympathetic activity after
    scratching (Tran et al. 2010). WESAD's subjects didn't have eczema, and itching or scratching
    can move HRV on its own. The firmware should skip stress windows that overlap scratching.
  - Lab-trained stress models lose accuracy in daily life; walking raises heart rate like stress
    does. The check is taken seated and still for this reason.
  - We found no published validation of the AD8232's R-peaks or HRV against a clinical ECG, only
    hobby projects, so our own reference check matters.
- **Status:** 🟡 wired and sampling; on-body validation and the retrained stress model are next.

## LM35 temperature sensor: wrist skin temperature

- **Job:** one input to the stress model (skin temperature at distal sites drops slightly under
  stress). **Not** an eczema marker: dermatologists told us skin temperature is too variable for
  that, and the lesion-vs-healthy pair was dropped on 31 Oct.
- **Wiring (flat face toward you, legs down):** left +Vs to **5V/VIN** (it needs at least 4 V),
  middle Vout to GPIO 35, right GND. A 0.1 µF capacitor across +Vs and GND at the sensor; on a
  long lead, a 75 Ω + 1 µF damper from Vout to GND stops it oscillating.
- **Specs:** linear, 10 mV per °C, so skin (30-36 °C) gives 0.30-0.36 V, read with the ADC at
  0 dB for the finest steps. Datasheet accuracy is about ±0.5-1 °C depending on grade; the model
  uses changes from the wearer's calm baseline, so the offset matters less.
- **Placement:** under the band against the inside of the wrist, on a lead away from the ESP32's
  own heat. The TO-92 package touches skin poorly and reacts slowly: thermal paste or a small
  copper disc on the flat face helps; let it settle a minute.
- **Calibration:** the LM35 is within about 0.5-1 °C, but the ESP32's ADC is off by tens of
  millivolts near 0.3 V, which is several degrees. Each band is calibrated once against a
  reference thermometer (`temp cal <°C>` over USB serial, with both settled side by side for 5
  minutes): a one-point offset, stored on the band. Offsets over 10 °C are refused, because that
  much is a wiring fault, not inaccuracy.
- **Our measurements (31 Oct):** 🟡 three early runs read 34.8-35.5 °C, then about 63 °C, then
  about 8 °C: a loose connection (the firmware rejects anything outside 10-50 °C, so those were
  never sent). After the wiring settled it read a steady 31.0-31.3 °C, and the calibration
  command was checked on the board (a 90 °C reference refused, a 30 °C one stored as −0.49 °C).
  Still to do: calibrate against a real thermometer, then an air-vs-skin check.
- **Research, for:** stress lowers skin temperature at distal sites such as the fingers (Vinkers et
  al. 2013); in WESAD it dropped under stress in 12 of 15 people. Heat and sweating are
  established itch triggers (Murota et al. 2017), so a rise is also worth showing.
- **Research, against:** the effect depends on body site (none at proximal sites) and sex
  (Vinkers et al. 2013), and the wrist sits in between, so expect small changes. Going outside or
  into air conditioning moves skin temperature too.
- **Status:** 🟡 steady now; needs its one-time calibration against a thermometer.

## Dropped sensors

| Sensor | Dropped | Why |
|---|---|---|
| GSR (skin conductance) | 31 Oct | Replaced by ECG, in the band firmware and on the website (31 Oct) |
| Skin moisture patch | 31 Oct | Dropped with GSR |
| NTC thermistor | 31 Oct | Replaced by the LM35 |
| Second LM35 (lesion vs healthy) | 31 Oct | Thin, conflicting evidence; no data linking it to flares |
| MAX30100 (PPG) | 31 Oct | The ECG gives heart rate and HRV directly |
| Gesture and ultrasonic sensors (add-on board) | – | Not needed |

## Safety

- With ECG electrodes on, the band runs from a power bank or a laptop **on battery**, never a
  laptop plugged into the wall: the AD8232 board has no mains isolation.
- Bluetooth is encrypted and passkey-paired, so nobody nearby can read the band or send it
  commands.
- Alerts are suggestions, not medical advice.

## Research summary: how valid is the whole design?

| Part | Feasible? | Evidence |
|---|---|---|
| Scratch detection (ADXL345) | Yes | **Strong.** Wrist and hand sensors agree with video |
| ECG stress check (AD8232, Lead I) | Yes, seated | **Moderate.** Heart rate is robust, HRV more fragile, and eczema itself shifts HRV |
| Wrist temperature (LM35) | Yes | **Weak** as a stress input, so it's one feature among several |
| Stress as an eczema trigger | – | **Plausible but not proven.** Big disruptions worsen eczema (after the 1995 Hanshin earthquake, 38% of patients in the worst-hit area worsened against 7% elsewhere: Kodama et al. 1999), but most studies are retrospective (Picardi & Abeni 2001), and a meta-analysis found a very small pooled effect, mostly from asthma studies (Chida et al. 2008) |
| Flare-risk formula | Yes | **Heuristic.** Even the best per-patient forecasts of daily eczema severity, from past severity and treatment, improve on chance only modestly (Hurault et al. 2020) |

**What we do about it:** stress checks only while the accelerometer says you're still; skip
stress windows that overlap scratching; calibrate to each wearer's calm baseline; validate the
ECG against a reference; and present the output as a trigger alert, not a diagnosis.

### References

- Bonneval et al. 2025, *Sensors*: [10.3390/s25082380](https://doi.org/10.3390/s25082380)
- Boettger et al. 2008, *J Dermatol Sci*: [10.1016/j.jdermsci.2008.08.003](https://doi.org/10.1016/j.jdermsci.2008.08.003)
- Chida et al. 2008, *Psychosom Med*: [10.1097/PSY.0b013e31815c1b71](https://doi.org/10.1097/PSY.0b013e31815c1b71)
- Hurault et al. 2020, *Clin Exp Allergy*: [10.1111/cea.13717](https://doi.org/10.1111/cea.13717)
- Iwai et al. 2026, *J Clin Med*: [10.3390/jcm15093380](https://doi.org/10.3390/jcm15093380)
- Kodama et al. 1999, *J Allergy Clin Immunol*: [10.1016/s0091-6749(99)70130-2](https://doi.org/10.1016/s0091-6749(99)70130-2)
- Mahadevan et al. 2021, *npj Digit Med*: [10.1038/s41746-021-00402-x](https://doi.org/10.1038/s41746-021-00402-x)
- Mochizuki et al. 2019, *Br J Dermatol*: [10.1111/bjd.16921](https://doi.org/10.1111/bjd.16921)
- Munoz et al. 2015, *PLoS One*: [10.1371/journal.pone.0138921](https://doi.org/10.1371/journal.pone.0138921)
- Murota et al. 2017, *Allergol Int*: [10.1016/j.alit.2016.10.005](https://doi.org/10.1016/j.alit.2016.10.005)
- Picardi & Abeni 2001, *Psychother Psychosom*: [10.1159/000056237](https://doi.org/10.1159/000056237)
- Prajod & André 2022, arXiv: [2210.06225](https://arxiv.org/abs/2210.06225)
- Tran et al. 2010, *Acta Derm Venereol*: [10.2340/00015555-0890](https://doi.org/10.2340/00015555-0890)
- Vinkers et al. 2013, *Stress*: [10.3109/10253890.2013.807243](https://doi.org/10.3109/10253890.2013.807243)
- Xing et al. 2024, *Sensors*: [10.3390/s24113364](https://doi.org/10.3390/s24113364)
- Yang et al. 2021, *JAAD Int*: [10.1016/j.jdin.2021.06.005](https://doi.org/10.1016/j.jdin.2021.06.005)
- Yang et al. 2025, *JAMA Dermatol*: [10.1001/jamadermatol.2024.5697](https://doi.org/10.1001/jamadermatol.2024.5697)
