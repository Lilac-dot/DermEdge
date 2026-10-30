# DermEdge pitch deck: slide text for the Canva rebuild

**The deck in Canva:** https://canva.link/sjz2ti8lsuddh4j (the team's working copy; this file has its text).

Rewritten 31 Oct 2026 for the finale format: **10 minutes in total, including a 3-5 minute demo
video.** That leaves about 5-6 minutes of talking, so the deck is 12 slides plus the video, about
30 seconds per slide. Diagrams are in `docs/architecture/` (3200 × 1800 PNG);
`python scripts/draw_architecture.py` redraws them.

**What changed since the 26 Oct draft**
- **The skin model was rebuilt** (slides 6-8). Stage 2 is no longer a separate ResNet18: it's a
  head on the same CLIP embedding, retrained with South Asian hospital photos. Old numbers
  (81.1% accuracy, AUC 0.93, 67% → 83% calibration, Grad-CAM) are gone: use the numbers here.
- **Sensors:** ADXL345 (scratching), AD8232 ECG check on both wrists (stress), LM35 (skin
  temperature). GSR and the moisture patch are dropped from the band; moisture moves to future
  scope.
- **The phone model is dropped for the finale.** The full model now loads on phones (WebAssembly),
  so the 17 MB phone model is future scope.
- **New slides:** the old vs new model, hardware and biomarkers, and DermEdge vs the market.

Footer for every slide: **Telemedicon 2026 · Finale · MGUMST Jaipur** (check the organisers'
exact name for the round).

## Timing

| Time | Slide |
|---|---|
| 0:00 | 1 Title, 2 Problem |
| 1:00 | 3 Solution |
| 1:30 | **Demo video, 3-5 min** (slide 4 is its title card) |
| 5:30 | 5 Architecture, 6 Old vs new, 7 Stages 1-2, 8 Stage 3 |
| 7:15 | 9 Hardware and biomarkers, 10 Data to diagnosis |
| 8:30 | 11 Results, 12 DermEdge vs the market |
| 9:30 | 13 Impact and future scope, close |

If the video runs 5 minutes, merge slides 7 and 8, and say slide 10 in one sentence.

---

## 1. Title

- TELEMEDICON 2026 · AI & DIGITAL HEALTH HACKATHON
- **DermEdge**
- Private AI eczema screening and flare tracking, on your own device
- Team DermEdge: Tishya Yadlapalli, Nitish GB, Shreeya Jetti

## 2. The problem

**THE PROBLEM**
Eczema flares between doctor visits, is easily confused with look-alikes (psoriasis, fungal
infections, scabies) that need different treatment, and most skin AI is trained on lighter skin.

**THE EVIDENCE**
- Highest burden of any skin disease; 15th of all non-fatal diseases (GBD 2017, Laughter et al.)
- About 204 million people affected worldwide (BJD global meta-analysis)
- 8.5% of school children in a Central India survey (Agarwal et al., EJPD)
- Dermatology atlases hold 3.6× more lightest- than darkest-skin images (Groh et al., Fitzpatrick17k)

**THE GAP**
Doctors see a snapshot every few weeks. Nothing records the days in between: no photos over time,
no scratching or stress data, and the apps that exist upload photos to the cloud.

## 3. Our solution

**DermEdge = a camera check + a wearable band + a doctor report, all on your own device.**
- **Is there a rash? Is it eczema? How severe?** Three answers from one photo, in the browser
- **The DermEdge band** records scratching, stress (ECG) and skin temperature, and shows alerts
- **Tracks every body site over time**, with treatments, triggers and a printable doctor report
- **Private:** no photo leaves the device; works offline once installed

## 4. Demo (video, 3-5 minutes)

Title card: **"DermEdge in action"**. Two parts, recorded separately:

**Part A: the skin check (website, phone)**
1. Open derm-edge.vercel.app on a phone, show the install as an app
2. Point the camera at healthy skin: "No rash"
3. Show an eczema photo: rash → eczema → severity band; on a laptop, the heatmap
4. The History tab: one body site improving over time; open the doctor report

**Part B: the band receiving sensor data**
1. The band powering on, pairing over Bluetooth with the 6-digit code on its screen
2. Start a recording from the band's touch pad
3. Scratching: the band detects an episode, the website's count goes up
4. ECG check (electrodes on both wrists) and skin temperature arriving in the website live
5. A flare alert sent to the band's screen, dismissed with a double tap

Keep the website's sensor charts on screen for a few seconds: that's the "data a doctor can read"
moment for slide 10.

## 5. System architecture

Diagram: `docs/architecture/0_full_system.png` (redrawn 31 Oct)

Caption: Two paths, one device. The camera path answers rash, eczema and severity; the band path
records scratching and stress; both meet in a daily flare risk and the doctor report.

## 6. How the skin model evolved

Diagram: `docs/architecture/7_old_vs_new.png` (new)

Talking point: *"Our first model scored 81% on textbook photos. When we tested it on real hospital
photos from South Asia, it struggled. So we rebuilt it: one shared CLIP pass with three trained
heads, retrained with South Asian hospital photos. It now catches 84% of those rashes instead of
33%, and works as well on darker skin as on lighter skin."*

## 7. Stages 1 and 2: rash, then eczema vs look-alikes

Diagrams: `docs/architecture/1_stage1_clip_rash_v2.png` and
`docs/architecture/2_stage2_clip_head_v2.png` (both new). Use one per half of the slide, or two
slides if time allows.

Captions:
- Stage 1: AUC 0.92 on real phone photos; 86% of rashes caught on the darkest skin types, the same
  as on lighter skin.
- Stage 2: AUC 0.87 on South Asian hospital photos; on par with dermatology foundation models
  (DermLIP, PanDerm) in our head-to-head test.

## 8. Stage 3: severity

Diagram: `docs/architecture/3_stage3_severity.png` (unchanged)

Caption: The same CLIP embedding, a third head: four EASI signs scored 0-12. Within one severity
band 97.6% of the time; mean error 1.2 points.

## 9. The hardware: the DermEdge band

Photo of the band on the breadboard, plus `docs/architecture/6_band_scratch.png`.

**ON THE BAND**
- **ADXL345 accelerometer** on the back of the hand: scratch detection running on the ESP32
- **AD8232 ECG**, a 1-2 minute seated check with electrodes on both wrists: heart rate and HRV for stress
- **LM35 skin temperature**: a stress input
- Touch pad, 1.8" screen for alerts, encrypted Bluetooth, about 20 hours of offline storage

**KNOWN BIOMARKERS CAN FLAG ECZEMA: THE SOUTHAMPTON STUDY**
We analysed the Southampton e-textile study (13 patients with eczema, University of Southampton,
2026). Comparing each patient's eczema skin with their own healthy skin:
- The wearable moisture sensor read lower on eczema skin in **13 of 13 patients**
- The clinical standard (Corneometer) agreed in **13 of 13**; water loss (TEWL) was higher in **7 of 7**

So a wearable reading of skin barrier biomarkers separates eczema skin from healthy skin. This is
the basis for a moisture patch on a future band.

## 10. From data to diagnosis

**TODAY: THE BAND COLLECTS THE DATA**
No public dataset pairs our sensors (scratching, ECG, skin temperature) with eczema outcomes. So
the band records them, and the website charts each recording with a CSV download: a doctor reads
scratching at night, stress and temperature next to the photo history.

**NEXT: DOCTORS LABEL, THE DATA POOLS**
Every recording a doctor reviews becomes labelled data: this night, this flare, this severity.

**THEN: A MODEL AUTOMATES IT**
Once enough data pools up, a model learns the patterns, as we did for stress from WESAD and for
scratching from our own recordings. The doctor then only confirms.

Visual: three steps, left to right: **Collect → Doctor validates → Model automates**.

## 11. Results

| What | Result | How we tested |
|---|---|---|
| Rash or not (stage 1) | **AUC 0.92**, 88% of rashes caught | Real phone photos (SCIN), people the model never saw |
| Rashes on South Asian hospital photos | **84% caught** (first version: 33%) | SkinDisNet, 1,710 photos, 2 hospitals in Bangladesh |
| Darkest skin types (Fitzpatrick V-VI) | **86% caught, AUC 0.95**, the same as lighter skin | SCIN, by skin type |
| Eczema vs look-alikes (stage 2) | **AUC 0.87**; 78% of eczema caught, 81% of look-alikes cleared | South Asian hospital photos, 5-fold cross-validation |
| Severity (stage 3) | **97.6%** within one band; mean error 1.2 / 12 | Held-out validation set |
| Scratch detection | 4 of 4 scratch clips, 0 false alarms in 12 others | One wearer, held-out round |
| Skin barrier biomarker | Eczema skin drier in **13 of 13** patients | Southampton e-textile study |

## 12. DermEdge vs the market

| | **DermEdge** | Google MedSigLIP | Google Derm Foundation | DermLIP (Derm1M) |
|---|---|---|---|---|
| Skin-condition AUC | **0.92** rash screen; **0.87** eczema on South Asian photos | 0.88 | 0.84 | 0.91 on South Asian photos, vs our 0.89 in the same test |
| Says if there's a rash | ✔ | needs a head per task | needs a head per task | needs a head per task |
| Eczema vs look-alikes | ✔ | – | – | – |
| Severity score | ✔ | – | – | – |
| Trained on South Asian hospital photos | ✔ | not stated | not stated | – |
| Runs on a phone, offline | ✔ 90 MB | ✗ ~800 MB, too big for phones | ✗ large TensorFlow model | model only, no app |
| Wearable tracking over time | ✔ band | – | – | – |

Talking point: *"The biggest models from Google score about the same as ours. They are general
building blocks; DermEdge is a finished screening tool that also scores severity, runs on a phone
and tracks the condition between visits, with sensor data to make it more accurate over time."*

Footnote on the slide (small): *Google figures: 79 skin conditions, Google's tele-dermatology
data (MedSigLIP model card). DermLIP: our head-to-head on SkinDisNet with the same heads.
Different datasets, so a guide, not a contest.*

## 13. Impact and future scope

**IMPACT**
Earlier, fairer eczema checks at home, built to work on South Asian skin, and doctor visits
informed by data from the days in between.

**FUTURE SCOPE**
- **Sensor-trained flare model:** once doctors have labelled enough band recordings, scratching,
  stress and temperature feed a learned model instead of a fixed rule
- **Moisture patch** on the band, using the Southampton biomarker finding
- **A 17 MB phone model** that copies the full model, for low-end phones (training script ready)
- **On-device camera:** a 300 KB model on an ESP32-CAM, already trained
- **Clinic partnerships:** share reports with a doctor, with consent, under the DPDP Act
- A PCB band instead of the breadboard

Close: *"DermEdge brings private, fair eczema care home: a check in seconds, tracking in between,
and data your doctor can act on."*

---

## For questions from judges (not on the slides)

Know these so no question catches us out:
- **US phone photos, stage 2:** eczema vs look-alikes on SCIN is AUC 0.70. We train on both sets
  together; a model trained on only one set doesn't transfer to the other. More labelled consumer
  photos is the fix.
- **The 0.87 on SkinDisNet** is 5-fold cross-validation split by photo: the dataset publishes no
  patient IDs, so the same patient can be on both sides. Treat it as an upper bound.
- **Healthy skin cleared** by the rash screen: 82% on SCIN. Only 107 healthy photos exist in SCIN
  (16 of them Fitzpatrick V-VI), and none from South Asia, so false alarms on healthy brown skin
  aren't measured yet. Test it on our own arms before the demo.
- **Why not DermLIP if it scored 0.91?** It's a 0.016 AUC gain, its licence forbids
  redistribution of derivatives, and switching would have meant retraining severity. Its training
  data includes SCIN, so its SCIN scores are inflated.
- **Market numbers** compare different datasets and tasks: MedSigLIP's 0.88 is an average over
  79 conditions on Google's data.
- **Severity labels** cover four EASI signs on curated photos, not a dermatologist's full EASI.
- **Stress:** the ECG model is being retrained on WESAD's chest ECG (Tishya); the old GSR model was
  90% accurate, 73% of stress caught, leave-one-person-out.
- **Flare risk** is a stated rule (image score × noisy-OR of triggers), not a trained model.

Sources: [MedSigLIP model card](https://huggingface.co/google/medsiglip-448) ·
[Derm Foundation](https://developers.google.com/health-ai-developer-foundations/derm-foundation) ·
[DermLIP / Derm1M](https://github.com/SiyuanYan1/Derm1M) ·
[SkinDisNet](https://data.mendeley.com/datasets/yj3md44hxg) ·
[Southampton e-textile data](https://doi.org/10.5258/SOTON/D3873) · our runs:
`experiments/encoder_comparison/summary.json`, `experiments/web_heads_v2/summary.json`.
