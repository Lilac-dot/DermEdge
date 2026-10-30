# Finals-eve plan: skin model tiers and DermLIP

Pull `main` first. The phone loading fix is already live (commit 3d0f36f): phones run the model
on WebAssembly, load stage 2 after stage 1, retry a failed load, and keep error banners up 12 s.

## What we're building tonight

| Tier | Runs on | Model | Size | Status |
|---|---|---|---|---|
| Laptop | Browser, WebGPU | CLIP ViT-B/16 (or DermLIP if it wins) + ResNet18 stage 2 | ~90 + 45 MB | live |
| Phone | Browser, WebAssembly | MobileNetV3 trained to copy the CLIP embedding, so every existing head works on it | ~17 MB | **Shreeya, Colab** |
| ESP32-CAM | On the board | MobileNetV1-0.25 int8, rash vs no rash | 300 KB | exists, untested on a board; slide only |

The website picks the tier itself (phone or laptop, WebGPU or not), with a Settings switch to use
the full model on a strong phone.

DermLIP check: DermLIP ViT-B/16 and DermLIP PanDerm-base are both the same size as our CLIP. Each
gets the same heads trained on the same data and is compared on held-out photos for all three
stages and the skin-type check. Switch only if it ties or wins.

## Who does what

### Shreeya: phone model on Colab (about 1 hour, mostly waiting)
1. colab.research.google.com > New notebook > Runtime > Change runtime type > **T4 GPU**.
2. Run in one cell:
   ```
   !git clone https://github.com/Lilac-dot/DermEdge.git
   %cd DermEdge
   !pip -q install timm onnx onnxruntime transformers
   !python scripts/train_phone_student.py --download-scin --out /content/student
   ```
   It downloads about 10,000 SCIN photos (public, no login, ~5 min), trains for 25 epochs and
   prints `held-out cos` after each one (how closely it copies the laptop model; closer to 1 is better).
3. At the end it prints a summary. The numbers that matter are under `through_website_heads`:
   `rash_decision_agreement` (how often the phone model makes the same rash call as the laptop
   model; aim for 0.9+) and `severity_mean_abs_diff_points`.
4. Download `/content/student/phone_vision.onnx` and `/content/student/summary.json` (Files
   panel on the left > right-click > Download) and send both to Nitish.
5. If Colab disconnects, rerun the last line: photos already downloaded are kept for that session.

Then the PPT.

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
- Tier switching in `web/js/engine.mjs`; plug in Shreeya's `phone_vision.onnx`.
- Test on a real Android phone and an iPhone, then deploy.
- PPT with Shreeya.

## Rough timings
| Step | Time |
|---|---|
| Phone model training on Colab | ~1 h |
| DermLIP comparison, once the data is here | 1.5–2 h |
| Switching to DermLIP if it wins (browser export, checks, skin-type offsets) | +1.5 h |
| Tier switching + phone model in the website + phone tests | ~1.5 h |
