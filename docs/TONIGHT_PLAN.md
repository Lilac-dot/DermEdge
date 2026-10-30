# Finals-eve plan: skin model tiers and DermLIP

Pull `main` first. The phone loading fix is already live (commit 3d0f36f): phones run the model
on WebAssembly, load stage 2 after stage 1, retry a failed load, and keep error banners up 12 s.

## What we're building tonight

| Tier | Runs on | Model | Size | Status |
|---|---|---|---|---|
| Laptop | Browser, WebGPU | CLIP ViT-B/16 (or DermLIP if it wins) + ResNet18 stage 2 | ~90 + 45 MB | live |
| Phone | Browser, WebAssembly | the same CLIP model (loads on phones since the 31 Oct fix); a 17 MB copy is future scope | ~90 MB | live |
| ESP32-CAM | On the board | MobileNetV1-0.25 int8, rash vs no rash | 300 KB | exists, untested on a board; slide only |

The website picks the tier itself (phone or laptop, WebGPU or not), with a Settings switch to use
the full model on a strong phone.

DermLIP check: DermLIP ViT-B/16 and DermLIP PanDerm-base are both the same size as our CLIP. Each
gets the same heads trained on the same data and is compared on held-out photos for all three
stages and the skin-type check. Switch only if it ties or wins.

## Who does what

### Shreeya: the PPT
The phone model is **dropped for the finale**: the full model now loads on phones (WebAssembly),
so `scripts/train_phone_student.py` stays as future scope. Don't run the Colab steps.
Rebuild the Canva deck from [PITCH_DECK.md](PITCH_DECK.md) (rewritten 31 Oct: 10-minute timing,
demo video plan, new diagrams: the `v2_*.png` files in `docs/architecture/`).

### Tishya: sensors, plus one upload
The heads were trained on files that were never committed. Zip these from your DermEdge folder
and put them on Drive, even if the sensor work isn't finished:
- `SkinDisease/` (the images and the `manifest_curated_v3_*.csv` files)
- `Eczema/` (the eczema subtype archive; its download link was never recorded)
- `dead_ends/eczema_severity_proxy_attempt/severity_labels/` (the severity labels; needed for stage 3)
- `experiments/healthy_gate/oof_predictions.csv`
- `experiments/esp32_rash_gate_mnv1_025/manifest.csv`

If `SkinDisease/` is too big, the CSVs alone are enough; the images also come from Kaggle
(see `DATASETS.md`).

### Nitish (with Claude on the laptop)
- DermLIP vs CLIP comparison, then switch if it wins.
- Website: accept and chart the band's ECG / heart-rate and LM35 readings, needed for demo video part B.
- Test on a real Android phone and an iPhone, then deploy.
- PPT with Shreeya.

## Rough timings
| Step | Time |
|---|---|
| DermLIP comparison, once the data is here | 1.5–2 h |
| Switching to DermLIP if it wins (browser export, checks, skin-type offsets) | +1.5 h |
| Tier switching + phone model in the website + phone tests | ~1.5 h |
