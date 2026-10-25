# DermEdge band firmware (ESP-IDF)

The wrist band: scratch detection from the ADXL345, a touch pad for start/stop, the 1.8" ST7735
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
| Grove GSR module | SIG / VCC / GND | 32 / 3V3 / GND |
| NTC thermistor, 10 kΩ B3950 | divider: 3V3 → 10 kΩ → **GPIO 34** → thermistor → GND | 34 |

## Skin sensors (GSR and skin temperature)

A skin task (`main/skin_adc.c`, maths in `main/skin.c`) reads GSR at 4 Hz and the thermistor
once a second on ADC1, and the band sends them in the website's format: GSR four readings to a
message, skin temperature as a 5 s average. Nothing is sent while a reading isn't real:
electrodes off the skin, a sensor unplugged (an unplugged GSR pin floats, which the task catches
by briefly pulling it up and down; a real module's output doesn't move), or the thermistor not yet
steady for 3 s. The recording's detail page (tap) shows both, and the header says **SIM** while
they're simulated.

Over USB serial (`./idf.sh -p /dev/cu.SLAB_USBtoUART monitor`, or any serial terminal at 115200):

| Command | Does |
|---|---|
| `skin on` / `skin off` | print the pins' millivolts, the readings and a verdict once a second |
| `gsr cal` | with the electrodes **off** the skin: store this module's no-skin reading (about 512) |
| `sim on` / `sim stress` / `sim off` | simulated calm / stressed readings instead of the sensors |
| `start` / `stop` | start or stop a recording, like the touch pad (works without the website) |
| `peek` | the last few readings exactly as they'll go out over Bluetooth |

## States and screens

The device: `BOOT` → `PAIRING` (shows the 6-digit Bluetooth code) → `READY` ⇄ `RECORDING` →
`SYNCING` → `READY`, `ERROR` while the accelerometer is missing (`main/app_sm.c`). On top of it,
`main/ui_flow.c` picks the screen and the warning, so a hold shows its progress, a start is
confirmed and a stop shows what was saved:

| Screen | When |
|---|---|
| Connect | not paired: how to connect, or the pairing code |
| Home | ready: phone and skin sensor status, "Hold the pad to start" |
| Hold to start / Hold to stop | while the pad is held: a ring fills over 2 s; letting go cancels |
| Started | 1.5 s after a recording starts |
| Recording | timer, scratch episodes, "Scratching detected" while it's happening |
| Saved | 4 s after stopping: length, scratches, and whether it's synced |
| Sensors | tap on Home or Recording: movement, skin sensor, skin temp, phone, memory, in words |
| Syncing, Sensor problem | readings going out; the accelerometer missing |
| Flare alert | over anything, until a double tap (or the website) dismisses it |

Users see plain status, not raw numbers (those are on the serial line: `skin on`). Warnings sit
in a strip along the bottom, most urgent first: band memory 80% full; skin sensor off the skin or
temperature sensor not reading for 10 s during a recording, once it had worked; recording offline.

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
| `...7511` status | read / notify, 1 Hz | `{"id","s","r","src","label","el","n","act","q","alert"}` |
| `...7512` data | notify | one reading per message in the hub's `/readings` shape plus `"r"` (recording number), `t` in seconds since that recording started |
| `...7513` command | write | `start[:label]`, `stop`, `dismiss`, `alert:<level>\|<reason>\|<tip>`, `token:<device token>`, `unpair` |
