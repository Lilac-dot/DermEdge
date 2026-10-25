> **Archived (2026-10-25):** written for the Wi-Fi GSR wristband in this folder. The DermEdge band
> replaced it; for the current hardware, pins and sensor plan see `docs/HARDWARE_PLAN.md`. Its
> moisture pins (GPIO 4 and 15) clash with the band, where GPIO 4 is the TFT's RESET.

# DermEdge wristband: hardware guide

Everything the hardware side needs in one place: what the wristband feeds into, what is real
vs simulated today, how to add the LM35 skin-temperature sensor to the breadboard, how to
record scratching, and how to test against the hub. Build/flash basics are in `README.md`
next to this file.

---

## 1. Where the wristband fits

There is **one** pipeline, with or without the wristband. The photo always goes through the
same three image stages; the wristband only adds *triggers* to the flare-risk score.

```
photo ─► CLIP ─► Stage 1 normal/rash ─► Stage 2 eczema/look-alikes ─► Stage 3 severity ─► image_score
                                                                                            │
wristband ─► stress (GSR + temp + PPG + motion) ─┐                                          ▼
             dry skin (moisture patch)           ├─► noisy-OR ─► trigger_index ─► flare risk = image_score × trigger_index
             night scratching (IMU)              │
journal ─► stress, sleep, new product ───────────┘
```

- No wristband connected → its triggers are simply absent; the app still works on photo + journal.
- No rash in the photo → flare risk is 0 no matter what the wristband says.
- The **Wearable tab** shows trigger alerts on its own, without a photo.
- Code: `app/pipeline.py` (`trigger_scores`, `flare_risk`), `app/wearable.py` (`Wearable.triggers`).

## 2. What the hub expects from the board

The stress model was trained on the WESAD dataset (Empatica E4 wristband), so the board
samples at the same rates and units and POSTs a raw window every 30 s:

| Signal | Rate | Unit | Hub endpoint |
|---|---|---|---|
| GSR / EDA (skin conductance) | 4 Hz | microsiemens | `POST /api/wearable/window` `{eda, temp, bvp, acc}` |
| Skin temperature | 4 Hz | °C | same window |
| PPG / BVP (pulse wave) | 64 Hz | unitless | same window |
| Accelerometer | 32 Hz × 3 axes | 1/64 g | same window |
| Moisture patch | every window | capacitance-like reading | `POST /api/wearable/moisture` `{patch: [...], reference: [...]}` |
| Scratch bursts | when detected | count | `POST /api/wearable/scratch` `{count}` |

**Start calm.** The hub uses the first 3 windows (90 s) to calibrate the stress model to the
wearer, so what matters is each signal's *change* from that baseline, not its exact value.

## 3. Status of each sensor

| Sensor | Today | Pin | Real data behind it |
|---|---|---|---|
| GSR | Simulated, or Grove GSR module | GPIO34 (ADC1 ch6) | WESAD |
| Skin temperature | Simulated, or **LM35 (new, see §4)** | GPIO35 (ADC1 ch7) | WESAD |
| Moisture patch | Simulated, or ESP32 touch pins as crude electrodes | GPIO4 (eczema area), GPIO15 (healthy skin) | Southampton e-textile study |
| PPG / pulse | Simulated | – | WESAD |
| Accelerometer | Simulated (no IMU wired) | – | WESAD |
| Scratching | Simulated (no public dataset) | – | none yet: see §6 |

Pick each source in VS Code → SDK Configuration Editor → **DermEdge wristband**. Everything
defaults to simulated, and with simulated GSR the **BOOT button toggles calm/stressed** for demos.

**Next sensor worth adding:** a pulse sensor. The
[bme-biosensors-wearables](https://github.com/esha-ahmed/bme-biosensors-wearables) repo's
section 2 uses a Pulse Sensor Amped (analog, easy) or a MAX30101 (I2C, better). That would make
PPG and heart rate real. Its ECG and respiration builds aren't needed: the model only uses
wrist signals.

## 4. Adding the LM35 to the breadboard

### Parts
- LM35 temperature sensor (TO-92 package, three legs)
- 3 jumper wires, long enough that the LM35 can sit on the wrist **away from the ESP32**
  (the board's own heat skews the reading)
- Heat-shrink or tape for the joints, plus an elastic band or tape to hold it on the skin

### Wiring
Hold the LM35 with the **flat side facing you**, legs down:

```
LM35 left   (+Vs)  ──► ESP32 5V / VIN     ← NOT 3V3: the LM35 needs at least 4 V
LM35 middle (Vout) ──► ESP32 GPIO35
LM35 right  (GND)  ──► ESP32 GND
```

- 5V/VIN is live whenever the board is on USB.
- On 5V the output is still only ~300–350 mV at skin temperature (10 mV per °C), so it is safe
  for the ESP32's 3.3 V input.
- GPIO35 is an ADC1 pin. Don't move it to an ADC2 pin: ADC2 stops working while WiFi is on.
  It must also differ from the GSR pin (GPIO34).

### Turning it on in the firmware
1. Open `firmware_dermedge/wristband` in VS Code (the project folder itself, not the parent).
2. SDK Configuration Editor → **DermEdge wristband → Skin temperature source → LM35 on an ADC pin**.
   The channel defaults to 7 (= GPIO35).
3. Build, Flash, Monitor. The start-up log prints the first reading:
   - `LM35 reads 32.4 C` → working.
   - `LM35 reads 4.1 C -- check the wiring ...` → usually +Vs is on 3V3 instead of 5V, or a
     wire is in the wrong hole.

The firmware averages 32 ADC reads per sample because a single ESP32 reading is noisy by about
as much as the ~1 °C calm→stress shift the model looks for. Code: `wristband/main/signals.c`
(`signals_temp`).

> ⚠️ The LM35 firmware change hasn't been compiled yet: ESP-IDF wasn't installed on the
> laptop it was written on. If the first build errors, send the error output over.

### Before trusting it
- **Warm-up:** leave it on the wrist 2–3 minutes before starting. The TO-92 package heats slowly,
  and the first 90 s are used for calibration.
- **Sanity check:** hold a normal thermometer next to it once. Wrist skin should read about
  **30–35 °C** (that's where the WESAD participants sit).
- **Accuracy:** ±0.5 °C sensor + ADC noise. Fine, because the model works on changes from the
  wearer's own baseline.
- **If buying new parts instead:** the **MAX30205** is made for body temperature (±0.1 °C),
  runs on 3.3 V and is digital (I2C), which avoids the 5 V and ADC-noise issues. It would need a
  small firmware addition.

## 5. Real datasets behind the sensors (for reference and the pitch)

Both are downloaded on the laptop under `DermEdge/dataset/` (git-ignored, not in the repo).

### WESAD (stress + skin temperature): `dataset/WESAD/`
15 healthy adults (12 male, 3 female, aged 24–35) wearing an Empatica E4 wristband through a
calm baseline and a stress task (public speaking + mental arithmetic). Summary in
`wesad_wrist_summary.csv`; raw wristband CSVs in `subjects/S*/wrist_E4/`.

| Under stress | People |
|---|---|
| Skin conductance goes **up** (e.g. 5.1 → 14.0 µS) | 14 / 15 |
| Heart rate goes **up** (e.g. 73 → 120 bpm) | 14 / 15 |
| Skin temperature goes slightly **down** | 12 / 15 |

So the LM35 should show a small drop under stress, not a rise.

Gotcha if you slice these CSVs yourself: the phase times in `S*_quest.csv` count from the
*chest* strap's start, and the wristband started 15–24 min earlier. Shift by each person's
offset (`wrist_lead_min` in the summary) or the phases land in the wrong place.

### Southampton e-textile study (skin moisture): `dataset/southampton_moisture/`
13 eczema patients, a textile capacitive sensor on eczema skin vs healthy skin, plus a
Corneometer (the clinical hydration standard). Summary in `moisture_patient_summary.csv`.

| Measurement | Result | Patients |
|---|---|---|
| E-textile sensor lower on eczema skin (by 1.7–10.2 %) | ✔ | 13 / 13 |
| Corneometer lower on eczema skin (by 12–55 %) | ✔ | 13 / 13 |
| Water loss (TEWL) higher on eczema skin | ✔ | 7 / 7 |

How big the sensor's drop is only loosely tracks the Corneometer (r = 0.34). So say it
*reliably detects that eczema skin is drier*, not that it measures dryness as precisely as
clinical tools. The touch-pin patch on our board is a crude stand-in for this sensor.

The dataset zip also contains a `SkinSensorGUI.exe`. It isn't needed, so don't run it.

## 6. Recording scratching from a volunteer with eczema

No public scratch dataset exists, so a real recording from a friend with eczema beats
simulation. The wristband has no IMU yet, so:

1. **Recorder:** a phone strapped to the wrist (or a smartwatch) running a free
   accelerometer-logging app such as **phyphox**, which exports CSV.
2. **Labels:** film the session, or have someone note scratch start/stop times. Without labels
   you can't tell scratching from other movement.
3. **Negatives:** also record movements that look like scratching but aren't: typing, brushing
   teeth, waving, stirring. Otherwise the detector flags every hand movement.
4. **Consent and privacy:** get your friend's written OK, keep their name out of file names,
   and store recordings under `DermEdge/dataset/` (git-ignored).
5. **Framing for judges:** "a pilot recording from one volunteer with eczema". It's a strong
   demo, but one person can't support an accuracy number.

Once there's a recording, it can become a simple scratch detector that posts to the existing
`/api/wearable/scratch` endpoint.

## 7. Testing the board against the hub (Windows laptop)

1. Put the laptop and board on the same network, e.g. **Windows Mobile Hotspot**. The laptop
   is then usually `192.168.137.1`; check with `ipconfig`.
2. In menuconfig → **DermEdge network**: WiFi name/password and hub URL
   `http://<laptop IP>:8000`. The default `192.168.2.1` is for a macOS hotspot; change it.
3. Start the hub: `python app/server.py`, then open http://localhost:8000 → Wearable tab.
   Allow Python through the Windows firewall when asked, or the board can't reach port 8000.
4. Watch the board's monitor: each window logs an HTTP status (200 = received).

Laptop setup notes:
- The macOS-only `libomp.dylib` line that crashed the hub on Windows is fixed.
- This laptop has Python 3.13, but `requirements.txt` pins numpy 2.0.2 and scipy 1.13.1,
  which have no Python 3.13 Windows builds. Use a **Python 3.12** virtualenv for the hub
  (recommended), or raise those pins to numpy 2.1.3 / scipy 1.14.1.

## 8. One-paragraph answer for judges

> Each sensor model is trained and validated on real public data: WESAD for stress (AUC
> 0.92–0.94 on people the model never saw) and the Southampton e-textile study for skin
> moisture (drier on eczema skin in 13 of 13 patients). No dataset pairs wearable signals with
> skin photos from the same patients, so we combine them with a transparent rule, the way
> clinical scores like SCORAD and EASI do. Scratch detection is a pilot recording and is labelled
> as such. Validating the combined score in a real patient pilot is our next step.
