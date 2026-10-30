# Final stretch: everything left before the finale

Written 31 Oct 2026, night before the finale. Everyone: `git pull` first. The slide text is in
[PITCH_DECK.md](PITCH_DECK.md), the diagrams are `docs/architecture/v2_*.png`, and the live site
is derm-edge.vercel.app (the v2 skin model is live).

Tick items off in this file as you go (edit, commit, push), so everyone sees the state.

**The story for the sensors (decided):** we don't train or show a model on our own sensors. The
band **collects data for the doctor** (scratching, heart rate and HRV from the ECG, skin
temperature), and models come later once that data pools. The one sensor-to-eczema result we
quote is **Southampton**: a threshold classifier built on their data (eczema patch drier than the
same patient's healthy skin) flags eczema skin in **13 of 13 patients**. Call it a "threshold
classifier", not a trained or neural model: 13 patients is too few to train one, and a simple
rule already separates all 13. The scratch detector on the band is the only model we trained
on our own recordings (22 of 26 scratch clips, 7 people).

## Order of work (do them in this order)

| # | When | Who | What |
|---|---|---|---|
| 1 | now | all | `git pull`, read this file |
| 2 | now, ~1 h | Shreeya | Build the slides in Canva from PITCH_DECK.md (below) |
| 2 | now, ~45 min | Nitish | Phone + laptop checks, then record video part A |
| 2 | now, ~1.5 h | Tishya | Band on the body, then record video part B |
| 3 | after videos | Nitish | Edit parts A + B into one 3-5 min video, drop it into slide 4 |
| 4 | after slides | all | Two timed run-throughs of the full 10 minutes |
| 5 | before sleeping | all | Study your Q&A area (below) |
| 6 | morning | all | Day-of checklist (bottom) |

---

## Shreeya: the slides (Canva)

**Source:** every slide's text is in [PITCH_DECK.md](PITCH_DECK.md), sections 1-13, with the
timing table at the top. Put on the slides only what's under each slide heading. The section
"For questions from judges" is **not** for slides; it's our Q&A crib.

**Using Claude with the Canva connector (you have it):** open Claude with Canva connected and
paste this, with the repo's files attached or pasted in:

> Open my Canva design https://canva.link/sjz2ti8lsuddh4j. First list every page with its current
> text, and don't change anything yet. Then, page by page, replace the text with the matching
> section of the PITCH_DECK.md I'm giving you (sections 1-13; skip "For questions from judges").
> Keep the design's fonts, colours and layout; one idea per slide; no paragraph longer than two
> lines. Add a page if a section has none. Show me each page before moving to the next.

Then upload the images and place them (Canva connector upload, or drag them in by hand):

| Slide | Image (docs/architecture/) |
|---|---|
| 5 System architecture | `v2_0_full_system.png` |
| 6 How the skin model evolved | `v2_1_old_vs_new.png` |
| 7 Stages 1 and 2 | `v2_2_stage1_rash.png`, `v2_3_stage2_eczema.png` |
| 8 Stage 3 severity | `3_stage3_severity.png` (still current) |
| 9 Hardware | a photo of the band on the breadboard + `v2_5_band_data_loop.png` |
| 10 From data to diagnosis | `v2_5_band_data_loop.png` (the loop half) or three steps drawn in Canva |
| 11-12 Results, vs the market | `v2_6_results_vs_market.png` |
| 13 Future scope (optional) | `v2_4_device_tiers.png` |

The images are 3200 × 1800 PNG. If one is too dense for a slide, crop it to the top half (the
flow) and keep the numbers as slide text.

**Rules for the slide text (so no judge can catch us out):**
- Old numbers are gone: no "81.1%", no "AUC 0.93", no "67% → 83% calibration", no "ResNet18 / Grad-CAM".
- Say "built and tested for South Asian skin", never "best for South Asian skin" (no published comparison exists).
- Keep the small footnote on the market slide: Google's numbers are a different task.
- No stress model and no stress accuracy: the ECG gives "heart rate and HRV, recorded for the doctor".
- Southampton: "threshold classifier, 13 of 13 patients", never "we trained a model".
- Severity: say "within one band 97.6% of the time", not "dermatologist-labelled" (still checking who labelled it).

**Check with the team:** the footer says "Finale · MGUMST Jaipur". Confirm the round's exact name
and venue from the organisers' email before export.

**Export:** PDF + PPTX, both onto a pen drive and Google Drive, with the video embedded **and** as
a separate MP4 next to it.

---

## Nitish: site checks, video part A, the edit

**Checks first (15 min):**
- [ ] Phone (Android, Chrome): open derm-edge.vercel.app, wait for "Models ready", run one check. If it fails, send the banner text to Claude.
- [ ] iPhone (Safari): same check; install via Share → Add to Home Screen.
- [ ] Laptop (Chrome): an eczema photo shows rash → eczema → severity. Does a **heatmap** appear? Tell Claude either way.
- [ ] **Healthy Indian skin:** 10 checks on our own healthy arms and hands, different light. Count how many say "No rash". If more than 2 of 10 say rash, tell Claude before the demo.
- [ ] Turn on airplane mode after the model has loaded once and run a check: the offline claim must hold.

**Video part A (skin check), per PITCH_DECK.md slide 4:**
1. Phone: open the site, show "Install DermEdge" / installed app
2. Healthy skin → "No rash detected"
3. An eczema photo (from a screen or printed): rash → eczema → severity; on the laptop, the heatmap
4. History tab: one body site improving (use **Load sample history**, it's labelled); open the doctor report

Record at 1080p, phone screen recording plus one shot of the phone in hand. Keep it to 1.5-2 min.

**Edit:** part A + part B into one video, 3-5 minutes, captions on (judges may watch muted), no
music over speech. Export MP4 1080p; also keep A and B as separate files in case of a time cut.

---

## Tishya: the band, video part B, two answers

**Band (in this order):**
- [ ] LM35 one-time calibration against a real thermometer (`temp cal`), then an air-vs-skin check
- [ ] ECG on the body: electrodes on both wrists, steady heart rate and HRV in the bring-up test
- [ ] The website (latest `main`) shows the band's live ECG heart rate, HRV and skin temperature (`a9de348`)
- [ ] Scratch episode → the website's count goes up
- [ ] A flare alert reaches the band's screen; double tap dismisses it
- [ ] Spare: USB cable, a power bank that powers the band, spare electrodes, tape

**No model to train.** The ECG and LM35 only need to read reliably and show up on the website:
the pitch says the band collects data for the doctor, and models come later.

**Video part B (band), per PITCH_DECK.md slide 4:** power on and pairing code → start a
recording from the touch pad → scratching → the website's count goes up → ECG and temperature
arriving live → an alert on the band, double tap. Film the band and the laptop screen together
in one shot if possible. 1.5-2.5 min.

**Two answers for the team:**
- [ ] Who labelled the severity data (`dead_ends/eczema_severity_proxy_attempt`)? Dermatologist, us, or a proxy? Write it here.
- [ ] Confirm the scratch numbers on the slides: 22 of 26 scratch clips across 7 people, leave-one-person-out.

---

## Study: who answers what

Each person owns an area, and the others pass questions to them. Read the "For questions from
judges" section of PITCH_DECK.md together once.

**Nitish: the AI**
- What CLIP is (a model trained on 400M image-text pairs), what an embedding is (512 numbers per photo), why one frozen encoder with small trained heads (a logistic regression each) instead of one big model
- AUC (1.0 perfect, 0.5 chance), sensitivity / specificity, 5-fold cross-validation, why SCIN is split by patient
- The v1 → v2 story: 81% on atlas photos, failed on real photos, retrained with SCIN + SkinDisNet; the DermLIP / PanDerm benchmark and why we kept CLIP (as good, lighter, freely licensed; Derm1M includes SCIN, so SkinDisNet was the fair test)
- Skin-tone fairness: Fitzpatrick I-VI, V-VI 67% → 86%
- The occlusion heatmap (grey out squares, see how the score drops)
- On-device: ONNX Runtime Web, WebGPU on laptops, WebAssembly on phones, 90 MB once, offline, nothing uploaded
- The weak spots and the answers: US-photo stage 2 is AUC 0.70 (fix: more labelled consumer photos); SkinDisNet CV is by photo, not patient

**Tishya: the hardware and sensors**
- ESP32, BLE with a 6-digit passkey (encrypted), offline buffering, the band's screen and touch pad
- ADXL345 scratch detection: 800 Hz, the rhythm and frequency features, the 7-person leave-one-person-out result and its limits (acted scratching, not eczema at night)
- AD8232 ECG: Lead I across both wrists, R-peaks → heart rate; HRV (RMSSD) and what a doctor reads from it (stress and sleep load); lead-off detection
- LM35: 10 mV/°C, calibration, why it's recorded as context and not used as an eczema signal
- The Southampton study: 13 patients, moisture sensor lower on eczema skin in 13/13, Corneometer 13/13, TEWL higher 7/7; our threshold classifier on it (13/13) and why it's a rule, not a trained model; why moisture is future scope on our band
- Why no public dataset links wearables to eczema flares, and so why the band collects data first (the collect → doctor validates → model automates loop)

**Shreeya: the problem, the clinic, the market**
- Eczema burden (GBD 2017, 204 million, 8.5% of Central Indian school children), look-alikes (psoriasis, tinea, scabies) and why telling them apart matters for treatment
- Severity scales: EASI and SCORAD, our 0-12 score from four EASI signs (redness, papules, scratch marks, thickening)
- The skin-tone gap in dermatology data (atlases hold 3.6× more lightest- than darkest-skin images)
- The market slide: what MedSigLIP and Derm Foundation are, why their task differs, what DermEdge does that they don't (rash + eczema + severity, on the phone, with the band's tracking)
- Privacy: on-device, consent at setup, delete-all, India's DPDP Act; screening aid, not a diagnosis
- Sustainability: free web app, no servers to pay for, low-cost band kit, clinic partnerships

**Everyone:** the 30-second answer to "what is DermEdge?", and the flow on slide 5.

---

## Run-throughs

- [ ] Run-through 1: full 10 minutes with the video, stopwatch on. Note where it runs long.
- [ ] Cut, then run-through 2. Target 9:30, leaving 30 s of slack.
- [ ] Decide who speaks which slides and who presses play.
- [ ] Practise the fallback: if the video won't play, the live site on the laptop (already loaded).

## Day-of checklist

- [ ] Laptop charged, charger, HDMI / USB-C adapter
- [ ] Site loaded once on the laptop and both phones before leaving (it then works offline)
- [ ] Video as MP4 on the laptop, a pen drive and Google Drive
- [ ] Slides as PDF + PPTX in the same three places
- [ ] The band, its USB cable, a power bank, spare electrodes
- [ ] A phone hotspot in case the venue Wi-Fi is bad
- [ ] Printed sample eczema photo for a live demo if asked
