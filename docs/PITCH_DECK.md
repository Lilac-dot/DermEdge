# DermEdge pitch deck: slide text for the Canva rebuild

**The deck in Canva:** https://canva.link/sjz2ti8lsuddh4j (the team's working copy; this file has its text).

Drafted 26 Oct 2026 from the current plan ([HARDWARE_PLAN.md](HARDWARE_PLAN.md) decisions log),
replacing the older `DermEdge Pitch (with diagrams).pptx`. Each slide lists the headings and text
to paste in, and the diagram to place. Diagrams are in `docs/architecture/` (3200 × 1800 PNG);
`python scripts/draw_architecture.py` redraws slides 5, 9 and 10's.

**What changed from the old deck**
- Photos come from the **phone or laptop camera** (website or hub). The **ESP32-CAM moves to
  future scope**; its slide is replaced by the DermEdge band.
- The **"GSR wristband"** is now the **DermEdge band**: an ESP32 on the back of the hand that
  detects scratching with a model trained on our recordings, over encrypted Bluetooth.
- **Skin temperature is a stress input only** (one thermistor), not an eczema signal.
- **Stress numbers:** quote the band's GSR model (90% accuracy, 73% of stress caught), not the old
  four-signal LightGBM's AUC 0.92-0.94. That model turned out to call 50% of meditation and
  amusement windows "stress": it had partly learned "later in the session"
  (`scripts/train_gsr_stress.py`).
- New slides: the band, data and validation, and future scope.

Footer for every slide: **Telemedicon 2026 · Finale · MGUMST Jaipur** (check the organisers'
exact name for the round).

---

## 1. Title

- TELEMEDICON 2026 · AI & DIGITAL HEALTH HACKATHON
- Finale · MGUMST Jaipur · 31 Oct - 1 Nov 2026
- **DermEdge**
- Private AI eczema screening and flare tracking, on your own device
- Team DermEdge: Tishya Yadlapalli, Nitish GB, Shreeya Jetti

## 2. Problem statement & validation

**THE PROBLEM**
Eczema flares between doctor visits, is easily confused with look-alikes (psoriasis, fungal
infections) that need different treatment, and most skin-AI tools upload photos and work worse on
darker skin.

**WHO IS AFFECTED?**
Children and adults with eczema, their families, and dermatologists with only minutes per visit,
especially where skin specialists are few.

**VALIDATION / EVIDENCE**
- Highest burden of any skin disease; 15th of all non-fatal diseases (GBD 2017, Laughter et al.)
- About 204 million people affected worldwide (BJD global meta-analysis)
- 8.5% of school children in a Central India survey (Agarwal et al., EJPD)
- Dermatology atlases hold 3.6× more lightest- than darkest-skin images (Groh et al., Fitzpatrick17k)

**CURRENT ALTERNATIVES & GAPS**
Clinic visits, paper diaries and cloud symptom-checker apps. Gaps: nothing tracks the days
between visits, no trigger data, photos uploaded to the cloud, skin-tone bias.

## 3. Proposed solution & innovation

**SOLUTION OVERVIEW**
DermEdge pairs any phone or laptop camera with the DermEdge band, a wearable on the back of the
hand. All the AI runs on the user's own device: it checks skin for a rash, tells eczema from
look-alikes, scores severity, and turns scratching, stress and dry skin into a daily flare risk.

**KEY FEATURES**
- Live rash check with a heatmap of where the model looked
- Eczema vs 7 look-alikes, and severity on a 0-12 scale
- The DermEdge band: detects scratching, alerts on its own screen
- Body-site progress, treatment log, trigger insights, doctor report

**HOW IT WORKS**
Photo → quality check → normal vs rash → eczema vs look-alikes → severity.
Band and journal → scratching, stress, dry skin → trigger index.
Image score × trigger index → flare risk + tips → dashboard, band alert, doctor report.

**INNOVATION & TECHNOLOGY**
- Skin-tone calibration: darker-skin rash detection from 67% to 83%
- Runs in the browser, offline after one download, as an installable app: no photo leaves the device
- A scratch detector trained on our own recordings, running on the band's ESP32
- CLIP, ResNet18 and a calibrated stress model, fused into one flare score

## 4. Technical methodology

**1 INPUT**
- Skin photo (phone or laptop camera)
- DermEdge band: accelerometer (scratching); GSR and a skin-temperature thermistor (stress, being added)
- Moisture patch (planned), journal, skin type

**2 PROCESSING**
- Blur and lighting quality gate
- CLIP rash screen → ResNet18 → severity
- Scratch model on the band; stress model calibrated to the wearer's own calm baseline
- Noisy-OR trigger index × image score

**3 OUTPUT**
- Verdict, severity, heatmap
- Flare risk + one tip per trigger; high-risk alerts on the band
- History and printable doctor report

**TECHNICAL DETAILS**
Built: a static web app (WebGPU / ONNX Runtime Web, installable, offline), a FastAPI laptop hub
with logins, and ESP-IDF firmware for the band (Bluetooth LE, passkey-paired). Data: SCIN,
a curated eczema / look-alike set, WESAD, the Southampton moisture study, and
83 scratch recordings from 6 volunteers. Results: rash AUC 0.93, eczema recall 82.5%, severity
within one band 97.6%, stress 90% accuracy (leave-one-person-out). Privacy: on-device only,
consent at setup, delete-all.

## 5. System architecture

Diagram: `docs/architecture/0_full_system.png`

Optional caption: Everything runs on the user's device. The image path and the wearable path meet
in one flare-risk score; the ESP32-CAM (dashed) is future scope.

## 6. Stage 1: rash screening (CLIP)

Diagram: `docs/architecture/1_stage1_clip_rash.png` (unchanged)

Optional caption: One frozen CLIP pass, two uses: a skin check and a trained rash head. AUC 0.93 on
held-out real phone photos.

## 7. Stage 2: eczema vs look-alikes (ResNet18 + Grad-CAM)

Diagram: `docs/architecture/2_stage2_resnet_gradcam.png` (unchanged)

Optional caption: 81.1% accuracy, 82.5% eczema recall on 507 test images; the heatmap shows where it
looked.

## 8. Stage 3: severity scoring

Diagram: `docs/architecture/3_stage3_severity.png` (unchanged)

Optional caption: Reuses the CLIP embedding. Mean error 1.2 points out of 12; within one band
97.6% of the time.

## 9. Stage 4: wearable triggers & flare risk

Diagram: `docs/architecture/4_wearable_flare_risk.png` (redrawn)

Optional caption: Stress and scratching are learned from data; the fusion is a stated rule,
because no dataset pairs photos with wearables.

## 10. The DermEdge band

Diagram: `docs/architecture/6_band_scratch.png` (new; replaces the ESP32-CAM slide)

Optional caption: An ESP32 on the back of the hand: scratch detection on the device, an hour of
offline buffering, encrypted Bluetooth, and alerts on its own screen.

## 11. Data, validation & privacy

**HOW WE TESTED**

| Model | Result | Test |
|---|---|---|
| Normal vs rash | AUC 0.93; 1,182 of 1,444 rashes caught, 84 of 90 healthy cleared | SCIN phone photos, 5-fold CV grouped by case |
| Eczema vs look-alikes | 81.1% accuracy, 82.5% recall | 507-image test split |
| Severity | Mean error 1.2 / 12; within one band 97.6% | Held-out validation set |
| Stress (band GSR) | 90% accuracy; 73% of stress caught | WESAD, leave-one-person-out, 15 people |
| Scratching | 4 of 4 scratch clips, 0 false episodes in 12 others | One wearer, held-out round |
| Moisture | Eczema skin read lower in 13 of 13 patients | Southampton e-textile study |

**FAIRNESS**
At one threshold the rash screen caught 84% of rashes on Fitzpatrick I-IV but 67% on V-VI;
skin-type calibration brings V-VI to 83%.

**HONEST DATA**
Real public datasets for every model. We augment them with our own sensors' measured noise, test
only on real people the model never saw, and never synthesise paired data for real patients.

**NEXT TESTS (BEFORE THE FINALE)**
- Scratch model tested person by person on 83 clips from 6 volunteers
- A one-day pilot with a volunteer with eczema: an N = 1 feasibility test on real eczema skin

**PRIVACY**
Photos and health data never leave the device. Encrypted, passkey-paired Bluetooth. Consent at
setup, delete-all at any time.

## 12. Prototype / proof of concept

Screenshots: the Check tab, History, and the Home tab with the band connected (retake them in the
new teal theme).

**CURRENT STATUS**
Working prototype: website live, band running on a breadboard.

**WHAT WORKS TODAY?**
- Website: live 3-stage check with heatmap, skin-type calibration, body-site tracking, treatment
  log, insights, doctor report, privacy section; installable and offline
- DermEdge band: scratch detection at 800 Hz, touch start / stop, Bluetooth pairing with the
  website, an hour of offline buffering, alerts on its screen
- Laptop hub with logins, storing band recordings and alerts
- Simulated wearable days (clearly labelled) to demo how triggers drive flare risk

**DEMO / REPOSITORY LINK**
derm-edge.vercel.app · github.com/Lilac-dot/DermEdge

## 13. Impact, sustainability & next steps

**HEALTHCARE & SOCIAL IMPACT**
Earlier, fairer eczema checks at home for every skin tone, and doctor visits informed by real
data from the days in between.

**MEASURING SUCCESS**
Rash AUC of at least 0.9 on every skin type; agreement with dermatologists' severity scores;
scratch detection on people it never trained on; fewer severe flares per user.

**SUSTAINABILITY / MODEL**
A free web app that needs no cloud servers; a low-cost band kit; clinic and teledermatology
partnerships.

**RISKS, PRIVACY & LIMITATIONS**
A screening aid, not a diagnosis. Data stays on the device. Needs more real-world and
darker-skin photos; the scratch model has been tested on few people; the flare score isn't
clinically validated yet.

**IMPLEMENTATION ROADMAP**
1. Person-by-person scratch test and the volunteer pilot
2. GSR and the skin-temperature thermistor on the band, then the stress model on live data
3. Dermatologist validation study, then a 2-4 week wear study
4. Off the breadboard: a PCB band

Our solution matters because it brings fair, private eczema care to every skin tone, at home.

## 14. Future scope

Beyond the finale build, kept so the work isn't lost:
- **On-device camera:** a 300 KB INT8 model on an ESP32-CAM, already trained and building
- **Personal flare model:** learn which triggers matter for each user from their own check-ins
  (Bayesian, like EczemaPred), replacing the fixed rule
- **Finger-only scratching:** a low-noise vibration sensor to catch what motion misses (ADAM study)
- **A second band** for the other hand
- **Lesion-vs-healthy skin temperature:** only with dermatologist support and data that it tracks flares
- **Clinic and teledermatology:** share reports with a doctor, with consent and a full DPDP Act privacy policy
- **iPhone:** a native app, since iPhone browsers don't support Web Bluetooth
