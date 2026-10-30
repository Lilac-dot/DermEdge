# DermEdge app guide: what the app needs to do

For the teammates building the DermEdge app. It covers what the band and the hub already do,
the features the app has to provide, and the exact Bluetooth and API contracts. Kept in step with
the code: `firmware_dermedge/band/` (the band) and `app/band.py` (the hub API).

## How the pieces fit

```
DermEdge band (ESP32)  ──Bluetooth LE, encrypted──►  App / website  ──HTTP + login──►  Hub (laptop)
 ADXL345 scratch detection                           pairs the band                     app/server.py
 touch pad start/stop                                relays readings                    SQLite: users, bands,
 TFT status + alerts                                 shows history + alerts             sessions, readings, alerts
```

- **A working reference:** the website already does all of this in `web/js/band.js` (Web
  Bluetooth, pairing, relaying, alerts both ways) on its Home tab. In the browser version,
  `web/js/local-api.js` answers the same `/api/band/*` calls and keeps everything in the
  browser's IndexedDB; on the hub, `app/band.py` answers them behind a login.
- **The band never talks to the hub directly** (for now). The app relays: it reads the band over
  Bluetooth and posts to the hub as the logged-in user.
- **Data stays local.** The hub runs on the user's laptop; there is no cloud. Every band endpoint
  needs a login, so others on a shared school or hospital network can't read anyone's data.

## Features the app needs

### 1. Account
- Register and log in: `POST /api/auth/register`, `POST /api/auth/login` with
  `{username, password}` (username 3-40 letters/digits/`_.-`, password 8+ characters).
- The hub sets an HttpOnly cookie `dermedge_session` (14 days) and also returns `token` for
  `Authorization: Bearer <token>` in native apps. Never store the password.
- `GET /api/auth/me` to check the login; `POST /api/auth/logout`.

### 2. Pair a band
1. Scan for the band: name `DermEdge`, service `a3c87510-8ed3-4bdf-8a39-a01bebede295`.
2. Connect and read the **status** characteristic. The band asks for pairing straight away; the
   phone or laptop shows a system pairing prompt and the band shows a **6-digit code** on its
   screen. The user types that code. (This is Bluetooth LE Secure Connections with a passkey; every
   characteristic refuses to work without it.)
3. Read `id` from the status (e.g. `DE-004B122E3672`) and register it:
   `POST /api/band/devices {"device_id": "<id>", "name": "My band"}` → `device_token`.
4. Write `token:<device_token>` to the **command** characteristic (the band keeps it for posting
   over Wi-Fi later).
- Unpair: `DELETE /api/band/devices/{id}` and write `unpair` to the band.

### 3. Start and stop recording
- From the app: write `start` (or `start:sleep`, `start:day`, ...) and `stop` to **command**.
- From the band: the user holds the touch pad for 2 s. The app sees `"s":"recording"` with
  `"src":"touch"` in the status and must start a hub session too.
- Hub side: `POST /api/band/sessions/start {"device_id", "label", "started_by": "website"|"touch"}`
  → `{id, ...}`. Calling it again while a session is open returns the same one, so a band tap and
  an app tap at the same moment don't make two. Keep a map from the band's recording number
  `r` to the hub session `id`.
- When recording stops, wait until the band's queue `q` reaches 0 (everything relayed), then
  `POST /api/band/sessions/{id}/stop`.

### 4. Relay readings
- Subscribe to the **data** characteristic. Each message is one reading, already in the hub's
  shape, plus `r`:
  ```
  {"r":3,"motion":[{"t":12.0,"activity":0.1031,"scratch_energy":0.0210}]}
  {"r":3,"scratch_events":[{"t":40.2,"duration_s":6.0,"intensity":0.2580}]}
  ```
- `t` is seconds since the band started that recording. Batch a few, drop `r`, and
  `POST /api/band/sessions/{id}/readings`. For accurate times when the hub session started later
  than the band (offline recording), send `ts = now - (status.el - t)` instead of `t`.
- The band keeps readings in its **flash** while disconnected: about **35 hours** of recording,
  and they survive a restart or a flat battery. It sends them, oldest first, when the link comes
  back (status `"s":"syncing"`, `q` counting down; a long backlog takes a while over Bluetooth).
  Recording numbers `r` keep counting across restarts, so they stay unique. When the flash fills,
  the band keeps the oldest data and thins the new: from 90% full, motion summaries every 10 s
  only; from 98%, scratch episodes only.
- **Sensor readings** (changed 31 Oct: **ECG replaces GSR**, an **LM35** replaces the thermistor,
  moisture is dropped). Same `r` and `t` as above. What the band's firmware sends:
  ```
  {"r":3,"hrv":[{"t":30.0,"hr":71.8,"rmssd":42.5,"sdnn":51.0,"beats":36}]}
                    one per 30 s of ECG electrode contact with the hand still; t = window start
  {"r":3,"skin_temp":[{"t":12.0,"c":32.4}]}                   every 5 s, a 5 s average, 10-50 C
  ```
  `hr` is beats per minute; `rmssd` and `sdnn` are heart rate variability in ms (lower under
  stress); `beats` is how many clean beat intervals the window used. The band sends an `hrv`
  reading only when the window is usable (still hand, at least 15 clean beats, under 20%
  artifacts) and skin temperature only once the LM35 reads steadily, so a gap means "no data",
  never zeros. The electrodes go on both wrists for a seated 1-2 minute check, so expect a few
  `hrv` readings per check, not a continuous stream.
- **How the website uses them** (`web/js/local-api.js`): the first 3 `hrv` readings of a
  recording are the wearer's calm level; later ones are shown against it ("+12 bpm vs your calm
  level"). **Stress is scored once a model exists** at `web/models/stress_ecg.json`, in the same
  format as `stress_gsr.json` (`features`, `mean`, `scale`, `coef`, `intercept`), with feature names
  `<name>_rel` taken relative to the calm level from `HR_mean`, `RMSSD`, `SDNN` and `TEMP_mean`.
  When it's there, the trigger "stress (band ECG)" joins the flare index; until then heart rate
  and HRV are shown but not scored. The website still accepts `gsr`, `moisture` and
  `temperatures` from older firmware.
- The hub (`app/band.py`) doesn't store `hrv` or `skin_temp` yet; the browser version does.

### 5. Flare alerts
- A readings upload can return `"alert": {id, level, risk, reasons, tips}`; the hub raises at most
  one every 30 minutes, only when the flare index is high.
- Show it in the app **and** send it to the band:
  `alert:<level>|<first reason>|<first tip>` (keep it under ~180 characters).
- The user can dismiss on the band (double tap: status `alert` goes from 1 to 0) or in the app
  (write `dismiss` to the band). Either way, `POST /api/band/alerts/{id}/dismiss`.
- `GET /api/band/alerts` lists them for an alert history.
- **Wording:** suggestions, not medical orders. For example, "Consider your usual treatment
  plan" or "Try a short breathing break", not "Take your medication".

### 6. Live view
The status arrives about once a second. Show the state, the timer, a movement bar and the scratch
count:

| Field | Meaning |
|---|---|
| `id` | the band's id, for pairing |
| `s` | `boot`, `pairing`, `ready`, `recording`, `syncing`, `error` (accelerometer missing) |
| `r` | recording number since the band booted |
| `src`, `label` | who started it (`touch` or `website`) and its label |
| `el` | seconds recorded so far |
| `n` | scratch episodes this recording |
| `act` | movement in the last second, g (about 0-0.6) |
| `q` | readings waiting to be sent |
| `alert` | 1 while an alert shows on the band |
| `ecg` | 1 while the ECG electrodes are on |
| `hr` | live heart rate in bpm, `null` until a few clean beats are found |
| `tc` | live skin temperature in °C, `null` while unplugged or settling |
| `sim` | 1 while the band's heart and temperature readings are **simulated** (testing without sensors): label them so |

### 7. History
- `GET /api/band/sessions`: every session with `duration_s` and `scratch_count`.
- `GET /api/band/sessions/{id}`: the readings, for charts (a movement timeline with the scratch
  episodes marked).
- `POST /api/band/sessions/{id}/raw {label, fs, samples: [[x,y,z], ...]}`: opt-in 100 Hz clips
  labelled `scratching`, `typing`, `walking` or `resting`, used to tune the detector. Only on the
  user's request.

## Bluetooth reference

Service `a3c87510-8ed3-4bdf-8a39-a01bebede295`. All values are UTF-8 text; all need the paired,
encrypted link.

| UUID | Name | Access | |
|---|---|---|---|
| `a3c87511-…` | status | read, notify (~1 Hz) | JSON, fields above |
| `a3c87512-…` | data | notify | one reading per message |
| `a3c87513-…` | command | write | `start[:label]`, `stop`, `dismiss`, `alert:<level>\|<reason>\|<tip>`, `token:<device token>`, `unpair` |

Platform notes: Web Bluetooth works in **Chrome and Edge** (not Safari or Firefox, so not on
iPhone browsers). A native app can use CoreBluetooth (iOS) or the Android BLE API; both handle the
passkey prompt. Ask for an MTU of 247 so longer messages fit.

## Hub API reference

Base URL: `http://<laptop IP>:8000`. Interactive docs with "try it" buttons:
`http://localhost:8000/docs`. Errors are `{"detail": "..."}` with 401 (log in), 403 (a band token
can't do this), 404 (not yours or doesn't exist), 409 (username taken, band paired elsewhere, or
session already stopped), 422 (bad input).

| Endpoint | Body | Returns |
|---|---|---|
| `POST /api/auth/register`, `/login` | `{username, password}` | `{id, username, token}` + cookie |
| `POST /api/auth/logout`, `GET /api/auth/me` | | |
| `POST /api/band/devices` | `{device_id, name}` | `{id, device_id, name, device_token}` |
| `GET /api/band/devices`, `DELETE /api/band/devices/{id}` | | |
| `POST /api/band/sessions/start` | `{device_id, label, started_by, notes}` | session |
| `POST /api/band/sessions/{id}/stop` | | session |
| `GET /api/band/sessions`, `GET /api/band/sessions/{id}` | | sessions / session with readings |
| `POST /api/band/sessions/{id}/readings` | `{scratch_events, motion, hrv, skin_temp}` (also `gsr`, `moisture`, `temperatures` from older firmware) | `{stored, alert}` |
| `POST /api/band/sessions/{id}/raw` | `{label, fs, samples}` | `{id, n}` |
| `GET /api/band/alerts`, `POST /api/band/alerts/{id}/dismiss` | | |

## Design

Match the website (`web/index.html`): titles in **Young Serif**, text in **Figtree** (both in
`web/static/fonts`, SIL Open Font License). Light theme: background `#F4F8F7`, cards `#FFFFFF`,
teal accent `#0F7C74` (white text on it), soft accent `#DCEFEC`, headings `#134E4A`, text
`#1D2B2A`, muted `#5C6F6C`, lines `#D9E6E3`, alerts `#C92F4C`, good `#3A7D1F`. Keep pink and red
for alerts only, since the skin photos themselves are pink. The full light and dark token sets
are at the top of `web/index.html`; the site is always light, and the dark set applies only with
`data-theme="dark"` on `<html>` (no switch for it yet).
The band's screen still uses the old pink palette (rose `#D84A78`, pink card `#F6B3C6`, deep
green `#1F551A`) and is due to move to a deeper teal, since its panel washes pastels out; its
screens are in `firmware_dermedge/band/test/preview.png`.

## Known limits and what's coming

- **Scratch detection is motion only, at 100 Hz.** It finds strong, rhythmic 2.5-8 Hz motion and
  ignores walking, typing and jolts in tests, but the thresholds aren't tuned on real recordings
  yet. Research (the ADAM sensor study: Chun et al., *Sci. Adv.* 7, eabf9405, 2021) shows the clearest scratch
  signal is a 100-800 Hz skin vibration, which this misses, including finger-only scratching.
  Planned: sample faster (the ADXL345 goes to 3200 Hz) and move the sensor to the back of the hand.
- **Stress from the ECG needs its model:** the band and website handle heart rate and HRV now
  (31 Oct); `web/models/stress_ecg.json` still has to be trained (WESAD chest ECG). Every sensor,
  with its checks and limits: [SENSORS.md](SENSORS.md); decisions:
  [HARDWARE_PLAN.md](HARDWARE_PLAN.md).
- **Security:** fine for a prototype. Wi-Fi between the band and the hub is plain HTTP today
  (HTTPS planned), and the hub's older photo and journal endpoints don't have the login yet.
  Real clinical use would mean meeting health-data rules such as HIPAA.
- **Decisions for the team:** a web app (Web Bluetooth, Chrome only) or a native app; and whether
  the hub stays on a laptop or moves to a server, which brings back the privacy questions.
