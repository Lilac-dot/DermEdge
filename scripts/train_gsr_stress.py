"""
Stress model for the DermEdge band's real sensor: skin conductance (GSR) only.

Why not the 4-channel LightGBM (models/wesad_stress_lightgbm.txt): the band has no PPG or
wrist-motion stream into the stress model, and on WESAD's meditation and amusement phases (never
used in its training) that model called 50% of windows stress -- it learned "later in the
session", not stress. This model is trained so that can't happen: calm windows from BEFORE and
AFTER the stress task are negatives, so time-in-session alone doesn't separate the classes.

Data: WESAD wrist E4 tables, extracted by wesad_wrist_summary.py (dataset/WESAD/WESAD.zip, with
its clock-offset fix). Never unpickles the S*.pkl files.
  positives  TSST (public speaking + mental arithmetic)
  negatives  Base, Medi 1, Fun, Medi 2
Windows: 30 s, as the band and hub use. Features: the six EDA window statistics
(wesad_features.py) relative to the wearer's first 3 calm windows (90 s), as the app calibrates;
those 3 windows are not scored. Relative features only, so the E4's and a Grove GSR's different
absolute scales cancel.
Model: standardised logistic regression (C=0.1, balanced classes).
Test: leave-one-person-out over the 15 participants, threshold 0.5.

Writes experiments/gsr_stress/{stress_gsr.json, summary.json}, and web/models/stress_gsr.json, which
the website (web/js/local-api.js) loads to score the band's live skin conductance.
"""
import json
import zipfile

import numpy as np
import pandas as pd
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import roc_auc_score
from sklearn.pipeline import make_pipeline
from sklearn.preprocessing import StandardScaler

import wesad_wrist_summary as ws
from paths import ROOT
from wesad_calibration import apply_feature_baseline, compute_feature_baseline_profile
from wesad_features import extract_window_features

OUT_DIR = ROOT / "experiments" / "gsr_stress"
WIN = 30
FS = {"EDA": 4, "TEMP": 4, "BVP": 64, "ACC": 32}
CALIB_WINDOWS = 3
PHASES = (("Base", 0), ("TSST", 1), ("Medi 1", 2), ("Fun", 2), ("Medi 2", 2))  # 2 = calm after the task
FEATURES = ["EDA_mean_rel", "EDA_std_rel", "EDA_min_rel", "EDA_max_rel", "EDA_range_rel", "EDA_slope_rel"]


def load_acc(path):
    raw = pd.read_csv(path, header=None)
    return float(raw.iloc[0, 0]), float(raw.iloc[1, 0]), raw.iloc[2:, :3].to_numpy(float)


def windows(z):
    rows = []
    for d in sorted(ws.OUT_DIR.iterdir(), key=lambda p: int(p.name[1:])):
        s = d.name
        sig = {k: ws.load_e4(d / "wrist_E4" / f"{k}.csv") for k in ("EDA", "TEMP", "BVP")}
        sig["ACC"] = load_acc(d / "wrist_E4" / "ACC.csv")
        chest0 = ws.chest_start(z, s)
        ph = ws.phases(d / f"{s}_quest.csv")
        feats = []
        for phase, label in PHASES:
            a, b = ph[phase]
            for w0 in np.arange(a, b - WIN + 1e-9, WIN):
                sl = {}
                for k, (t0, fs, x) in sig.items():
                    i = int(round((chest0 + w0 - t0) * fs))
                    sl[k] = x[i:i + int(WIN * fs)]
                if any(len(sl[k]) < WIN * FS[k] for k in sl) or (sl["TEMP"] > 45).any():
                    continue
                feats.append((label, extract_window_features(sl)))
        profile = compute_feature_baseline_profile([f for lab, f in feats if lab == 0][:CALIB_WINDOWS])
        n_base = 0
        for lab, f in feats:
            rows.append(dict(subject=s, label=lab, calib=lab == 0 and n_base < CALIB_WINDOWS,
                             **apply_feature_baseline(f, profile)))
            n_base += lab == 0
    df = pd.DataFrame(rows)
    return df[~df["calib"]].reset_index(drop=True)


def model():
    return make_pipeline(StandardScaler(), LogisticRegression(C=0.1, class_weight="balanced", max_iter=5000))


def main():
    z = zipfile.ZipFile(ws.ZIP_PATH)
    if not ws.OUT_DIR.exists():
        ws.extract(z)
    df = windows(z)
    y, lab = (df["label"] == 1).astype(int).values, df["label"].values
    p = np.zeros(len(df))
    for s in df["subject"].unique():
        te = (df["subject"] == s).values
        p[te] = model().fit(df.loc[~te, FEATURES].values, y[~te]).predict_proba(df.loc[te, FEATURES].values)[:, 1]
    pred = p >= 0.5
    bal = []
    for _, d in df.groupby("subject"):
        pp, yy = pred[d.index], y[d.index]
        bal.append((pp[yy == 1].mean() + (~pp[yy == 0]).mean()) / 2)
    summary = dict(
        test="leave-one-person-out, 15 WESAD participants, 30 s windows, calibrated on each person's first 90 s",
        n_windows=dict(stress=int(y.sum()), calm_before=int((lab == 0).sum()), calm_after=int((lab == 2).sum())),
        accuracy=round(float((pred == y).mean()), 3),
        majority_class_accuracy=round(float(1 - y.mean()), 3),
        balanced_accuracy=round(float((pred[y == 1].mean() + (~pred[y == 0]).mean()) / 2), 3),
        auc_pooled=round(float(roc_auc_score(y, p)), 3),
        stress_caught=round(float(pred[lab == 1].mean()), 3),
        calm_before_cleared=round(float((~pred[lab == 0]).mean()), 3),
        calm_after_cleared=round(float((~pred[lab == 2]).mean()), 3),
        people_below_0_7_balanced_accuracy=int(sum(b < 0.7 for b in bal)),
    )
    final = model().fit(df[FEATURES].values, y)
    sc, lr = final[0], final[1]
    export = dict(
        features=FEATURES, mean=sc.mean_.round(8).tolist(), scale=sc.scale_.round(8).tolist(),
        coef=lr.coef_[0].round(8).tolist(), intercept=round(float(lr.intercept_[0]), 8), threshold=0.5,
        window_s=WIN, calibration_windows=CALIB_WINDOWS, slope_units="microsiemens per sample at 4 Hz",
        validation=summary,
    )
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    (OUT_DIR / "stress_gsr.json").write_text(json.dumps(export, indent=2))
    (OUT_DIR / "summary.json").write_text(json.dumps(summary, indent=2))
    (ROOT / "web" / "models" / "stress_gsr.json").write_text(json.dumps(export, indent=2))
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
