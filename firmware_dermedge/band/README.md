# DermEdge band firmware (ESP-IDF)

The wrist band: scratch detection from the ADXL345, a touch pad for start/stop, the 1.8" ST7735
screen in the website's petal and sage palette, and a Bluetooth LE link to the DermEdge website,
which relays readings to the laptop hub (`app/band.py`).

```
./idf.sh build
./idf.sh -p /dev/cu.SLAB_USBtoUART flash monitor
sh test/run.sh        # host tests: scratch detector, touch gestures, state machine
sh test/preview.sh    # renders every screen with the real ui.c to test/preview.png
python3 tools/make_fonts.py   # regenerates main/fonts.c from the website's fonts
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

Every second, the last 2 s of motion (gravity removed) counts as scratchy when it is strong
enough (0.04-1.5 g RMS) and rhythmic at 2.5-8 strokes a second (autocorrelation peak of the most
active axis). Two scratchy seconds start an episode and two calm ones end it. The thresholds in
`main/scratch.h` are first guesses from synthetic motion and still need tuning on real
recordings (the hub's opt-in raw clips are for that).

Known limit: this is motion only. The ADAM sensor study (Chun et al., "A skin-conformable wireless sensor to objectively quantify symptoms of pruritus", *Sci. Adv.* 7, eabf9405, 2021)
found the most telling
scratch signal is a 100-800 Hz skin vibration, which 100 Hz sampling can't see: a 100 Hz
smartwatch took hand-waving for scratching and missed finger-only scratching. The ADXL345 runs at
up to 3200 Hz, so the next step is a vibration-band check alongside the rhythm test, with the
sensor on the back of the hand as in that study.

## Bluetooth protocol

Service `a3c87510-8ed3-4bdf-8a39-a01bebede295`, all text, every characteristic behind an
encrypted, passkey-paired link:

| Characteristic | | |
|---|---|---|
| `...7511` status | read / notify, 1 Hz | `{"id","s","r","src","label","el","n","act","q","alert"}` |
| `...7512` data | notify | one reading per message in the hub's `/readings` shape plus `"r"` (recording number), `t` in seconds since that recording started |
| `...7513` command | write | `start[:label]`, `stop`, `dismiss`, `alert:<level>\|<reason>\|<tip>`, `token:<device token>`, `unpair` |
