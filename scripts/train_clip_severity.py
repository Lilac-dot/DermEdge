"""
Stage 3 of the app: eczema severity (Mild / Moderate / Severe) from a photo.

Ridge regression from frozen CLIP ViT-B/16 image embeddings to the 0-12 EASI-signs score,
then the labels' own bands (Mild 0-2, Moderate 3-4, Severe 5+). Regressing the score beat a
3-band classifier (val: 53% exact band, within-one-band 95%) because neighbouring bands
are noisy and the score keeps their order. Same backbone stage 1 already runs, so the app
loads one large model, not two.

Labels: four EASI signs (erythema, papulation, excoriation, lichenification), each 0-3, for
every curated eczema image; bands from the 0-12 sum. severity_train.csv trains,
severity_val.csv picks alpha and is reported; severity_test.csv is not read.

Writes experiments/clip_severity_head/{head.npz, summary.json}.
"""
import json

import numpy as np
import pandas as pd
import torch
from scipy.stats import spearmanr
from sklearn.linear_model import Ridge
from sklearn.metrics import confusion_matrix

from finetune_clip_eczema import EMB_PATH, embed
from paths import ROOT

LABEL_DIR = ROOT / "dead_ends" / "eczema_severity_proxy_attempt" / "severity_labels"
OUT_DIR = ROOT / "experiments" / "clip_severity_head"
CLASSES = ["Mild", "Moderate", "Severe"]
BAND_EDGES = [2.5, 4.5]  # score < 2.5 Mild, < 4.5 Moderate, else Severe (labels: 0-2, 3-4, 5-9)


def band(score):
    return np.digitize(score, BAND_EDGES)


def embeddings(paths):
    from transformers import CLIPModel, CLIPProcessor
    cached = dict(np.load(EMB_PATH, allow_pickle=True)) if EMB_PATH.exists() else {}
    known = dict(zip(cached.get("paths", []), cached.get("X", [])))
    todo = [p for p in paths if p not in known]
    if todo:
        device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
        name = "openai/clip-vit-base-patch16"
        model = CLIPModel.from_pretrained(name).to(device).eval()
        proc = CLIPProcessor.from_pretrained(name)
        print(f"embedding {len(todo)} images")
        for p, f in zip(todo, embed(todo, model, proc, device)):
            known[p] = f
        np.savez(EMB_PATH, paths=np.array(list(known)), X=np.stack(list(known.values())))
    return np.stack([known[p] for p in paths]).astype(np.float32)


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    tr = pd.read_csv(LABEL_DIR / "severity_train.csv")
    va = pd.read_csv(LABEL_DIR / "severity_val.csv")
    Xtr, Xva = embeddings(list(tr["path"])), embeddings(list(va["path"]))
    ytr, yva = tr["easi_signs_sum"].values, va["easi_signs_sum"].values
    bva = va["severity_band"].map(CLASSES.index).values

    results = {}
    for alpha in (1.0, 3.0, 10.0, 30.0):
        reg = Ridge(alpha=alpha).fit(Xtr * 10, ytr)
        score = reg.predict(Xva * 10)
        results[alpha] = dict(mae=float(np.abs(score - yva).mean()),
                              spearman=float(spearmanr(score, yva)[0]),
                              band_acc=float((band(score) == bva).mean()),
                              within_one=float((np.abs(band(score) - bva) <= 1).mean()),
                              reg=reg, score=score)
        r = results[alpha]
        print(f"alpha={alpha}: MAE {r['mae']:.2f}, Spearman {r['spearman']:.3f}, "
              f"band accuracy {r['band_acc']:.3f}")
    alpha = min(results, key=lambda a: results[a]["mae"])
    r = results[alpha]
    np.savez(OUT_DIR / "head.npz", w=(r["reg"].coef_ * 10).astype(np.float32),
             b=np.float32(r["reg"].intercept_), band_edges=np.array(BAND_EDGES, np.float32),
             classes=np.array(CLASSES))
    summary = dict(labels="EASI-signs score 0-12",
                   n_train=len(tr), n_val=len(va), alpha=alpha,
                   val_mae_points=round(r["mae"], 3), val_spearman=round(r["spearman"], 3),
                   val_band_accuracy=round(r["band_acc"], 4),
                   val_band_within_one=round(r["within_one"], 4),
                   chance_band=round(float(np.bincount(bva).max() / len(bva)), 4),
                   val_band_confusion_rows_true_cols_pred=confusion_matrix(bva, band(r["score"])).tolist(),
                   classes=CLASSES)
    with open(OUT_DIR / "summary.json", "w") as f:
        json.dump(summary, f, indent=2)
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
