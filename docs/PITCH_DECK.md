# DermEdge pitch deck: slide text for the Canva rebuild

**The deck in Canva:** https://canva.link/sjz2ti8lsuddh4j (the team's working copy; this file has its text).

Rewritten 31 Oct 2026 for the finale format: **10 minutes in total, including the demo video.**
The video is 41 seconds (skin check only, no sound: we talk over it), so the deck is 16 slides,
about 35 seconds each. Slides carry short bullets only; everything to say out loud is in each
slide's **speaker notes in Canva** (the Notes button under the slide, or Presenter view; the PPTX
export keeps them in the notes pane). Diagrams are in `docs/architecture/` (3200 × 1800 PNG); the `v2_*` ones are this deck's,
redrawn by `python scripts/draw_pitch_v2.py`.

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
- **31 Oct night:** a "Why DermEdge is different" slide (what's missing today vs what DermEdge
  does), Impact and Future scope split into two slides so impact gets a full slide, Nitish's demo
  video on slide 5, and a QR code for derm-edge.vercel.app on the closing slide
  (`docs/demo/dermedge_site_qr.png`).

Footer for every slide: **Telemedicon 2026 · Finale · MGUMST Jaipur** (check the organisers'
exact name for the round).

## Timing

| Time | Slide |
|---|---|
| 0:00 | 1 Title, 2 Problem |
| 1:00 | 3 Solution |
| 1:30 | 4 Why DermEdge is different |
| 2:15 | 5 **Demo video, 41 s**, narrated live |
| 3:15 | 6 Architecture, 7 Old vs new, 8 Stages 1-2, 9 Stage 3 |
| 5:20 | 10 The band, 11 Data to diagnosis |
| 6:40 | 12 Results, 13 DermEdge vs the market |
| 8:00 | 14 **Impact** (slow down: this is what the judges score) |
| 8:45 | 15 What dermatologists told us, 16 Future scope |
| 9:30 | 17 Thank you + QR code, questions |

If time runs short, say slide 11 in one sentence and skip the detail on slide 9.

Slide numbers here are the deck's corner labels ("NN/ TELEMEDICON 26'"); the full-page diagram
slides in Canva sit between them and carry no label.

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
- **The DermEdge band** records scratching, heart rate and HRV (ECG) and skin temperature for the doctor, and shows alerts
- **Tracks every body site over time**, with treatments, triggers and a printable doctor report
- **Private:** no photo leaves the device; works offline once installed

## 4. Why DermEdge is different

Title: **WHY DERMEDGE IS DIFFERENT**. Four boxes, each "what's missing today" then "what DermEdge does":

| Box | Today | DermEdge |
|---|---|---|
| **Between visits** | Nothing records the weeks between visits | Photo history and band data in one doctor report |
| **On brown skin** | Most skin AI is trained on lighter skin | 84% of rashes caught on South Asian photos (was 33%) |
| **Look-alikes** | Psoriasis, tinea and scabies get mistaken for eczema | Flags look-alikes and says "see a doctor" |
| **Private and offline** | Skin apps upload your photos to the cloud | Runs on your phone, offline; no photo leaves it |

Say it as "Today... DermEdge...". Never "the best": say "built and tested for South Asian skin".

## 5. Demo (video, 41 seconds)

Video: [`docs/demo/DermEdge_demo_partA.mp4`](demo/DermEdge_demo_partA.mp4), on the slide under the
title "DermEdge in action" (subtitle: the skin check on a phone, then three photos on a laptop).
No sound, captions only: we narrate it live (script in the slide's speaker notes).

1. DermEdge installed on a phone like an app: runs fully on the phone, no photo uploaded
2. Healthy skin: "No rash detected"
3. A rash, captured automatically in about 2 seconds: eczema (89%), moderate severity
4. Laptop, three labelled uploads: eczema on darker skin → likely eczema + heatmap; fungal
   infection → look-alike; psoriasis → look-alike, see a doctor

The band is not filmed: show the band itself on slide 10 (photo + the website's sensor readings).

## 6. System architecture

Diagram: `docs/architecture/v2_0_full_system.png`

Caption: Two paths, one device. The camera path answers rash, eczema and severity; the band path
records scratching and stress; both meet in a daily flare risk and the doctor report.

## 7. How the skin model evolved

Diagram: `docs/architecture/v2_1_old_vs_new.png`

Talking point: *"Our first model scored 81% on textbook photos. When we tested it on real hospital
photos from South Asia, it struggled. So we rebuilt it: one shared CLIP pass with three trained
heads, retrained with South Asian hospital photos. It now catches 84% of those rashes instead of
33%, and works as well on darker skin as on lighter skin."*

## 8. Stages 1 and 2: rash, then eczema vs look-alikes

Diagrams: `docs/architecture/v2_2_stage1_rash.png` and
`docs/architecture/v2_3_stage2_eczema.png` (the occlusion heatmap is in stage 2). Use one per half of the slide, or two
slides if time allows.

Captions:
- Stage 1: AUC 0.92 on real phone photos; 86% of rashes caught on the darkest skin types, the same
  as on lighter skin.
- Stage 2: AUC 0.87 on South Asian hospital photos; on par with dermatology foundation models
  (DermLIP, PanDerm) in our head-to-head test.

## 9. Stage 3: severity

Diagram: `docs/architecture/3_stage3_severity.png` (unchanged)

Caption: The same CLIP embedding, a third head: four EASI signs scored 0-12. Within one severity
band 97.6% of the time; mean error 1.2 points.

## 10. The hardware: the DermEdge band

Photo of the band on the breadboard, plus `docs/architecture/v2_5_band_data_loop.png` (sensors → Bluetooth → record → doctor
reviews → data pools → models learn, with the Southampton evidence box).

**ON THE BAND**
- **ADXL345 accelerometer** on the back of the hand: scratch detection running on the ESP32
- **AD8232 ECG**, a 1-2 minute seated check with electrodes on both wrists: heart rate and HRV, recorded for the doctor
- **LM35 skin temperature**: recorded alongside
- Touch pad, 1.8" screen for alerts, encrypted Bluetooth, about 20 hours of offline storage

**KNOWN BIOMARKERS CAN FLAG ECZEMA: THE SOUTHAMPTON STUDY**
We analysed the Southampton e-textile study (13 patients with eczema, University of Southampton,
2026). Comparing each patient's eczema skin with their own healthy skin:
- The wearable moisture sensor read lower on eczema skin in **13 of 13 patients**
- The clinical standard (Corneometer) agreed in **13 of 13**; water loss (TEWL) was higher in **7 of 7**

From this data we built a threshold classifier (eczema patch drier than the same patient's healthy
skin) that flags eczema skin in **13 of 13 patients**. So a wearable reading of skin barrier
biomarkers can detect eczema, and this is the basis for a moisture patch on a future band.

## 11. From data to diagnosis

**TODAY: THE BAND COLLECTS THE DATA**
No public dataset pairs our sensors (scratching, ECG, skin temperature) with eczema outcomes. So
the band records them, and the website charts each recording with a CSV download: a doctor reads
scratching at night, heart rate, HRV and temperature next to the photo history.

**NEXT: DOCTORS LABEL, THE DATA POOLS**
Every recording a doctor reviews becomes labelled data: this night, this flare, this severity.

**THEN: A MODEL AUTOMATES IT**
Once enough data pools up, a model learns the patterns, as we did for scratching from our own
recordings and for skin barrier readings from the Southampton study. The doctor then only confirms.

Visual: the loop in `docs/architecture/v2_5_band_data_loop.png` (shared with slide 10), or three steps left to right:
**Collect → Doctor validates → Model automates**.

## 12. Results

| What | Result | How we tested |
|---|---|---|
| Rash or not (stage 1) | **AUC 0.92**, 88% of rashes caught | Real phone photos (SCIN), people the model never saw |
| Rashes on South Asian hospital photos | **84% caught** (first version: 33%) | SkinDisNet, 1,710 photos, 2 hospitals in Bangladesh |
| Darkest skin types (Fitzpatrick V-VI) | **86% caught, AUC 0.95**, the same as lighter skin | SCIN, by skin type |
| Eczema vs look-alikes (stage 2) | **AUC 0.87**; 78% of eczema caught, 81% of look-alikes cleared | South Asian hospital photos, 5-fold cross-validation |
| Severity (stage 3) | **97.6%** within one band; mean error 1.2 / 12 | Held-out validation set |
| Scratch detection | 22 of 26 scratch clips, 14 false alarms in 73 others | 7 people, each tested by a model that never saw them |
| Skin barrier biomarker | Eczema skin drier in **13 of 13** patients | Southampton e-textile study |

## 13. DermEdge vs the market

Chart: `docs/architecture/v2_6_results_vs_market.png` (AUC bars, with the different-task footnote).

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

Footnote on the slide (small): *Google figures: 79 skin conditions, US tele-dermatology data
(MedGemma technical report, arXiv 2507.05201, Table 15): a different task. DermLIP: our head-to-head on SkinDisNet with the same heads.
Different datasets, so a guide, not a contest.*

## 14. Impact

The slide the judges score: big, few words. Three stat boxes, then who benefits, then one line.

| 204M | FREE | 84% |
|---|---|---|
| people live with eczema; 8.5% of Central Indian school children | to check your skin: any phone browser, no servers, works offline | of rashes caught on South Asian hospital photos |

**WHAT CHANGES** (two check marks)
- Patients: a private check in seconds, at home · Families: when to see a doctor, and what to show
- Doctors: the weeks between visits, in one report · Few dermatologists nearby: a free first check

Closing line: **Care that doesn't stop between visits.**

Screening aid, not a diagnosis (say it in the notes, not on the slide). Not "₹0": Playfair draws
the zero like a letter o.

## 15. What dermatologists told us

Bridge into future scope. Four boxes, same style as slide 4. No names on the slide.

| Box | Bullets |
|---|---|
| **What they check** | How dry the skin is near the rash · How warm the skin is around it |
| **What's missing** | Checked by hand, only at the visit · Nothing measures it in between |
| **The evidence** | Southampton: drier eczema skin, 13 of 13 · Clinical moisture meter agreed, 13 of 13 |
| **What we'll add** | A soft skin patch on the rash · Moisture + temperature, recorded for the doctor |

Rules: say "check" or "examine", never "diagnose" (eczema is diagnosed clinically; dry skin is one
of the signs). Name the dermatologists out loud only if a judge asks and they agreed to be named.
Moisture is the strong evidence (Southampton); temperature at the rash is supporting, and our band's
LM35 is on the wrist, so only the future patch measures it at the rash. The patch isn't built yet.

## 16. Future scope

Three boxes, said left to right as steps: measure more, then smarter models, then reach more people.

| Measure more | Smarter models | Reach more people |
|---|---|---|
| Skin patch on the rash: moisture + temperature | Stress model from heart rate + HRV | 17 MB model for low-end phones |
| What dermatologists check, recorded every day | Flare model, once doctors label band data | 300 KB camera on the band |
| Optical heart sensor: no electrodes | Dermatologist-validated severity scores | Clinic partnerships, with consent; a smaller PCB band |

Closing line on the slide: **Private, fair eczema care at home.** None of these exist yet (the
17 MB and 300 KB models are trained but not shipped); say "next", never "we have".

Close: *"DermEdge brings private, fair eczema care home: a check in seconds, tracking in between,
and data your doctor can act on."*

## 17. Thank you

"Team DermEdge · derm-edge.vercel.app", with the QR code for the site
(`docs/demo/dermedge_site_qr.png`). Leave it up during questions.

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
- **Severity labels** cover four EASI signs on curated photos, not a dermatologist's full EASI. Ask Tishya who labelled
  them (they sit in a `dead_ends/eczema_severity_proxy_attempt` folder) before calling them dermatologist-labelled.
- **Stress:** we don't claim a stress model. The ECG gives heart rate and HRV, recorded for the doctor;
  a model comes once labelled data pools. (Earlier we built one on WESAD's skin conductance: 90%
  accurate, leave-one-person-out. The band switched to ECG, so it isn't on the band.)
- **Southampton** is a threshold classifier (patch vs the same patient's healthy skin), not a trained
  neural network: 13 patients is too few to train one, and a simple rule already separates all 13.
- **Flare risk** is a stated rule (image score × noisy-OR of triggers), not a trained model.

Sources: [MedSigLIP model card](https://huggingface.co/google/medsiglip-448) ·
[Derm Foundation](https://developers.google.com/health-ai-developer-foundations/derm-foundation) ·
[DermLIP / Derm1M](https://github.com/SiyuanYan1/Derm1M) ·
[SkinDisNet](https://data.mendeley.com/datasets/yj3md44hxg) ·
[Southampton e-textile data](https://doi.org/10.5258/SOTON/D3873) · our runs:
`experiments/encoder_comparison/summary.json`, `experiments/web_heads_v2/summary.json`.
