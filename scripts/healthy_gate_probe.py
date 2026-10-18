"""
Stage 1 "healthy skin vs skin condition" gate: frozen foundation-model embeddings + a
linear probe, trained and tested on SCIN only.

Why this design: the only healthy-skin images with no source shortcut are the 61 SCIN
cases from download_scin_healthy.py -- far too few to fine-tune a CNN, but enough for a
logistic-regression probe on frozen features. Both classes come from SCIN (same users,
same phones), so the probe can't score well by recognising the source. The larger public
"healthy skin" set we found (Mendeley vitiligo, 10.17632/wbvnkmbxbf) turned out to be
11k Hands studio photos + watermarked stock portraits, i.e. the same failure as the
original "Normal" class (docs/PROJECT_HISTORY.md 1.1), so it isn't used.

Data
  healthy (label 0): dataset/healthy/scin_healthy_rows.csv, minus 3 cases whose images
      show a visible lesion despite the "no discernible pathology" grade (checked by eye).
  condition (label 1): every SCIN image in the skin-tone DEVELOPMENT split
      (dataset/skintone/manifest_{train,val}.csv) -- eczema and the 7 look-alikes.
      manifest_test.csv (frozen) is never read.
  Both classes: images more than 30% black (SCIN privacy masks) are dropped, and every
  image goes through the same 320-px JPEG cache (preprocess_skintone_images.shrink), so
  file format can't leak the label.

Feature sets, each scored with the same case-grouped 5-fold CV:
  shortcut   mean RGB, brightness, black fraction, width, height, aspect, shot type.
             If this scores well, the classes differ by something other than skin.
  dinov2_s   timm vit_small_patch14_dinov2 (22M params, Pi-sized), CLS embedding
  clip_b16   openai/clip-vit-base-patch16 image embedding; also zero-shot with prompts

Writes experiments/healthy_gate/{embeddings_<name>.npz, oof_predictions.csv, summary.json}
"""
import json

import numpy as np
import pandas as pd
import torch
from PIL import Image
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import roc_auc_score
from sklearn.model_selection import StratifiedGroupKFold
from sklearn.pipeline import make_pipeline
from sklearn.preprocessing import StandardScaler

from paths import ROOT, DATASET_DIR
from preprocess_skintone_images import shrink

OUT_DIR = ROOT / "experiments" / "healthy_gate"
CACHE_DIR = DATASET_DIR / "healthy" / "cache"
SEED = 42
N_FOLDS = 5
N_BOOT = 1000
MAX_BLACK = 0.3
LESION_VISIBLE = {"SCIN_-6477152086839235973", "SCIN_1554139664304435205",
                  "SCIN_4898834119557476781"}
ZERO_SHOT_PROMPTS = {
    0: ["a photo of healthy normal skin", "a close-up photo of clear skin with no rash"],
    1: ["a photo of a skin rash", "a close-up photo of a skin disease lesion"],
}


def black_fraction(path):
    a = np.asarray(Image.open(path).convert("L").resize((128, 128)))
    return float((a < 8).mean())


def load_data():
    healthy = pd.read_csv(DATASET_DIR / "healthy" / "scin_healthy_rows.csv")
    healthy = healthy[~healthy["group_id"].isin(LESION_VISIBLE)].copy()
    dsts = [CACHE_DIR / (p.replace("/", "__").rsplit(".", 1)[0] + ".jpg") for p in healthy["path"]]
    for p, d in zip(healthy["path"], dsts):
        shrink((ROOT / p, d))
    healthy["path_small"] = [str(d.relative_to(ROOT)) for d in dsts]

    dev = pd.concat([pd.read_csv(DATASET_DIR / "skintone" / "manifest_train.csv"),
                     pd.read_csv(DATASET_DIR / "skintone" / "manifest_val.csv")], ignore_index=True)
    cond = dev[dev["source"] == "SCIN"].copy()
    cond["label"] = 1

    cols = ["path_small", "group_id", "label", "disease_class", "fst_group", "shot_type"]
    df = pd.concat([healthy[cols], cond[cols]], ignore_index=True)
    df["black"] = [black_fraction(ROOT / p) for p in df["path_small"]]
    n0 = len(df)
    df = df[df["black"] <= MAX_BLACK].reset_index(drop=True)
    print(f"dropped {n0 - len(df)} mostly-masked images")
    return df


def shortcut_features(df):
    rows = []
    for p, black, shot in zip(df["path_small"], df["black"], df["shot_type"]):
        im = Image.open(ROOT / p).convert("RGB")
        a = np.asarray(im, dtype=np.float32)
        w, h = im.size
        rows.append([*a.reshape(-1, 3).mean(0), a.mean(), black, w, h, w / h,
                     shot == "CLOSE_UP", shot == "AT_AN_ANGLE", shot == "AT_DISTANCE"])
    return np.array(rows, dtype=np.float32)


@torch.no_grad()
def dinov2_features(df, device):
    import timm
    model = timm.create_model("vit_small_patch14_dinov2.lvd142m", pretrained=True,
                              num_classes=0, img_size=224).to(device).eval()
    cfg = timm.data.resolve_data_config({}, model=model)
    cfg["input_size"] = (3, 224, 224)
    tf = timm.data.create_transform(**cfg)
    feats = []
    for i in range(0, len(df), 64):
        x = torch.stack([tf(Image.open(ROOT / p).convert("RGB"))
                         for p in df["path_small"].iloc[i:i + 64]]).to(device)
        feats.append(model(x).float().cpu().numpy())
    return np.concatenate(feats)


@torch.no_grad()
def clip_features(df, device):
    """Returns (L2-normalised image embeddings, zero-shot P(condition))."""
    from transformers import CLIPModel, CLIPProcessor
    name = "openai/clip-vit-base-patch16"
    model = CLIPModel.from_pretrained(name).to(device).eval()
    proc = CLIPProcessor.from_pretrained(name)
    text = {}
    for label, prompts in ZERO_SHOT_PROMPTS.items():
        t = model.get_text_features(**proc(text=prompts, return_tensors="pt", padding=True).to(device))
        t = t / t.norm(dim=-1, keepdim=True)
        text[label] = t.mean(0) / t.mean(0).norm()
    feats = []
    for i in range(0, len(df), 64):
        ims = [Image.open(ROOT / p).convert("RGB") for p in df["path_small"].iloc[i:i + 64]]
        f = model.get_image_features(**proc(images=ims, return_tensors="pt").to(device))
        feats.append((f / f.norm(dim=-1, keepdim=True)).float().cpu().numpy())
    feats = np.concatenate(feats)
    logits = 100 * feats @ torch.stack([text[0], text[1]]).cpu().numpy().T
    zero_shot = torch.softmax(torch.from_numpy(logits), dim=1)[:, 1].numpy()
    return feats, zero_shot


def cached(name, fn):
    path = OUT_DIR / f"embeddings_{name}.npz"
    if path.exists():
        z = np.load(path)
        return [z[k] for k in sorted(z.files)]
    out = fn()
    out = out if isinstance(out, tuple) else (out,)
    np.savez(path, **{f"a{i}": a for i, a in enumerate(out)})
    return list(out)


def oof_probe(X, df):
    """Out-of-fold P(condition) from a class-balanced logistic regression; folds keep every
    case on one side and are stratified on label x Fitzpatrick group."""
    strata = df["label"].astype(str) + "|" + df["fst_group"].fillna("unk")
    folds = StratifiedGroupKFold(N_FOLDS, shuffle=True, random_state=SEED)
    oof = np.zeros(len(df))
    for tr, te in folds.split(X, strata, df["group_id"]):
        clf = make_pipeline(StandardScaler(), LogisticRegression(
            C=0.1, class_weight="balanced", max_iter=5000))
        clf.fit(X[tr], df["label"].values[tr])
        oof[te] = clf.predict_proba(X[te])[:, 1]
    return oof


def boot_auc(y, p, groups, seed=SEED):
    """AUC with a 95% CI from bootstrapping cases, not images."""
    uniq = np.unique(groups)
    idx_by_group = {g: np.flatnonzero(groups == g) for g in uniq}
    rng = np.random.RandomState(seed)
    boots = []
    for _ in range(N_BOOT):
        idx = np.concatenate([idx_by_group[g] for g in rng.choice(uniq, len(uniq))])
        if len(np.unique(y[idx])) == 2:
            boots.append(roc_auc_score(y[idx], p[idx]))
    return dict(auc=round(float(roc_auc_score(y, p)), 4),
                ci_low=round(float(np.percentile(boots, 2.5)), 4),
                ci_high=round(float(np.percentile(boots, 97.5)), 4))


def operating_point(y, p, target_specificity=0.9):
    """Sensitivity / specificity at the threshold giving ~90% specificity on healthy skin
    (a gate that rarely sends healthy skin on to the eczema model), and at 0.5."""
    thr = float(np.quantile(p[y == 0], target_specificity))
    out = {}
    for name, t in [("spec90", thr), ("p0.5", 0.5)]:
        pred = p >= t
        out[name] = dict(threshold=round(t, 4),
                         sensitivity=round(float(pred[y == 1].mean()), 4),
                         specificity=round(float((~pred[y == 0]).mean()), 4))
    return out


def evaluate(df, p):
    y, g = df["label"].values, df["group_id"].values
    ecz = (df["label"] == 0) | (df["disease_class"] == "Eczema")
    res = dict(all=boot_auc(y, p, g), eczema_vs_healthy=boot_auc(y[ecz], p[ecz], g[ecz]),
               operating=operating_point(y, p), by_fst={})
    for fg, d in df.dropna(subset=["fst_group"]).groupby("fst_group"):
        if d["label"].nunique() == 2:
            res["by_fst"][fg] = boot_auc(d["label"].values, p[d.index], d["group_id"].values)
    return res


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    df = load_data()
    print(df.groupby("label").agg(images=("group_id", "size"), cases=("group_id", "nunique")))

    (X_short,) = cached("shortcut", lambda: shortcut_features(df))
    (X_dino,) = cached("dinov2_s", lambda: dinov2_features(df, device))
    X_clip, zs = cached("clip_b16", lambda: clip_features(df, device))

    preds = {"shortcut": oof_probe(X_short, df), "dinov2_s": oof_probe(X_dino, df),
             "clip_b16": oof_probe(X_clip, df), "clip_b16_zero_shot": zs}
    summary = {"n_images": {"healthy": int((df.label == 0).sum()), "condition": int(df.label.sum())},
               "n_cases": {"healthy": int(df[df.label == 0].group_id.nunique()),
                           "condition": int(df[df.label == 1].group_id.nunique())},
               "frozen_test_used": False, "results": {}}
    for name, p in preds.items():
        summary["results"][name] = evaluate(df, p)
        df[f"prob_{name}"] = p
        r = summary["results"][name]
        print(f"{name:20s} AUC {r['all']['auc']:.3f} [{r['all']['ci_low']:.3f}, {r['all']['ci_high']:.3f}]"
              f"  eczema-vs-healthy {r['eczema_vs_healthy']['auc']:.3f}  @spec90 sens "
              f"{r['operating']['spec90']['sensitivity']:.3f}")
    df.to_csv(OUT_DIR / "oof_predictions.csv", index=False)
    with open(OUT_DIR / "summary.json", "w") as f:
        json.dump(summary, f, indent=2)


if __name__ == "__main__":
    main()
