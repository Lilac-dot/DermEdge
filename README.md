# DermEdge

**Private eczema screening and flare tracking: a camera, a wearable, and AI that runs on your own device. No cloud; photos never leave the device.**

DermEdge checks a photo of skin in three stages, tracks each body site over time, and combines the result with wearable stress and skin-moisture markers into a daily flare-risk score with tips. It runs on a laptop hub or entirely in the browser.

**Live demo: https://derm-edge.vercel.app** (runs on your device; the first visit downloads the skin model once).

Built by **Team DermEdge** (Tishya Yadlapalli, Nitish GB, Shreeya Jetti) for the **Telemedicon 2026 AI & Digital Health Hackathon**, IIIT Sri City.

| Check | History | Wearable & care |
|---|---|---|
| ![Live check: rash detected, eczema vs look-alikes, severity and heatmap](docs/check.jpg) | ![History: progress by body site and trigger insights](docs/history.jpg) | ![Wearable: stress, heart rate, hydration deficit and a flare alert](docs/wearable.jpg) |

## How it works

```
ESP32-CAM / webcam ─► [1] Normal vs rash ─► [2] Eczema vs 7 look-alikes ─► [3] Severity 0–12
                                                                               │
GSR wristband + moisture patch ─► stress, hydration, night scratching ──► Flare risk + tips
                                                                               │
                                                     Dashboard: history, body sites, doctor report
```

| Part | Model | Runs on | Result |
|---|---|---|---|
| 1. Normal vs rash | CLIP ViT-B/16 + fine-tuned head | Laptop hub | **AUC 0.93** on held-out real phone photos (SCIN, 5-fold CV by case) |
| 2. Eczema vs look-alikes | ResNet18 | Laptop hub | **81.1% accuracy, 82.5% recall** (curated test split, 507 images) |
| 3. Severity | Ridge head on CLIP features, 0–12 EASI-signs score | Laptop hub | Mean error **1.2 points**; band within one step **97.6%** |
| On-device screen | MobileNetV1-0.25 INT8, 300 KB | ESP32-CAM | **97.1%** accuracy on its test set |
| Stress | LightGBM on GSR, skin temp, PPG, motion, calibrated per wearer | Laptop hub | **AUC 0.92–0.94** leave-one-person-out (WESAD) |

Stage 2 tells eczema apart from seven look-alikes: psoriasis, tinea, candidiasis, insect bites, lichen planus, drug eruption and rosacea.

| Stage 1: normal vs rash | Stage 2: eczema vs look-alikes |
|---|---|
| <img src="docs/stage1_confusion_matrix.png" width="400" alt="Stage 1 confusion matrix, SCIN 5-fold CV: 84 of 90 healthy cleared, 1182 of 1444 rashes caught"> | <img src="docs/stage2_confusion_matrix.png" width="400" alt="Stage 2 confusion matrix on the 507-image test split: 204 look-alikes and 207 eczema correct, 52 false eczema, 44 missed eczema"> |
| Real phone photos (SCIN), held out by case: 1182 of 1444 rashes caught, 84 of 90 healthy photos cleared. | Curated test split: of 251 eczema photos it caught 207 and missed 44; of 256 look-alikes it flagged 52 as eczema. |

### Fair across skin tones
At one shared threshold, the rash screen caught 84% of rashes on Fitzpatrick I–IV but only 67% on V–VI, even though it ranked them almost as well (AUC 0.91 vs 0.93). Choosing your skin type applies a calibrated threshold, which brings V–VI to **83%**. Details: `experiments/clip_rash_head/summary.json`.

## Features
- Live camera with a rash / no-rash badge; auto-analyses when a rash is held in view
- Photo quality check: asks for a retake when the photo is blurry, too dark or washed out
- Grad-CAM heatmap of where the eczema model looked
- "Not sure, see a dermatologist" when the models disagree or are uncertain
- Body-site tracking with before/after comparison ("inner elbow 6.1 → 4.3, better")
- Wearable markers: stress, heart rate, skin conductance, skin temperature, hydration deficit, night scratching
- Daily flare risk with alerts and one tip per active trigger
- Skin-type calibration (Fitzpatrick I–VI)
- Treatment log (moisturiser, steroid cream, antihistamine, wet wrap…) shown on the history chart
- Trigger insights: which of your triggers line up with worse check-ins, from your own log
- First-run setup: privacy consent, skin type and the body sites you want to track
- History chart, sample history for demos (clearly labelled), printable doctor report

## Try it in your browser
`web/` is a static version of the dashboard where every model runs on the visitor's own device: CLIP through transformers.js (WebGPU + fp16 where available, otherwise WebAssembly), the eczema-vs-look-alikes ResNet18 through ONNX Runtime Web, and the stress model in plain JavaScript. History is kept in the browser, nothing is uploaded, and the wearable tab uses a simulated wristband. The first visit downloads the skin model once (~175 MB with WebGPU). It's deployed on Vercel from this repo (`vercel.json`); to try it locally, serve `web/` with any static server.

Checked against the Python models: the browser CLIP gives the same stage-1 decision on 98.5% of 200 held-out phone photos, and the ONNX ResNet18 and JavaScript stress model match their Python versions exactly. `scripts/export_web_models.py` rebuilds `web/models/`.

## Run the hub

```bash
pip install -r requirements.txt
python3 app/server.py
```
Open http://localhost:8000 and allow camera access. The first start downloads the CLIP model (~600 MB) from Hugging Face once; after that everything runs offline.

On macOS without Homebrew's `libomp`, LightGBM borrows PyTorch's copy (see `app/wearable.py`); Windows and Linux need nothing extra.

**Sample history:** put a few eczema photos in `app/sample_photos/`, then click *Load sample history* on the History tab. Every sample entry is labelled.

**Wearable simulation:** the *Calm day / Stressful day / Itchy night / Flare* buttons replay real recordings, which can't be redistributed here. To enable them, place the WESAD windows at `dataset/WESAD/wesad_raw_windows.npz` and the Southampton e-textile patient CSVs in `dataset/southampton_moisture/PatientTest/` (sources below). Without them, the buttons are disabled and the hub still accepts live data from the wristband.

## Firmware
`firmware_dermedge/` has two ESP-IDF projects (ESP32-CAM screen and GSR wristband) and a shared WiFi/hub component. Build and flash with the VS Code ESP-IDF extension; see `firmware_dermedge/README.md`. Both projects build with ESP-IDF v5.4.1.

## Webcam-only demo
```bash
python3 scripts/webcam_eczema_demo.py
```

## Retraining
The training scripts in `scripts/` rebuild the product's models from the research datasets (not included):

| Script | Builds |
|---|---|
| `finetune_clip_eczema.py --target rash` | stage-1 head and skin-type thresholds |
| `train_clip_severity.py` | severity head |
| `train_esp32_rash_gate.py --data v1` | ESP32-CAM model and its C array |
| `healthy_gate_probe.py`, `download_scin_healthy.py`, `prepare_megpaulson_healthy.py` | healthy-skin data and the probe that checked it for shortcuts |
| `wesad_wrist_summary.py`, `southampton_moisture_summary.py` | readable tables and calm-vs-stress / eczema-vs-healthy checks of the wearable datasets (see `docs/SENSOR_DATA.md`) |

## Data
- **SCIN** (Google, open access): real smartphone photos, healthy and diseased; used for the rash screen and its fairness check.
- **Human Skin Diseases** (Kaggle, Apache 2.0) and a DermNet-style eczema archive: the curated eczema-vs-look-alikes set.
- **WESAD** (Schmidt et al., 2018): wrist sensor recordings for the stress model.
- **Southampton e-textile AD sensor data** (Todorov 2026, CC-BY, DOI 10.5258/SOTON/D3873): moisture readings on eczema vs healthy skin.
- **MegPaulson/Healthy_Skin** (Hugging Face): extra healthy-skin close-ups, filtered.

What the wearable datasets show, where to download them, and next steps for the wearable models: `docs/SENSOR_DATA.md`.

## Limitations
DermEdge is a screening aid, not a diagnosis. The on-device models and the darker-skin calibration would benefit from more real-world photos, and the firmware is ready to flash but hasn't been tested on boards yet.
