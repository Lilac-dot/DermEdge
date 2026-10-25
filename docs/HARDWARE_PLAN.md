# DermEdge hardware and architecture plan

Where the hardware stands, which sensors are coming and on which pins, and how the pieces talk.
For building an app against it, see [DERMEDGE_APP_GUIDE.md](DERMEDGE_APP_GUIDE.md); for the
band's firmware, [firmware_dermedge/band/README.md](../firmware_dermedge/band/README.md).

## 1. The system

![DermEdge: full system](architecture/0_full_system.png)

A photo goes through three image stages; the wearable and the journal add **triggers**, combined
by a noisy-OR into a trigger index. Flare risk is the image score times the trigger index, so it
stays near zero without eczema in the photo. The Wearable tab shows trigger alerts on their own.

![Wearable triggers and flare risk](architecture/4_wearable_flare_risk.png)

Since these diagrams were drawn, **night scratching is real**: the DermEdge band's scratch
episodes feed the trigger "scratching (DermEdge band)" (full strength at 30 episodes in 8 hours,
the same scale as the simulated one). Stress, moisture and the journal work as drawn.

The other diagrams: [stage 1](architecture/1_stage1_clip_rash.png),
[stage 2](architecture/2_stage2_resnet_gradcam.png), [stage 3](architecture/3_stage3_severity.png),
[the ESP32-CAM on-device screen](architecture/5_esp32_on_device.png).

## 2. How the band talks to everything

```
 add-on board on the back of the hand          ESP32 band (firmware_dermedge/band)
 ┌──────────────────────────────┐             ┌───────────────────────────────────────────────┐
 │ ADXL345 ── I2C 800 Hz ───────┼────────────►│ sensor task (10 ms) ─► detector task (1 s)    │
 │ touch pad OUT1 ──────────────┼────────────►│ state machine ◄── touch gestures               │
 └──────────────────────────────┘             │ ST7735 screen ◄── UI      outbox (1 h offline) │
                                              └──────────────┬────────────────────────────────┘
                                                             │ Bluetooth LE, passkey-paired, encrypted
                                                             ▼
                          website / app (web/js/band.js) ── /api/band/* ──► browser storage (website)
                                                                        └─► laptop hub, app/band.py (login)
```

- **Band:** records sessions started from its touch pad or the app, detects scratch episodes,
  and buffers about an hour of readings while disconnected.
- **Website (Vercel):** pairs with the band from the Home tab, relays its readings into recording
  sessions stored in the browser, feeds scratching into the flare risk, and sends high-risk
  alerts back to the band's screen (a double tap there dismisses them on both).
- **Hub (laptop):** the same `/api/band/*` API in SQLite, behind a login, for the laptop version.

## 3. Hardware in hand

| Part | Status | Notes |
|---|---|---|
| ESP32 DevKit (CP2102 USB) | working | Silicon Labs driver on macOS; upload on `/dev/cu.SLAB_USBtoUART` |
| 1.8" ST7735 TFT, red V1.1 board | working | VCC on **5V** (its regulator starves the panel on 3V3: white screen); RGB order (pink showed as lilac in BGR) |
| Add-on board: ADXL345 accelerometer | working, 800 Hz | CS to 3V3 (I2C mode) and SDO to GND (address 0x53), or it drops off the bus |
| Add-on board: 4-pad touch module (OUT1-OUT4) | OUT1 working | Hold 2 s: start/stop, tap: next view, double tap: dismiss. OUT2-4 could become real buttons if pins allow |
| Add-on board: gesture and ultrasonic sensors | not used | The ultrasonic's echo is 5V: it needs a divider before any ESP32 pin |

**Where to wear it:** the add-on board on the **back of the hand** you scratch with, flat and
snug, the same way round every time. Scratch vibration is strongest there and fades by the wrist.

## 4. Pin map

| ESP32 pin | Used by | Why this pin |
|---|---|---|
| 23, 18 | TFT SDA, SCK | VSPI hardware SPI |
| 5 | TFT CS | |
| 16 | TFT A0 | not 2 (onboard LED), not 21 (I2C) |
| 4 | TFT RESET | |
| 21, 22 | I2C SDA, SCL: ADXL345 (and MAX30100 if added) | the ESP32's default I2C pins |
| 27 | touch pad OUT1 | |
| 34, 35 | **planned:** lesional temperature, lesion and reference | ADC1, input only |
| 32 | **planned:** GSR | ADC1 |
| 33 | **planned:** skin moisture, if the parts arrive | ADC1 |
| 26, 25, 13 | spare: touch OUT2-4 if wanted | |

Rules behind it: analog sensors go on **ADC1** pins (32-39), because ADC2 stops working while
Bluetooth or Wi-Fi is on. Avoid the boot strapping pins (0, 2, 12, 15) for anything that could
pull them at reset. The old Wi-Fi wristband put moisture on GPIO 4 and 15; GPIO 4 is now the
TFT's RESET, so that plan (in `archive/gsr_wristband_wifi/HARDWARE_GUIDE.md`) no longer applies.

## 5. Sensors: what's coming

### Lesional temperature (next sensor)
Two sensors, one **on the lesion** and one on nearby **healthy skin**. The difference is what
matters: it cancels room and body temperature, and inflamed skin runs warmer.
- **Preferred: two 10 kΩ NTC thermistors**, each in a divider with a 10 kΩ resistor from 3V3, to
  GPIO 34 and 35. Small beads sit flat on skin.
- **Fallback: two LM35s.** They need **5V** on +Vs (at least 4 V; on 3V3 they read wrongly low).
  Output is 10 mV/°C, about 0.33 V at skin temperature, which is safe for the ESP32. Flat side
  facing you, legs down: left +Vs to 5V, middle Vout to the ADC pin, right GND.
- Both: keep the sensor away from the ESP32's own heat, tape it to the skin, and let it settle
  a minute before trusting a reading.
- It would add a `temperatures` reading (`lesion_c`, `reference_c`); the hub and website API
  already accept it.

### GSR (stress)
Skin conductance on GPIO 32, using the archived wristband's code (`archive/gsr_wristband_wifi/main/signals.c`)
and the WESAD stress model (LightGBM, calibrated on the wearer's first calm minutes). GSR is much
stronger on the fingers or palm than the wrist, so finger electrodes are worth it.

### MAX30100 (optional)
Relevant: the stress model was trained on PPG (pulse) as well as GSR, and stress lowers heart rate
variability. But it's motion-sensitive on the hand, and many purple MAX30100 boards pull I2C up to
1.8 V, which the ESP32 reads unreliably (take its pull-ups off and use 4.7 kΩ to 3V3). GSR first.

### Skin moisture (maybe)
A patch on the eczema site and one on healthy skin, on GPIO 33. Eczema skin read lower in all 13
patients of the Southampton e-textile study the website's simulator replays.

## 6. Scratch detection

**Now:** the ADXL345 samples at 800 Hz. Every second the detector scores the last 2 s of motion
and 1 s of vibration:
- motion rhythm: 2.5-8 strokes a second
- vibration: band powers 2-400 Hz, and whether the 80-400 Hz vibration pulses in the stroke rhythm

This follows the ADAM sensor study (Chun et al., *Sci. Adv.* 7, eabf9405, 2021). Scratching makes
a skin vibration that a 100 Hz smartwatch can't see, which is why one mistook waving for
scratching and missed finger-only scratching. So vibration separates them: waving has rhythm but
no vibration; finger-only scratching has vibration but little motion.

**In progress:** training it on labelled recordings. On a still band, the ADXL345's noise at 800
Hz is 14-22 mg RMS, flat across frequencies, well above the hand-set rule's first guess, so the
vibration threshold has to come from real data:
1. `tools/record.py session` records 8 activities twice (nails, fingertips, waving, typing,
   phone, rubbing, marching, rest). Clips stay in `~/DermEdge-recordings`, never in git.
2. `tools/tune.py` trains a small logistic model, tests it on the round it didn't see, and
   writes `main/scratch_model.h` for the band.
3. Report per-activity results, especially false alarms on waving, typing and rubbing.

**Limits to keep in mind:** one wearer's recordings make a personal model, not a validated one.
ADAM tested on held-out people for that reason. Night-time is the easy case, with few lookalike
activities.

## 7. Security and privacy

- **Bluetooth:** LE Secure Connections with a 6-digit passkey shown on the band, bonded; every
  characteristic needs that encrypted link, so others nearby (a school, a hospital) can't read
  the band or send it commands.
- **Website:** everything stays in the browser's own storage; "Delete all my data" removes the
  band's recordings too.
- **Hub:** logins (salted PBKDF2), hashed session and band tokens, every user's data separate.
  Still to do: the hub's older photo and journal endpoints have no login, and band-to-hub Wi-Fi
  would need HTTPS.
- **Not medical advice:** alerts are suggestions ("try a cool compress"), not instructions. Real
  clinical use would mean meeting health-data rules such as HIPAA.

## 8. Roadmap

1. Train and test the scratch model on recordings (in progress).
2. Lesional temperature pair on GPIO 34/35, shown on the band and fed into the flare risk.
3. GSR on GPIO 32 with the stress model; MAX30100 if wanted.
4. The hub's own dashboard page: the same Home tab, band connection and camera fix as the website.
5. Off the breadboard: perfboard, then a PCB.
