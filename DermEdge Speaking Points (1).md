# DermEdge: Speaking Points

Oct 19, 2026 · @Nitish GB

## Overview

DermEdge is "a camera, a wearable, and AI that runs on your own device", screening eczema and predicting flares without any photo leaving the device. It was built by Team DermEdge (Tishya Yadlapalli, Nitish GB, Shreeya Jetti) for the Telemedicon 2026 AI & Digital Health Hackathon at IIIT Sri City.

The time split below follows the rubric weights for a 5-minute slot. The live demo happens inside the prototype section.

| Criterion | Weight | Time |
| --- | --- | --- |
| 1. Problem Identification and Clarity | 15% | 45 s |
| 2. Technical Innovation | 20% | 60 s |
| 3. Clinical Relevance and Validation | 20% | 60 s |
| 4. Social Impact and Sustainability | 15% | 45 s |
| 5. Quality of the Prototype / Proof of Concept | 20% | 60 s |
| 6. Presentation and Communication | 10% | 30 s |

## 1. Problem Identification and Clarity (15%)

The problem is that eczema flares between clinic visits, and neither patient nor doctor sees what triggered them.

- Eczema is a chronic, flare-and-remit condition, and it is hard to tell from look-alikes like psoriasis or tinea.
- A dermatologist sees the skin for minutes, but flares build over days and are tied to stress, dryness and night scratching.
- Skin photos are sensitive, so many people avoid cloud apps. DermEdge answers with "no cloud, photos never leave the device."
- One-line pitch: a camera, a wearable, and AI that runs on your own device, screening eczema and predicting flares privately.

## 2. Technical Innovation (20%)

DermEdge runs a three-stage skin pipeline at three levels of hardware, and fuses it with wearable signals into one flare-risk score.

- **Three stages:** normal vs rash (CLIP ViT-B/16 with a fine-tuned head), eczema vs 7 look-alikes (ResNet18), then severity on a 0–12 EASI-signs scale.
- **Edge AI at three levels:** a 300 KB INT8 MobileNet on an ESP32-CAM; a laptop hub running the full pipeline offline after a one-time model download; and a static browser version using WebGPU and ONNX Runtime Web, so nothing is uploaded.
- **Sensor fusion:** a GSR wristband and moisture patch feed a LightGBM stress model, calibrated per wearer against their own calm baseline. It combines with the image result into a daily flare-risk score.
- **Trust features:** a Grad-CAM heatmap, a photo-quality check that asks for a retake, and a "not sure, see a dermatologist" fallback when the models disagree.
- **Parity check:** the browser model gives the same stage-1 decision as the Python model on 98.5% of 200 held-out photos.

## 3. Clinical Relevance and Validation (20%)

The strongest part of the validation is its design: case-grouped cross-validation on real phone photos, leave-one-person-out testing for the stress model, and an explicit skin-tone fairness check.

| Model | Result | Test setup |
| --- | --- | --- |
| Stage 1: normal vs rash | AUC 0.93; caught 1182 of 1444 rashes, cleared 84 of 90 healthy photos | SCIN phone photos, 5-fold CV grouped by case |
| Stage 2: eczema vs look-alikes | 81.1% accuracy, 82.5% recall | Curated test split, 507 images |
| Stage 3: severity | Mean error 1.2 points on 0–12; band within one step 97.6% | Held-out validation set |
| Stress | AUC 0.92–0.94 | WESAD, leave-one-person-out |

- **Skin-tone fairness:** at one shared threshold the rash screen caught 84% of rashes on Fitzpatrick I–IV but only 67% on V–VI. A skin-type calibration brings V–VI to 83%.
- **Leakage check:** a probe checks for source shortcuts, because an earlier "Normal" class could be recognized by its source rather than by the skin.
- **Clinician in the loop:** DermEdge is a screening aid, not a diagnosis. The printable doctor report and the "see a dermatologist" fallback keep a clinician involved.

## 4. Social Impact and Sustainability (15%)

DermEdge keeps cost and risk low for patients by running on their own hardware and keeping photos on the device.

- **Access:** the screen runs on a low-cost ESP32-CAM with a 300 KB model, and the full dashboard runs on a laptop or in a browser.
- **Privacy and offline use:** after a one-time model download, everything runs without a connection. This suits clinics with weak connectivity and patients wary of uploading skin photos.
- **Equity:** skin-tone calibration addresses the known under-representation of darker skin in dermatology AI.
- **Sustainability:** it is built on open or CC-BY datasets (SCIN, WESAD, Southampton), the code is open, and the browser version is deployed on Vercel. Inference runs on the user's device, so there is no recurring cloud inference cost.
- **Long-term value:** treatment logs and trigger insights (which of your triggers line up with worse check-ins) help patients and doctors manage a chronic condition.

## 5. Quality of the Prototype / Proof of Concept (20%)

The prototype is a full stack in one repo, with a live demo at [derm-edge.vercel.app](https://derm-edge.vercel.app).

- **Demo path:** open the Check tab (rash badge, heatmap, severity), then History (before/after by body site), then the Wearable tab (flare alert). The screenshots in `docs/` are the backup if the connection fails.
- **What is in the repo:** the web app, the Python hub, training scripts, ESP-IDF firmware for the camera and the wristband, and the exported on-device model. Both firmware projects build on ESP-IDF v5.4.1 (wristband 866 KB, camera 1.4 MB).
- **Reproducibility:** retraining scripts and the exported web models are included, so the numbers can be rebuilt from the public datasets.
- **Say this before a judge asks:** the wristband is simulated by default, and the firmware has not been flashed to real boards yet.

## 6. Presentation and Communication (10%)

Open with the problem in one sentence and have the demo on screen within the first 90 seconds.

- Keep one diagram on screen: camera and wristband, then the 3 stages, then flare risk, then the doctor report.
- Name the limitation yourselves, then give the next steps: real-board testing, more darker-skin photos, and a clinician pilot.

## Questions to prepare for

Four questions are likely, and each has an honest answer.

| Likely question | Honest answer |
| --- | --- |
| "97.1% on the ESP32 model seems high." | That test split is the original Eczema-vs-Normal set, and the repo notes the old "Normal" class had source shortcuts. Present it as proof the model fits on a 300 KB chip, and cite the SCIN result (AUC 0.93) as the real clinical number. |
| "How good is the severity score?" | The exact band is right 51.6% of the time, against 39% by chance, with Spearman 0.65. Call it a trend tracker; the within-one-band figure (97.6%) is the one that matters. |
| "Is the flare-risk score validated?" | The stress model is validated on WESAD, but WESAD is stress data, not eczema flares. No clinical validation of the combined flare score is in the repo, so call it a prototype hypothesis that needs a patient study. |
| "How solid is the darker-skin result?" | The V–VI group had 114 rash photos and 16 healthy ones, and the calibration offset was fitted on the same data. Present it as promising and in need of more data. |

## Sources

All figures come from the [DermEdge repo](https://github.com/Lilac-dot/DermEdge), read on 19 October 2026:

- `README.md`: pipeline, model results, fairness check, limitations
- `firmware_dermedge/README.md`: firmware status and build details
- `experiments/clip_rash_head/summary.json`: stage 1 and Fitzpatrick results
- `experiments/clip_severity_head/summary.json`: severity results
- `experiments/esp32_eczema_v1_mnv1_025/summary.json`: ESP32 model results
- `scripts/healthy_gate_probe.py`: shortcut-leakage probe
