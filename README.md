# DermEdge

**Offline eczema screening and flare tracking: a camera, a wearable, and a laptop hub. No cloud, no internet; photos never leave the device.**

DermEdge checks a photo of skin in three stages, tracks each body site over time, and combines the result with wearable stress and skin-moisture markers into a daily flare-risk score with tips. It is a hackathon prototype and a screening aid, not a diagnosis.

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

### Fair across skin tones
At one shared threshold, the rash screen caught 84% of rashes on Fitzpatrick I–IV but only 67% on V–VI, even though it ranked them almost as well (AUC 0.91 vs 0.93). Choosing your skin type applies a calibrated threshold, which brings V–VI to **83%**. Details: `experiments/clip_rash_head/summary.json`.

## Features
- Live camera with a normal/rash badge; auto-analyses when a rash is held in view
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

## Run the hub

```bash
pip install -r requirements.txt
python3 app/server.py
```
Open http://localhost:8000 and allow camera access. The first start downloads the CLIP model (~600 MB) from Hugging Face once; after that everything runs offline.

On macOS without Homebrew's `libomp`, LightGBM borrows PyTorch's copy (see `app/wearable.py`).

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

## Data
- **SCIN** (Google, open access): real smartphone photos, healthy and diseased; used for the rash screen and its fairness check.
- **Human Skin Diseases** (Kaggle, Apache 2.0) and a DermNet-style eczema archive: the curated eczema-vs-look-alikes set.
- **WESAD** (Schmidt et al., 2018): wrist sensor recordings for the stress model.
- **Southampton e-textile AD sensor data** (Todorov 2026, CC-BY, DOI 10.5258/SOTON/D3873): moisture readings on eczema vs healthy skin.
- **MegPaulson/Healthy_Skin** (Hugging Face): extra healthy-skin close-ups, filtered.

## Limitations
DermEdge is a screening aid, not a diagnosis. The on-device models and the darker-skin calibration would benefit from more real-world photos, and the firmware is ready to flash but hasn't been tested on boards yet.
