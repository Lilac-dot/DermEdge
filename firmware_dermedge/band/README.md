# DermEdge band firmware (ESP-IDF)

The wrist band: scratch detection from the ADXL345, a touch pad for start/stop, the 1.8" ST7735
screen in the website's old pink palette (moving to the site's teal), and a Bluetooth LE link to the DermEdge website,
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
anti-aliased bitmaps) and its palette pushed much deeper, since this panel washes pastels out to
white. The panel wants RGB order (`MADCTL 0xA0`); with the BGR bit set, pink shows as lilac.

## Wiring

| Part | Pin | ESP32 |
|---|---|---|
| TFT | VCC / GND / LED | 5V / GND / 3V3 |
| | SDA / SCK / CS / A0 / RESET | 23 / 18 / 5 / 16 / 4 |
| Add-on board (ADXL345) | VCC / GND | 3V3 / GND |
| | SDA / SCL | 21 / 22 |
| | CS / SDO | 3V3 / GND (I2C mode, address 0x53) |
| 4-pad touch module | OUT1 (OUT2-4 unused for now) | 27 (high while touched) |

## States

`BOOT` → `PAIRING` (shows the 6-digit Bluetooth code) → `READY` ⇄ `RECORDING` → `SYNCING` → `READY`,
`ERROR` while the accelerometer is missing, and a flare alert card over any state.

| Touch | Does |
|---|---|
| Hold 2 s | start / stop recording (also works offline; readings wait up to an hour) |
| Tap | flip between the main and detail view |
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
