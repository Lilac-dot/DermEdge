"""
Is DermLIP a better image encoder for DermEdge than the OpenAI CLIP the website runs today?

Every encoder gets the same simple head (logistic regression on its normalised image embedding),
trained and tested on the same SCIN phone photos with 5-fold cross-validation grouped by case,
so the comparison is about the encoder alone. Two tasks:
  rash     stage 1: any skin condition vs healthy skin (scin_rows + scin_healthy_rows)
  eczema   stage 2: eczema vs the other conditions, among the rash photos. The website's ResNet18
           stage-2 model (never trained on SCIN) is scored on the same photos for reference.
AUC differences come with a 95% paired bootstrap interval over cases.

Encoders: clip (openai/clip-vit-base-patch16), dermlip (redlessone/DermLIP_ViT-B-16),
dermlip_panderm (redlessone/DermLIP_PanDerm-base-w-PubMed-256; needs the Derm1M repo's open_clip,
cloned to ../tools/Derm1M). Embeddings are cached, one encoder per run:
    python scripts/compare_encoders.py --embed clip
    python scripts/compare_encoders.py --embed dermlip
    python scripts/compare_encoders.py --embed dermlip_panderm
    python scripts/compare_encoders.py --report
Writes experiments/encoder_comparison/{emb_<name>.npz, summary.json}.
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd
from PIL import Image

from paths import ROOT, DATASET_DIR

OUT = ROOT / "experiments" / "encoder_comparison"
SEED, N_FOLDS, CS = 42, 5, (0.1, 1.0, 10.0)
ENCODERS = ["clip", "dermlip", "dermlip_panderm"]


SKINDISNET_POS = {"AD", "EC"}  # atopic dermatitis, eczema
SKINDISNET_NEG = {"SC", "TC"}  # scabies, tinea corporis; contact (CD) and seborrhoeic (SD) dermatitis left out


def skindisnet_rows():
    """SkinDisNet (Bangladesh hospitals, smartphone photos, 2025): published after Derm1M was
    built, so neither DermLIP model can have seen it. Real photos only, not the Augmented/ copies."""
    base = DATASET_DIR / "SkinDisNet"
    out = []
    for p in sorted(base.rglob("*")):
        if p.suffix.lower() not in {".jpg", ".jpeg", ".png"} or "augment" in str(p).lower():
            continue
        cls = p.parent.name.strip().upper()
        if cls in SKINDISNET_POS | SKINDISNET_NEG:
            out.append(dict(path=str(p.relative_to(ROOT)), eczema=int(cls in SKINDISNET_POS), cls=cls))
    return pd.DataFrame(out)


def rows():
    rash = pd.read_csv(DATASET_DIR / "skintone" / "scin_rows.csv")
    healthy = pd.read_csv(DATASET_DIR / "healthy" / "scin_healthy_rows.csv")
    df = pd.concat([rash.assign(rash=1, eczema=rash["label"]), healthy.assign(rash=0, eczema=0)], ignore_index=True)
    df = df[df["path"].map(lambda p: (ROOT / p).exists())].drop_duplicates("path").reset_index(drop=True)
    return df[["path", "group_id", "rash", "eczema", "fst_group", "disease_class"]]


def load(paths):
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(8) as ex:
        return list(ex.map(lambda p: Image.open(ROOT / p).convert("RGB"), paths))


def embed(name, df, tag=""):
    import torch
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    if name == "clip":
        from transformers import CLIPImageProcessor, CLIPVisionModelWithProjection
        m = CLIPVisionModelWithProjection.from_pretrained("openai/clip-vit-base-patch16").to(dev).eval()
        proc = CLIPImageProcessor.from_pretrained("openai/clip-vit-base-patch16")
        enc = lambda ims: m(**proc(images=ims, return_tensors="pt").to(dev)).image_embeds
    else:
        if name == "dermlip_panderm":
            sys.path.insert(0, str(ROOT.parent / "tools" / "Derm1M" / "src"))
        import open_clip
        hub = {"dermlip": "hf-hub:redlessone/DermLIP_ViT-B-16",
               "dermlip_panderm": "hf-hub:redlessone/DermLIP_PanDerm-base-w-PubMed-256"}[name]
        m, _, pre = open_clip.create_model_and_transforms(hub)
        m = m.to(dev).eval()
        enc = lambda ims: m.encode_image(torch.stack([pre(i) for i in ims]).to(dev))
    out = []
    with torch.no_grad():
        for i in range(0, len(df), 64):
            f = enc(load(df["path"].iloc[i:i + 64].tolist())).float()
            out.append((f / f.norm(dim=-1, keepdim=True)).cpu().numpy())
            print(f"  {name}: {min(i + 64, len(df))}/{len(df)}", end="\r")
    print()
    OUT.mkdir(parents=True, exist_ok=True)
    np.savez(OUT / f"emb_{name}{tag}.npz", X=np.concatenate(out), path=df["path"].values)


def load_emb(name, df, tag=""):
    z = np.load(OUT / f"emb_{name}{tag}.npz", allow_pickle=True)
    pos = {p: i for i, p in enumerate(z["path"])}
    return z["X"][[pos[p] for p in df["path"]]]


def resnet_stage2(df):
    """The website's stage-2 ResNet18 (web/models/stage2.onnx), scored as engine.mjs runs it."""
    import onnxruntime as ort
    sess = ort.InferenceSession(str(ROOT / "web/models/stage2.onnx"), providers=["CPUExecutionProvider"])
    mean, std = np.array([0.485, 0.456, 0.406]), np.array([0.229, 0.224, 0.225])
    ps = []
    for p in df["path"]:
        x = (np.asarray(Image.open(ROOT / p).convert("RGB").resize((224, 224), Image.BILINEAR)) / 255 - mean) / std
        l0, l1 = sess.run(["logits"], {"image": x.transpose(2, 0, 1)[None].astype(np.float32)})[0][0]
        ps.append(1 / (1 + np.exp(l0 - l1)))
    return np.array(ps)


def oof(X, y, groups):
    """Out-of-fold probabilities for each C; logistic regression, classes weighted equally."""
    from sklearn.linear_model import LogisticRegression
    from sklearn.model_selection import StratifiedGroupKFold
    res = {}
    for C in CS:
        p = np.zeros(len(y))
        for tr, te in StratifiedGroupKFold(N_FOLDS, shuffle=True, random_state=SEED).split(X, y, groups):
            clf = LogisticRegression(C=C, class_weight="balanced", max_iter=5000).fit(X[tr], y[tr])
            p[te] = clf.predict_proba(X[te])[:, 1]
        res[C] = p
    return res


def metrics(y, p):
    from sklearn.metrics import roc_auc_score
    pred = p >= 0.5
    return dict(auc=round(float(roc_auc_score(y, p)), 4), caught=round(float(pred[y == 1].mean()), 4),
                cleared=round(float((~pred[y == 0]).mean()), 4),
                balanced_acc=round(float((pred[y == 1].mean() + (~pred[y == 0]).mean()) / 2), 4))


def paired_auc_diff(y, pa, pb, groups, n=2000):
    """95% bootstrap interval (resampling cases) of AUC(b) - AUC(a)."""
    from sklearn.metrics import roc_auc_score
    rng = np.random.default_rng(SEED)
    ug = np.unique(groups); idx = {g: np.where(groups == g)[0] for g in ug}
    d = []
    for _ in range(n):
        s = np.concatenate([idx[g] for g in rng.choice(ug, len(ug))])
        if 0 < y[s].sum() < len(s):
            d.append(roc_auc_score(y[s], pb[s]) - roc_auc_score(y[s], pa[s]))
    return [round(float(np.percentile(d, 2.5)), 4), round(float(np.percentile(d, 97.5)), 4)]


def report():
    df = rows()
    have = [e for e in ENCODERS if (OUT / f"emb_{e}.npz").exists()]
    emb = {e: load_emb(e, df) for e in have}
    summary = dict(test=f"{N_FOLDS}-fold CV on SCIN phone photos, grouped by case; same logistic head per encoder; "
                        f"C picked per encoder from {list(CS)} (same advantage for each)",
                   caveat="Derm1M (DermLIP's training data) includes 6,518 SCIN image-text pairs, so SCIN results "
                          "favour both DermLIP models; skindisnet_external is the fair test",
                   encoders=have)
    for task, sub in (("rash", df), ("eczema", df[df["rash"] == 1])):
        y, g = sub[task].values, sub["group_id"].values
        X = {e: emb[e][sub.index] for e in have}
        res, best = {}, {}
        for e in have:
            ps = oof(X[e], y, g)
            C = max(ps, key=lambda c: metrics(y, ps[c])["auc"])
            best[e] = ps[C]
            res[e] = dict(C=C, **metrics(y, ps[C]))
            if task == "rash":
                res[e]["by_fitzpatrick"] = {k: metrics(y[m], ps[C][m]) for k in ("I-II", "III-IV", "V-VI")
                                            for m in [(sub["fst_group"] == k).values] if 0 < y[m].sum() < m.sum()}
        if task == "eczema":
            p2 = resnet_stage2(sub)
            res["resnet18_stage2_website"] = metrics(y, p2)
            best["resnet18_stage2_website"] = p2
        if "clip" in best:
            res["auc_diff_vs_clip_95ci"] = {e: paired_auc_diff(y, best["clip"], best[e], g) for e in best if e != "clip"}
        summary[task] = dict(n_pos=int(y.sum()), n_neg=int(len(y) - y.sum()), n_cases=int(len(np.unique(g))), **res)
        if task == "eczema":
            chosen_C = {e: res[e]["C"] for e in have}

    ext = skindisnet_rows()
    have_ext = [e for e in have if (OUT / f"emb_{e}_skindisnet.npz").exists()]
    if len(ext) and have_ext:
        from sklearn.linear_model import LogisticRegression
        tr = df[df["rash"] == 1]
        y_tr, y = tr["eczema"].values, ext["eczema"].values
        res, best = {}, {}
        for e in have_ext:
            clf = LogisticRegression(C=chosen_C[e], class_weight="balanced", max_iter=5000).fit(emb[e][tr.index], y_tr)
            best[e] = clf.predict_proba(load_emb(e, ext, "_skindisnet"))[:, 1]
            res[e] = metrics(y, best[e])
        best["resnet18_stage2_website"] = resnet_stage2(ext)
        res["resnet18_stage2_website"] = metrics(y, best["resnet18_stage2_website"])
        groups = ext["path"].values  # one photo per patient is not recorded; each photo is its own group
        if "clip" in best:
            res["auc_diff_vs_clip_95ci"] = {e: paired_auc_diff(y, best["clip"], best[e], groups) for e in best if e != "clip"}
        summary["skindisnet_external"] = dict(
            test="eczema heads trained on all SCIN rash photos, tested on SkinDisNet (never seen by any model)",
            positives=sorted(SKINDISNET_POS), negatives=sorted(SKINDISNET_NEG),
            n_pos=int(y.sum()), n_neg=int(len(y) - y.sum()), **res)

        # Within SkinDisNet: can the encoder separate the classes once the head has seen this kind
        # of photo? 5-fold CV by photo (no patient IDs are published, so it may be optimistic).
        groups = np.arange(len(y))
        res, best = {}, {}
        for e in have_ext:
            ps = oof(load_emb(e, ext, "_skindisnet"), y, groups)
            C = max(ps, key=lambda c: metrics(y, ps[c])["auc"])
            best[e] = ps[C]
            res[e] = dict(C=C, **metrics(y, ps[C]),
                          right_by_class={c: round(float(((ps[C] >= 0.5) == (c in SKINDISNET_POS))[ext["cls"].values == c].mean()), 4)
                                          for c in sorted(SKINDISNET_POS | SKINDISNET_NEG)})
        if "clip" in best:
            res["auc_diff_vs_clip_95ci"] = {e: paired_auc_diff(y, best["clip"], best[e], groups) for e in best if e != "clip"}
        summary["skindisnet_within"] = dict(test="5-fold CV within SkinDisNet, by photo (no patient IDs), same head per encoder", **res)
    (OUT / "summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--embed", choices=ENCODERS)
    ap.add_argument("--report", action="store_true")
    a = ap.parse_args()
    if a.embed:
        if not (OUT / f"emb_{a.embed}.npz").exists():
            embed(a.embed, rows())
        if len(skindisnet_rows()):
            embed(a.embed, skindisnet_rows(), "_skindisnet")
    if a.report:
        report()
