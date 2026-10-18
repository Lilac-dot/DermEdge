"""
Fine-tunes the CLIP screen of the webcam demo / dashboard (webcam_eczema_demo.py, app/).

  --target rash    (default) normal skin vs ANY of the 8 conditions -- stage 1 of the app
  --target eczema  normal skin vs eczema only

What is trained: only a linear head on frozen CLIP ViT-B/16 image embeddings. It starts at
the zero-shot classifier (100 * (eczema_text - normal_text), bias -CLIP_MARGIN) and an L2
penalty pulls it back toward those weights, so the head can only adjust CLIP's judgement,
not forget it. Fine-tuning the whole backbone on ~90 real healthy-skin photos would
overfit.

Data (the positive class is eczema only for --target eczema, all 8 conditions for rash)
  eczema  SCIN eczema (skin-tone development split) + curated Stage B eczema (train+val)
  normal  SCIN healthy (download_scin_healthy.py, minus 3 visible-lesion cases)
          + Kaggle Normal 416px skin close-ups + MegPaulson least-rash-like 40%
          + own webcam captures (experiments/webcam_captures/{eczema,normal}) if any
  Frozen test splits (curated test, skintone manifest_test) are never read.

Evaluation: 5-fold CV over SCIN cases (grouped by case). Each fold trains on the other SCIN
folds + every non-SCIN image and is scored only on held-out SCIN eczema vs healthy, i.e.
real phone photos. Reported against zero-shot on the same folds.

Writes experiments/clip_<target>_head/{head.npz, summary.json} and a shared embedding cache
experiments/clip_eczema_head/embeddings.npz; head.npz is what the demo and app load.
"""
import json

import numpy as np
import pandas as pd
import torch
from PIL import Image
from sklearn.metrics import roc_auc_score
from sklearn.model_selection import StratifiedGroupKFold

from paths import ROOT, DATASET_DIR, SKINDISEASE_DIR
from webcam_eczema_demo import PROMPTS, CLIP_MARGIN

EMB_PATH = ROOT / "experiments" / "clip_eczema_head" / "embeddings.npz"
SEED = 42
N_FOLDS = 5
LESION_VISIBLE = {"SCIN_-6477152086839235973", "SCIN_1554139664304435205",
                  "SCIN_4898834119557476781"}


def build_rows(target):
    """Column `eczema` holds the positive label (eczema, or any rash for target rash)."""
    rows = []
    gate = pd.read_csv(ROOT / "experiments" / "healthy_gate" / "oof_predictions.csv")
    if target == "eczema":
        gate = gate[(gate["label"] == 0) | (gate["disease_class"] == "Eczema")]
    rows.append(pd.DataFrame(dict(path=gate["path_small"], eczema=(gate["label"] == 1).astype(int),
                                  source="scin", group=gate["group_id"], fst_group=gate["fst_group"])))

    cur = pd.concat([pd.read_csv(SKINDISEASE_DIR / f"manifest_curated_v3_{s}.csv")
                     for s in ("train", "val")], ignore_index=True)
    if target == "eczema":
        cur = cur[cur["disease_class"] == "Eczema"]
    rows.append(pd.DataFrame(dict(path=cur["path"], eczema=1, source="curated", group=cur["path"])))

    rash = pd.read_csv(ROOT / "experiments" / "esp32_rash_gate_mnv1_025" / "manifest.csv")
    ext = rash[(rash["label"] == 0) & rash["source"].isin(["megpaulson", "kaggle_normal"])]
    rows.append(pd.DataFrame(dict(path=ext["path"], eczema=0, source=ext["source"], group=ext["path"])))

    for label, folder in ((1, "eczema"), (0, "normal")):
        caps = sorted((ROOT / "experiments" / "webcam_captures" / folder).glob("*.jpg"))
        rel = [str(p.relative_to(ROOT)) for p in caps]
        rows.append(pd.DataFrame(dict(path=rel, eczema=label, source="webcam", group=rel)))
    return pd.concat(rows, ignore_index=True)


@torch.no_grad()
def embed(paths, model, proc, device):
    out = []
    for i in range(0, len(paths), 64):
        ims = [Image.open(ROOT / p).convert("RGB") for p in paths[i:i + 64]]
        f = model.get_image_features(**proc(images=ims, return_tensors="pt").to(device))
        out.append((f / f.norm(dim=-1, keepdim=True)).float().cpu().numpy())
    return np.concatenate(out)


@torch.no_grad()
def zero_shot_head(model, proc, device, positive):
    t = {}
    for key in ("normal", positive):
        e = model.get_text_features(**proc(text=PROMPTS[key], return_tensors="pt",
                                           padding=True).to(device))
        e = (e / e.norm(dim=-1, keepdim=True)).mean(0)
        t[key] = (e / e.norm()).float().cpu().numpy()
    return 100 * (t[positive] - t["normal"]), -CLIP_MARGIN


def fit_head(X, y, w_src, w0, b0, lam, epochs=300):
    """Logistic regression from (w0, b0) with an L2 pull back toward them. Each class weighs
    the same in total; within a class each source weighs the same."""
    X, y = torch.tensor(X), torch.tensor(y, dtype=torch.float32)
    sw = torch.tensor(w_src, dtype=torch.float32)
    w0t = torch.tensor(w0, dtype=torch.float32)
    w = w0t.clone().requires_grad_(True)
    b = torch.tensor(float(b0), requires_grad=True)
    opt = torch.optim.Adam([w, b], lr=0.05)
    for _ in range(epochs):
        logit = X @ w + b
        loss = (torch.nn.functional.binary_cross_entropy_with_logits(logit, y, reduction="none")
                * sw).sum() / sw.sum() + lam * ((w - w0t) ** 2).sum()
        opt.zero_grad()
        loss.backward()
        opt.step()
    return w.detach().numpy(), float(b.detach())


def sample_weights(df):
    w = pd.Series(1.0, index=df.index)
    for label, d in df.groupby("eczema"):
        per = d.groupby("source").size()
        w[d.index] = 1.0 / (d["source"].map(per) * len(per))
    return w.values


def rates(y, logit, thr=0.0):
    pred = logit >= thr
    return dict(auc=round(float(roc_auc_score(y, logit)), 4),
                positive_caught=round(float(pred[y == 1].mean()), 4),
                normal_cleared=round(float((~pred[y == 0]).mean()), 4))


def best_threshold(y, logit):
    qs = np.unique(np.quantile(logit, np.linspace(0.01, 0.99, 197)))
    return float(max(qs, key=lambda t: (logit[y == 1] >= t).mean() + (logit[y == 0] < t).mean()))


def main():
    import argparse
    from transformers import CLIPModel, CLIPProcessor
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="rash", choices=["rash", "eczema"])
    args = ap.parse_args()
    out_dir = ROOT / "experiments" / f"clip_{args.target}_head"
    out_dir.mkdir(parents=True, exist_ok=True)
    EMB_PATH.parent.mkdir(parents=True, exist_ok=True)
    device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    df = build_rows(args.target)
    print(df.groupby(["source", "eczema"]).size().to_string())

    emb_path = EMB_PATH
    cached = dict(np.load(emb_path, allow_pickle=True)) if emb_path.exists() else {}
    name = "openai/clip-vit-base-patch16"
    model = CLIPModel.from_pretrained(name).to(device).eval()
    proc = CLIPProcessor.from_pretrained(name)
    known = dict(zip(cached.get("paths", []), cached.get("X", [])))
    todo = [p for p in df["path"] if p not in known]
    if todo:
        print(f"embedding {len(todo)} images")
        for p, f in zip(todo, embed(todo, model, proc, device)):
            known[p] = f
        np.savez(emb_path, paths=np.array(list(known)), X=np.stack(list(known.values())))
    X = np.stack([known[p] for p in df["path"]]).astype(np.float32)
    y = df["eczema"].values
    w0, b0 = zero_shot_head(model, proc, device, args.target)

    scin = np.flatnonzero(df["source"].values == "scin")
    other = np.flatnonzero(df["source"].values != "scin")
    folds = StratifiedGroupKFold(N_FOLDS, shuffle=True, random_state=SEED)
    lams = [1e-2, 1e-3, 1e-4]
    oof = {lam: np.zeros(len(scin)) for lam in lams}
    zs = X[scin] @ w0 + b0
    for tr, te in folds.split(scin, y[scin], df["group"].values[scin]):
        train_idx = np.concatenate([scin[tr], other])
        sw = sample_weights(df.iloc[train_idx].reset_index(drop=True))
        for lam in lams:
            w, b = fit_head(X[train_idx], y[train_idx], sw, w0, b0, lam)
            oof[lam][te] = X[scin[te]] @ w + b

    ys = y[scin]
    summary = {"target": args.target,
               "test": f"5-fold CV, held-out SCIN {args.target} vs healthy phone photos, grouped by case",
               "n_scin": {args.target: int(ys.sum()), "healthy": int((ys == 0).sum())},
               "zero_shot": rates(ys, zs), "fine_tuned": {}}
    for lam in lams:
        thr = best_threshold(ys, oof[lam])
        summary["fine_tuned"][str(lam)] = dict(at_0=rates(ys, oof[lam]),
                                               at_best=rates(ys, oof[lam], thr), best_thr=round(thr, 3))
    best_lam = max(lams, key=lambda l: summary["fine_tuned"][str(l)]["at_best"]["auc"])
    summary["chosen_lambda"] = best_lam

    # Final head on everything; its threshold is the OOF point that best balances the two
    # hit rates, folded into the bias so the demo can keep using 0.5.
    # Skin-tone breakdown of the held-out predictions, at the threshold the app uses.
    thr = best_threshold(ys, oof[best_lam])
    fst = df["fst_group"].values[scin]
    summary["by_fitzpatrick"] = {}
    for g in ("I-II", "III-IV", "V-VI"):
        m = fst == g
        if m.any() and len(np.unique(ys[m])) == 2:
            summary["by_fitzpatrick"][g] = dict(rates(ys[m], oof[best_lam][m], thr),
                                                n_positive=int(ys[m].sum()), n_healthy=int((ys[m] == 0).sum()))
    # Per-skin-type threshold: a group whose rashes are caught clearly less often than skin
    # types I-IV's at the shared threshold gets its own threshold, set so it catches the same
    # share. The ranking (AUC) was similar across groups -- the gap is calibration. Chosen on
    # the same small held-out set (V-VI: 16 healthy images), so it is a demonstrated fix,
    # not a validated one. Stored as logit offsets the app adds for a self-reported type.
    l, light = oof[best_lam], np.isin(fst, ["I-II", "III-IV"])
    target = float((l[light & (ys == 1)] >= thr).mean())
    offsets, calibrated = {}, {}
    for g in ("I-II", "III-IV", "V-VI"):
        m = fst == g
        if not (m.any() and len(np.unique(ys[m])) == 2):
            continue
        g_thr = thr
        if (l[m & (ys == 1)] >= thr).mean() < target - 0.05:
            g_thr = float(np.quantile(l[m & (ys == 1)], 1 - target))
        offsets[g] = round(thr - g_thr, 3)
        calibrated[g] = rates(ys[m], l[m], g_thr)
    summary["fitzpatrick_offsets"] = offsets
    summary["by_fitzpatrick_calibrated"] = calibrated
    with open(out_dir / "fst_offsets.json", "w") as f:
        json.dump(offsets, f, indent=2)
    pd.DataFrame(dict(path=df["path"].values[scin], label=ys, fst_group=fst, oof_logit=oof[best_lam],
                      zero_shot_logit=zs)).to_csv(out_dir / "oof_predictions.csv", index=False)

    w, b = fit_head(X, y, sample_weights(df), w0, b0, best_lam)
    np.savez(out_dir / "head.npz", w=w.astype(np.float32), b=np.float32(b - thr))
    summary["final"] = dict(n_train=int(len(df)), threshold_folded_into_bias=round(thr, 3))
    with open(out_dir / "summary.json", "w") as f:
        json.dump(summary, f, indent=2)
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
