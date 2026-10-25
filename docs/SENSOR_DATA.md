# Sensor data: what we have, what it shows, where the models go next

Notes from 24 Oct 2026, when we downloaded and checked the real datasets behind the wristband.
Start here if you're picking up the wearable models. Hardware, pins and the decisions log are in
[HARDWARE_PLAN.md](HARDWARE_PLAN.md).

## Summary

We checked the real data behind every wearable signal. Stress and skin temperature come from
**WESAD**: 15 healthy adults wearing an Empatica E4 wristband through a calm baseline and a
stress task. Skin moisture comes from the **Southampton e-textile study**: 13 eczema patients,
each measured on eczema skin and on their own healthy skin. Both are downloaded under
`dataset/` (git-ignored, not redistributable or too large for the repo), and two scripts in
`scripts/` rebuild readable tables from the downloads.

Both datasets back up what the app relies on:
- Under stress, skin conductance and heart rate rise in 14 of 15 people.
- The moisture sensor reads lower on eczema skin in 13 of 13 patients.

The scratch sensor has no public data, so we plan a labelled pilot recording from a volunteer
with eczema.

What no dataset has is the link from these triggers to an actual flare. That's why flare risk
is currently a stated rule (`app/pipeline.py`: image severity × a noisy-OR of triggers), not a
trained model. The next modelling step is to learn that link per user from their own check-in
history (see the last section).

## The datasets

### WESAD: stress and skin temperature
- **Docs:** https://ubi29.informatik.uni-siegen.de/usi/data_wesad.html. The download also has a
  per-participant `S*_readme.txt`, and `wrist_E4/info.txt` lists each table's units.
- **Paper:** Schmidt et al., "Introducing WESAD, a multimodal dataset for Wearable Stress and
  Affect Detection", ICMI 2018, https://doi.org/10.1145/3242969.3242985. Read it for the study
  protocol (baseline / TSST stress / amusement / meditation) and its benchmark models: up to 93%
  accuracy on stress vs non-stress.
- **Licence:** academic, non-commercial. Cite the paper.
- **Get it:** the ICMI'18 zip (2.25 GB) from the page above, saved as `dataset/WESAD/WESAD.zip`.
- **Script:** `python scripts/wesad_wrist_summary.py`. It extracts the wristband CSVs and writes
  `dataset/WESAD/wesad_wrist_summary.csv`, with calm vs stress means per person.

| Under stress (TSST) | People |
|---|---|
| Skin conductance (EDA) up, e.g. 5.1 → 14.0 µS | 14 / 15 |
| Heart rate up, e.g. 73 → 120 bpm | 14 / 15 |
| Skin temperature slightly down | 12 / 15 |

Two things to watch for:
1. **Clock offset.** The phase times in `S*_quest.csv` count from the chest strap's start, but
   the wristband started 15–24 min earlier. Without correcting for this, heart rate looks
   *lower* under stress for everyone. The script corrects it; `wrist_lead_min` is each person's
   offset.
2. **Don't unpickle the `S*.pkl` files.** Loading a downloaded pickle can run code. The script
   leaves them inside the zip; the CSVs hold the same wrist signals.

Limitation for the pitch: all 15 are healthy young adults (12 male, 3 female, aged 24–35). That's
fine for a stress detector, since stress physiology isn't eczema-specific, but they aren't eczema
patients.

### Southampton e-textile study: skin moisture
- **Docs:** https://eprints.soton.ac.uk/511063/ (DOI 10.5258/SOTON/D3873). The dataset's own
  `README.txt`, extracted to `dataset/southampton_moisture/README.txt`, lists 8 related
  publications. Most useful for modelling:
  - Todorov et al., "Wearable sensor system for monitoring atopic dermatitis in patients", IEEE
    Sensors Journal, https://doi.org/10.1109/JSEN.2025.3601742 (the patient study; an earlier
    data deposit for it is https://eprints.soton.ac.uk/506015/)
  - https://doi.org/10.1109/jsen.2023.3342992 and https://doi.org/10.3390/s23083935 (sensor design)
  - The PhD thesis itself: https://eprints.soton.ac.uk/509845
- **Licence:** CC-BY 4.0.
- **Get it:** `Dataset_EtexSensAtopicDermatitis.zip` (53 MB) from the page above, downloaded in a
  browser (the site blocks scripts), into `dataset/southampton_moisture/`.
- **Script:** `python scripts/southampton_moisture_summary.py`. It extracts the patient tables to
  `dataset/southampton_moisture/PatientTest/`, the path `app/wearable.py` reads, and writes
  `moisture_patient_summary.csv`.

| Measurement | Result | Patients |
|---|---|---|
| E-textile sensor lower on eczema skin (by 1.7–10.2 %) | ✔ | 13 / 13 |
| Corneometer (clinical hydration standard) lower on eczema skin (by 12–55 %) | ✔ | 13 / 13 |
| Water loss (TEWL) higher on eczema skin | ✔ | 7 / 7 |

Units in `moisture_patient_summary.csv` (the patient CSVs don't label them; worked out from the
rest of the dataset):

| Columns | Unit |
|---|---|
| `sensor_eczema`, `sensor_healthy` | pF: the e-textile patch's capacitance (their GUI logs "Total Capacitance (pF)"); wetter skin reads higher |
| `corneo_eczema`, `corneo_healthy` | A.U., the Corneometer's arbitrary 0-120 scale (labelled A.U. in the thesis's own tables) |
| `TEWL_eczema`, `TEWL_healthy` | g/m²/h, the standard TEWL unit (not stated in the dataset) |
| `*_deficit_pct` | %: our calculation, (1 − eczema / healthy) × 100 for the same patient |

Our own moisture sensor won't read in pF, so only the **percentage deficit** carries over.

How big the sensor's drop is only loosely tracks the Corneometer (r = 0.34). Claim that it
*detects* drier eczema skin, not that it *measures* dryness clinically. The zip also contains a
`SkinSensorGUI.exe`; it isn't needed, so don't run it.

### Scratching: no public dataset
Published studies (Ji et al., npj Digit. Med. 2023; Moreau et al., IEEE JBHI 2018; the ADAM
sensor, Chun et al., Sci. Adv. 2021) didn't release their recordings, and none uses our
hardware. So the scratch model trains on our own recordings from the DermEdge band (ADXL345 at
800 Hz on the back of the hand): `tools/record.py` and `tools/tune.py`, see
[HARDWARE_PLAN.md](HARDWARE_PLAN.md) §6. The volunteer day below is its held-out test.

## Augmentation plan

The organisers allow synthetic data. We use it as **data augmentation**: modified copies of
real recordings, so a model trained on research-grade sensors copes with ours.

**Our sensors differ from the datasets' in three ways**, and each needs handling:

| Difference | Handling |
|---|---|
| Scale (pF or µS vs a raw ESP32 count) | Work in relative terms: change from the wearer's calm baseline (stress), or % deficit vs the same person's healthy skin (moisture) |
| Sensitivity | Moisture: measure our sensor's % change on dry vs moisturised skin. Compare it with Southampton's own dry vs treated test (`Chapter5_SilverIDC/Encapsulated_Silver_IDC_Study/VolunteerTest/`, not yet checked), and scale their deficits by the ratio |
| Noise, drift, steps | Measure them on our own hardware (below) and add them to each copy |

**Bench measurements** (on any of us, before the volunteer day): each sensor held still on skin
for 10 minutes (noise and drift); moisture patch pressed lightly vs firmly; moisture before and
15 minutes after moisturiser; GSR and thermistor through 5 min calm, 5 min mental arithmetic and
5 min rest.

**Rules:**
1. Split by person (or patient) **first**, then augment only the training people.
2. Test on **real, un-augmented** held-out data, and report only those numbers.
3. Augmentation adds variety, not information. 13 Southampton patients stay 13 patients, and no
   augmentation turns healthy WESAD people into eczema flares.
4. **Don't synthesise paired data.** No generated stress or GSR for the Southampton patients:
   nothing in that data says what their stress was. Each sensor model trains on its own dataset
   and outputs its own 0-1 score (late fusion); only the trigger→flare step would need data from
   the same people, and that has to be real.
5. "Virtual patients" (a WESAD person replayed together with a Southampton patient, as the
   simulator does) are fine for demos and software tests, labelled as simulation. Never quote
   accuracy from them.

## Volunteer day (one-day pilot)

One day, one friend with eczema, about 3 hours. Everything that doesn't need eczema skin is done
on ourselves first (bench measurements above), so the volunteer's time goes to:

| Time | Test | What it shows |
|---|---|---|
| 0:00 | Written consent, fit the band, 10 min sitting quietly | Calm baseline and noise on a real wearer |
| 0:15 | Moisture patch on an eczema spot vs a matching healthy spot, alternated 10 times, 30 s each | Repeats the Southampton finding with our sensor |
| 0:35 | Photos of the eczema site through the app, in different light | Real-patient test of the image stages |
| 0:50 | Scratch recording: 3 blocks of 10 min mixing scratching with look-alikes, filmed (hands only) | Held-out test of the scratch model |
| 1:30 | Optional 5 min stress task | Stress check on an eczema patient |
| 1:45 | Moisture before and after their usual cream | Our sensor's response on eczema skin |

- **Never ask them to scratch the eczema**: mimic on a sleeve or healthy skin; natural scratches
  count too.
- Clap or tap three times on camera at the start of each block, to line up video and data.
- No faces or names in any file; recordings stay out of git. Ask Dr. Priyanka Dwivedi to look
  over the consent form.
- Present it as an **N = 1 feasibility pilot**, with claims like "our patch read lower on eczema
  skin in X of 10 placements" and "caught X of Y scratches in a block it never trained on".

## Models: what exists and what to look into

| Piece | Now | Status |
|---|---|---|
| Stress | LightGBM on 30 s windows of EDA, temp, PPG, motion, normalised to the wearer's calm baseline (`scripts/fusion_pipeline.py`, `wesad_features.py`, `wesad_calibration.py`) | Proper model. AUC 0.92–0.94, tested on one held-out person at a time |
| Dry skin | Fixed threshold on the patch-vs-reference deficit (`app/wearable.py`) | Right labels, not yet a tested model |
| Trigger → flare | Hand-set weights and a noisy-OR, times image severity (`app/pipeline.py`) | A rule, because no dataset pairs wearables with flares |

Directions to look into:
1. **Dry-skin classifier.** Turn the threshold into a model tested on one held-out patient at a
   time with the Southampton data. Compare each patient's eczema patch with their own healthy
   skin, because absolute readings vary a lot between people (40–48). Small data (13 patients),
   so keep the model simple.
2. **Personal trigger → flare model (main gap).** Every check-in already stores the severity
   score and the active triggers (`app/features.py` `insights()` does a with/without comparison).
   Fit a per-user Bayesian logistic regression, label = "severity worse at the next check-in",
   with today's rule as the prior. On day 1 it behaves like the rule; as check-ins accumulate,
   it learns which triggers matter for that person. Evaluate by forward-chaining: train on days
   1…n, predict n+1, and compare against the fixed rule. Published precedent for personalised
   Bayesian eczema-severity forecasting:
   - Hurault et al., "Personalised prediction of daily eczema severity scores using a
     mechanistic machine learning model",
     https://www.medrxiv.org/content/10.1101/2020.01.16.20017772.full.pdf
   - Hurault et al., "EczemaPred: a computational framework for personalised prediction of eczema
     severity dynamics", Clinical and Translational Allergy 2022,
     https://www.ncbi.nlm.nih.gov/pmc/articles/PMC8967258/ (R package; Bayesian state-space
     models on PO-SCORAD)
3. **Real data for (2).** The volunteer wears the band for 2–4 weeks and logs a daily photo of
   the same body site plus a daily 0–10 itch score and the journal. That's 14–28 labelled days:
   enough to show the model adapting, not enough to claim accuracy.

Avoid: training on synthetic flare data and quoting its accuracy, predicting eczema from WESAD,
which has no eczema labels, and treating skin temperature as an eczema signal (it's a stress
input only; see the decisions log in [HARDWARE_PLAN.md](HARDWARE_PLAN.md)).
