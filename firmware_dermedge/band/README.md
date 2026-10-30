# DermEdge band firmware (ESP-IDF)

The wrist band: scratch detection from the ADXL345, a heart check from an AD8232 ECG, wrist
temperature from an LM35, a touch pad for start/stop, the 1.8" ST7735
screen in the website's teal theme, and a Bluetooth LE link to the DermEdge website,
which relays readings to the laptop hub (`app/band.py`).

```
./idf.sh build
./idf.sh -p /dev/cu.SLAB_USBtoUART flash monitor
sh test/run.sh        # host tests: scratch detector, touch gestures, state machine
sh test/preview.sh    # renders every screen with the real ui.c to test/preview.png
python3 tools/make_fonts.py   # regenerates main/fonts.c from the website's fonts
python3 tools/record.py check # is the accelerometer streaming 800 Hz cleanly?
```

![Every screen, rendered on the Mac by test/preview.sh](test/preview.png)

The screens use the website's fonts (Young Serif for titles, Figtree for text, converted to 4-bit
anti-aliased bitmaps), its teal, green and white theme (pale tints a step deeper, since this panel
washes them out) and its DermEdge drop logo, drawn from the website's SVG by `tools/make_art.py`.
The panel wants RGB order (`MADCTL 0xA0`); with the BGR bit set, colours swap red and blue.

## Wiring

| Part | Pin | ESP32 |
|---|---|---|
| TFT | VCC / GND / LED | 5V / GND / 3V3 |
| | SDA / SCK / CS / A0 / RESET | 23 / 18 / 5 / 16 / 4 |
| Add-on board (ADXL345) | VCC / GND | 3V3 / GND |
| | SDA / SCL | 21 / 22 |
| | CS / SDO | 3V3 / GND (I2C mode, address 0x53) |
| 4-pad touch module | OUT1 (OUT2-4 unused for now) | 27 (high while touched) |
| AD8232 ECG module | 3.3v / GND | **3V3** (not 5V: its maximum is 3.6 V) / GND |
| | OUTPUT / LO+ / LO− / SDN | 32 / 33 / 34 / 3V3 |
| | electrodes | RA inside of the right wrist, LA inside of the left wrist, RL back of a hand |
| LM35 (flat face toward you, legs down) | left / middle / right | **5V** / 35 / GND |

With the ECG electrodes on, power the band from a power bank or a laptop on battery, never a
laptop plugged into the wall: the AD8232 board has no mains isolation. Every part, with its
checks and the research behind it: `docs/SENSORS.md`.

## Heart (ECG) and skin temperature
A skin task (`main/skin_adc.c`, maths in `main/skin.c`) samples the AD8232 at 250 Hz from a
hardware timer and reads the LM35 on every sample, averaged over each second, both on ADC1.

- **Heart check:** sit still with your hands resting and the three electrodes on. The band finds
  each heartbeat (distance from a slow baseline against a decaying envelope of recent peaks, so
  electrode polarity doesn't matter and T waves aren't counted) and drops intervals that jump
  more than 30% from the recent ones. Every **30 s of unbroken electrode contact** becomes one
  reading, heart rate, RMSSD and SDNN, if the hand was still for 27 of those seconds (motion under
  0.03 g, no scratching), at least 15 clean beats were found and under 20% were artifacts.
  Electrodes coming off starts a new window. Readings go out only while recording.
- **Skin temperature:** an LM35 under the band, 10 mV per °C, read with the ADC at 0 dB for the
  finest steps. It counts once it has stayed in 10-50 °C and steady for 3 s, and goes out as a
  5 s average.
- Nothing is sent while a reading isn't real, so a gap means "no data", never zeros. The
  Recording's Sensors page (tap) shows both; the header says **SIM** while they're simulated.
- The stress model itself runs on the website (`web/models/stress_ecg.json`, still to come).

Over USB serial (`./idf.sh -p /dev/cu.SLAB_USBtoUART monitor`, or any serial terminal at 115200):

| Command | Does |
|---|---|
| `skin on` / `skin off` | once a second: the ECG and LM35 pins' millivolts, electrodes on/off, beats, heart rate, temperature |
| `sim on` / `sim stress` / `sim off` | a simulated ECG (about 68 bpm calm, 92 stressed) and temperature instead of the sensors |
| `start` / `stop` | start or stop a recording, like the touch pad (works without the website) |
| `peek` / `peek 40` | the next readings exactly as they'll go out over Bluetooth (from position 40) |
| `outbox clear` | drop every reading waiting to be sent (test recordings, say) |

## Offline storage

Readings wait for the phone in flash (`main/store.c`, the 2.4 MB `outbox` partition in
`partitions.csv`): about 159,000 readings, roughly 35 hours of recording, kept across restarts and
flat batteries. Items are written in two steps, so one half-written when the power goes is
skipped, and marked sent only after they're out, so a restart sends anything unconfirmed again.
When it fills, older data is kept and new data thins out: motion every 10 s from 90% full, only
scratch episodes from 98% (the band says "Memory full: scratches"). Recording numbers are kept in
NVS, so they stay unique across restarts. Erasing a 4 KB sector pauses the chip for about 45 ms
about every 2 minutes while recording, which costs a few accelerometer samples.

## States and screens

The device: `BOOT` → `PAIRING` (shows the 6-digit Bluetooth code) → `READY` ⇄ `RECORDING` →
`SYNCING` → `READY`, `ERROR` while the accelerometer is missing (`main/app_sm.c`). On top of it,
`main/ui_flow.c` picks the screen and the warning, so a hold shows its progress, a start is
confirmed and a stop shows what was saved:

| Screen | When |
|---|---|
| Connect | not paired: how to connect, or the pairing code |
| Home | ready: phone and ECG electrode status, "Hold the pad to start" |
| Hold to start / Hold to stop | while the pad is held: a ring fills over 2 s; letting go cancels |
| Started | 1.5 s after a recording starts |
| Recording | timer, scratch episodes, "Scratching detected" while it's happening |
| Saved | 4 s after stopping: length, scratches, and whether it's synced |
| Sensors | tap on Home or Recording: movement, ECG, skin temp, phone, memory, in words |
| Syncing, Sensor problem | readings going out; the accelerometer missing |
| Flare alert | over anything, until a double tap (or the website) dismisses it |

Users see plain status, not raw numbers (those are on the serial line: `skin on`). Warnings sit
in a strip along the bottom, most urgent first: band memory 80% full; temperature sensor not
reading for 10 s during a recording, once it had worked; recording offline. ECG electrodes coming
off doesn't warn: the heart check is a few minutes, and they're meant to come off afterwards.

| Touch | Does |
|---|---|
| Hold 2 s | start / stop recording (also works offline) |
| Tap | Sensors page and back |
| Double tap | dismiss an alert |

## Scratch detection

Wear the add-on board on the **back of the hand** you scratch with. The ADXL345 samples at
800 Hz; a sensor task drains its FIFO every 10 ms and a detector task (`main/scratch.c`) turns
each second into features: motion strength and rhythm (2.5-8 strokes a second, at 100 Hz after
decimation), band powers from 2 to 400 Hz, and whether the 80-400 Hz vibration pulses in the
stroke rhythm. A logistic model in `main/scratch_model.h` scores them (threshold 0.7); two
scratchy seconds start an episode and four calm ones end it.

Training it on your own recordings:

```
python3 tools/record.py check      # 10 s still: sample rate, dropouts, noise floor
python3 tools/record.py session    # 8 activities x 20 s, twice, into ~/DermEdge-recordings (not git)
python3 tools/tune.py              # held-out results: rule vs motion vs vibration vs all features
python3 tools/tune.py --write      # train on everything, write main/scratch_model.h, then rebuild
```

`tools/replay.c` runs the band's own detector over a recording, so training uses exactly the
band's features. The first model (one wearer, 16 clips) found every scratch clip with no false
episodes on the held-out round, using motion features only: the ADXL345's noise at 800 Hz
(14-22 mg) buries the skin vibration the ADAM study (Chun et al., *Sci. Adv.* 7, eabf9405, 2021)
relies on. Results and limits: `docs/HARDWARE_PLAN.md` section 6. The host tests check the
detector's logic with the hand-set rule (`-DSCRATCH_FORCE_RULE`), including that waving is
rejected and fingertip-only scratching caught.

## Bluetooth protocol

Service `a3c87510-8ed3-4bdf-8a39-a01bebede295`, all text, every characteristic behind an
encrypted, passkey-paired link:

| Characteristic | | |
|---|---|---|
| `...7511` status | read / notify, 1 Hz | `{"id","s","r","src","label","el","n","act","q","alert","ecg","hr","tc","sim"}` |
| `...7512` data | notify | one reading per message in the hub's `/readings` shape plus `"r"` (recording number), `t` in seconds since that recording started |
| `...7513` command | write | `start[:label]`, `stop`, `dismiss`, `alert:<level>\|<reason>\|<tip>`, `token:<device token>`, `unpair` |
