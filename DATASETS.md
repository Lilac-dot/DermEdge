# Datasets

Where to download the data behind DermEdge's models. None of it is stored in this repo:
it's too large for git, and patient photos and recordings can't be redistributed. Every
source below is public. Check each licence before reuse: several are non-commercial.

Put downloads under `dataset/<name>/` (see `scripts/paths.py`).

## Skin images

### SCIN: rash screen, healthy skin, skin-tone audit
Google/Stanford's crowd-sourced consumer dermatology dataset (Ward et al., *JAMA Network
Open* 2024;7(11):e2446615). 5,033 cases and 10,000+ smartphone photos taken by US users,
with dermatologist Fitzpatrick labels and Monk skin tone. Public bucket, no login:
```
https://storage.googleapis.com/dx-scin-public-data/dataset/scin_cases.csv
https://storage.googleapis.com/dx-scin-public-data/dataset/scin_labels.csv
https://storage.googleapis.com/dx-scin-public-data/<image_path>   (paths from image_1_path..image_3_path)
```
Schema: https://github.com/google-research-datasets/scin (`dataset_schema.md`).
`scripts/download_scin_skintone.py` fetches the skin-tone audit images and
`scripts/download_scin_healthy.py` fetches the healthy-skin cases.

### Human Skin Diseases: eczema vs look-alikes
Y. Mohamed, Kaggle, Apache 2.0. DermNet-style photos in 21 categories (including Normal),
all from the same source, with their own train/valid/test split:
https://www.kaggle.com/datasets/youssefmohmmed/human-skin-diseases-image

### Eczema subtype archive
17 DermNet-style eczema subtype folders, merged into the eczema class (11 subtypes kept).
The original download link was not recorded.

### Fitzpatrick17k-C: skin-tone audit
The cleaned Fitzpatrick17k release (Abhishek, Jain & Hamarneh, *Scientific Data* 2025):
11,394 clinical-atlas images with Fitzpatrick I–VI labels. CC BY-NC 4.0. Zenodo record 11101338:
```
https://zenodo.org/api/records/11101338/files/Fitzpatrick17k-C.csv/content
https://zenodo.org/api/records/11101338/files/Fitzpatrick17k_CategorizedAbbrvs.zip/content   (1.4 GB)
```

### DermaCon-IN: skin-tone audit
Madarkar et al., NeurIPS 2025 Datasets & Benchmarks. 5,450 clinical images of 3,002
patients from South Indian clinics, with Fitzpatrick (III–VI) and Monk labels. CC BY-NC-SA 4.0.
Harvard Dataverse, https://doi.org/10.7910/DVN/W7OUZM (`Skin_Metadata.tab`,
`DATASET_0.zip` + `DATASET_1.zip`, 3.6 GB). `scripts/preprocess_skintone_images.py`
prepares the audit images.

### SkinDisNet: external validation
Clinical smartphone photos from two Bangladesh hospitals (Sultana et al., *Data in Brief*
2025;63:112239). 1,710 real images in 6 classes (the synthetic `Augmented/` folder isn't
used). CC BY-NC 4.0, academic use only. Mendeley Data:
https://data.mendeley.com/datasets/yj3md44hxg. The page has no plain download link, but
the file list with direct URLs is at https://data.mendeley.com/public-api/datasets/yj3md44hxg.

### MegPaulson/Healthy_Skin: extra healthy skin
6,039 skin close-ups on Hugging Face, all labelled healthy. No licence is stated on the
card. The set is noisy, so `scripts/prepare_megpaulson_healthy.py` filters it:
https://huggingface.co/datasets/MegPaulson/Healthy_Skin

### Widiawaty et al.: paired photo + clinical text
Images and clinical questionnaire data for atopic dermatitis and non-AD patients from
Indonesia, supporting Widiawaty et al. 2025, "Multimodal Machine Learning Approach for
Diagnosing Atopic Dermatitis". CC BY 4.0. Figshare, https://doi.org/10.6084/m9.figshare.29925533.v4

### Eczema Infected + Normal: superseded
The first eczema-vs-normal set. Not used: its "Normal" class is stock photography, so a model
can tell the classes apart by photo style instead of by skin. Likely source (verify
before use): https://www.kaggle.com/datasets/adityush/eczema2

## Wearable sensor data

Details and findings for these are in `docs/SENSOR_DATA.md`.

### WESAD: stress
15 subjects, wrist and chest sensors (EDA, temperature, BVP, accelerometer) during a lab
stress protocol (Schmidt et al., ICMI 2018). No login, about 2.5 GB:
https://ubi29.informatik.uni-siegen.de/usi/data_wesad.html. That server is slow, and
Kaggle mirrors are faster (search "WESAD Wearable Stress and Affect Detection").

### AAUWSS: sleep
13 subjects, one night each, with a wrist Empatica E4 (same signals as WESAD) and
expert-scored sleep stages. CC BY 4.0, no login, about 6.1 GB:
https://zenodo.org/records/16919071. Only `empatica/` and `annotations/` are needed
(about 326 MB of the 14.6 GB unpacked).

### Southampton e-textile moisture sensor: eczema vs healthy skin
Todorov (2026), PhD thesis dataset, University of Southampton. CC BY.
https://doi.org/10.5258/SOTON/D3873 (https://eprints.soton.ac.uk/511063/). Only
`Chapter6_PatientTesting/PatientTest/` (13 patients, sensor vs Corneometer/TEWL) is used.

### Southampton wearable IDC sensor: AD patients
Todorov et al., "Wearable Sensor System for Monitoring Atopic Dermatitis in Patients",
*IEEE Sensors Journal* 2025, https://doi.org/10.1109/JSEN.2025.3601742. Patient data,
sensor characterisation, firmware and GUI from the printed interdigitated capacitive sensor
study. CC BY. https://doi.org/10.5258/SOTON/D3507 (https://eprints.soton.ac.uk/504106/)
