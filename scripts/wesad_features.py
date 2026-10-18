"""Shared window feature extraction for the WESAD LightGBM pipeline -- used by both
build_wesad_features.py (offline feature-table construction) and fusion_pipeline.py
(live inference), so the two can never drift out of sync with each other."""
import numpy as np

WINDOW_SEC = 60
FS = {"EDA": 4, "TEMP": 4, "BVP": 64, "ACC": 32}


def extract_window_features(sig_slices):
    """sig_slices: dict modality -> 1D (or Nx3 for ACC) array covering one window."""
    feats = {}

    for mod in ("EDA", "TEMP"):
        x = sig_slices[mod].astype(np.float64).ravel()
        feats[f"{mod}_mean"] = x.mean()
        feats[f"{mod}_std"] = x.std()
        feats[f"{mod}_min"] = x.min()
        feats[f"{mod}_max"] = x.max()
        feats[f"{mod}_range"] = x.max() - x.min()
        # linear trend across the window -- e.g. EDA tends to rise under stress,
        # peripheral TEMP tends to fall (vasoconstriction)
        t = np.arange(len(x))
        feats[f"{mod}_slope"] = np.polyfit(t, x, 1)[0] if len(x) > 1 else 0.0

    bvp = sig_slices["BVP"].astype(np.float64).ravel()
    feats["BVP_mean"] = bvp.mean()
    feats["BVP_std"] = bvp.std()
    feats["BVP_min"] = bvp.min()
    feats["BVP_max"] = bvp.max()
    # crude heart-rate proxy: dominant frequency in the plausible cardiac band (42-210 bpm)
    freqs = np.fft.rfftfreq(len(bvp), d=1.0 / FS["BVP"])
    spec = np.abs(np.fft.rfft(bvp - bvp.mean()))
    band = (freqs >= 0.7) & (freqs <= 3.5)
    if band.any() and spec[band].sum() > 0:
        feats["BVP_dominant_hz"] = freqs[band][np.argmax(spec[band])]
        feats["BVP_band_power"] = spec[band].sum()
    else:
        feats["BVP_dominant_hz"] = 0.0
        feats["BVP_band_power"] = 0.0

    acc = sig_slices["ACC"].astype(np.float64)
    mag = np.linalg.norm(acc, axis=1)
    feats["ACC_mag_mean"] = mag.mean()
    feats["ACC_mag_std"] = mag.std()
    for i, axis in enumerate("xyz"):
        feats[f"ACC_{axis}_mean"] = acc[:, i].mean()
        feats[f"ACC_{axis}_std"] = acc[:, i].std()

    return feats
