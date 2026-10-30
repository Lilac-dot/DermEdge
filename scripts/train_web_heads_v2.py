"""
Retrains the website's stage-1 and stage-2 heads on CLIP ViT-B/16 embeddings so they work on
South Asian clinic photos too, after scripts/compare_encoders.py found that:
  - the stage-2 ResNet18 is at chance on real phone photos (SCIN AUC 0.56, SkinDisNet 0.46),
  - the stage-1 rash head passes only 34% of SkinDisNet's rash photos (56% with the V-VI offset).

Stage 2 (new): logistic head on the CLIP embedding, eczema vs look-alikes, trained on SCIN rash
  photos + SkinDisNet (atopic dermatitis and eczema vs scabies and tinea). Replaces the ResNet18,
  so the website no longer downloads a second 45 MB model.
Stage 1 (updated): the current rash head, refit from its own weights with an L2 pull back toward
  them (as scripts/finetune_clip_eczema.py did), adding every SkinDisNet photo as a rash. The pull
  keeps what it learned from the curated, MegPaulson and webcam photos that aren't on this machine.

Both are scored with 5-fold cross-validation (SCIN grouped by case; SkinDisNet by photo, as it
publishes no patient IDs) and per source, so one source can't hide another's failures.
Writes experiments/web_heads_v2/summary.json and, with --write, web/models/heads.json.
"""
import argparse
import json

import numpy as np
import pandas as pd
import torch
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import roc_auc_score
from sklearn.model_selection import StratifiedGroupKFold

from compare_encoders import DATASET_DIR, OUT as EMB_DIR, SKINDISNET_NEG, SKINDISNET_POS, embed, load_emb, rows
from paths import ROOT

OUT = ROOT / "experiments" / "web_heads_v2"
HEADS = ROOT / "web" / "models" / "heads.json"
SEED, N_FOLDS = 42, 5
FST = ("I-II", "III-IV", "V-VI")


def skindisnet_all():
    base = DATASET_DIR / "SkinDisNet" / "Preprocessed"
    out = [dict(path=str(p.relative_to(ROOT)), cls=p.parent.name.upper())
           for p in sorted(base.rglob("*")) if p.suffix.lower() in {".jpg", ".jpeg", ".png"}]
    df = pd.DataFrame(out)
    df["eczema"] = df["cls"].map(lambda c: 1.0 if c in SKINDISNET_POS else 0.0 if c in SKINDISNET_NEG else np.nan)
    return df


def data():
    scin = rows().assign(source="scin", group=lambda d: d["group_id"])
    sdn = skindisnet_all()
    if not (EMB_DIR / "emb_clip_skindisnet_all.npz").exists():
        embed("clip", sdn, "_skindisnet_all")
    sdn = sdn.assign(source="skindisnet", rash=1, group=sdn["path"], fst_group=None)
    X = np.concatenate([load_emb("clip", scin), load_emb("clip", sdn, "_skindisnet_all")])
    return X, pd.concat([scin, sdn], ignore_index=True)


def source_weights(df, label):
    """Each class weighs the same in total; within a class, each source weighs the same."""
    w = pd.Series(1.0, index=df.index)
    for _, d in df.groupby(label):
        per = d.groupby("source").size()
        w[d.index] = 1.0 / (d["source"].map(per) * len(per))
    return (w / w.mean()).values


def rates(y, p, thr=0.5):
    pred = p >= thr
    r = dict(n_pos=int(y.sum()), n_neg=int(len(y) - y.sum()))
    if y.sum():
        r["caught"] = round(float(pred[y == 1].mean()), 4)
    if (y == 0).any():
        r["cleared"] = round(float((~pred[y == 0]).mean()), 4)
    if 0 < y.sum() < len(y):
        r["auc"] = round(float(roc_auc_score(y, p)), 4)
    return r


def folds(df, label):
    return StratifiedGroupKFold(N_FOLDS, shuffle=True, random_state=SEED).split(df, df[label].astype(int), df["group"])


def balanced_threshold(y, p, w):
    """The threshold with the best source-weighted balanced accuracy on out-of-fold scores."""
    qs = np.unique(np.quantile(p, np.linspace(0.02, 0.98, 97)))
    def score(t):
        pos, neg = y == 1, y == 0
        return (w[pos] * (p[pos] >= t)).sum() / w[pos].sum() + (w[neg] * (p[neg] < t)).sum() / w[neg].sum()
    return float(max(qs, key=score))


# ---------- stage 2: eczema vs look-alikes ----------
def stage2(X, df):
    d = df[df["eczema"].notna() & (df["rash"] == 1)].copy()
    d["eczema"] = d["eczema"].astype(int)
    Xd, y, w = X[d.index], d["eczema"].values, source_weights(d, "eczema")
    best = None
    for C in (0.3, 1.0, 3.0, 10.0):
        p = np.zeros(len(d))
        for tr, te in folds(d, "eczema"):
            p[te] = LogisticRegression(C=C, max_iter=5000).fit(Xd[tr], y[tr], sample_weight=w[tr]).predict_proba(Xd[te])[:, 1]
        auc = np.mean([roc_auc_score(y[m], p[m]) for m in [(d["source"] == s).values for s in ("scin", "skindisnet")]])
        if best is None or auc > best[0]:
            best = (auc, C, p)
    _, C, p = best
    thr = balanced_threshold(y, p, w)
    report = dict(C=C, threshold=round(thr, 4), mean_source_auc=round(best[0], 4),
                  by_source={s: rates(y[m], p[m], thr) for s in ("scin", "skindisnet")
                             for m in [(d["source"] == s).values]},
                  by_skindisnet_class={c: rates(y[m], p[m], thr) for c in sorted(SKINDISNET_POS | SKINDISNET_NEG)
                                       for m in [(d["cls"] == c).values]})
    # leave one source out, for the record
    loso = {}
    for held in ("scin", "skindisnet"):
        tr, te = (d["source"] != held).values, (d["source"] == held).values
        q = LogisticRegression(C=C, max_iter=5000).fit(Xd[tr], y[tr], sample_weight=source_weights(d[tr], "eczema")).predict_proba(Xd[te])[:, 1]
        loso[f"train_without_{held}"] = rates(y[te], q, thr)
    report["leave_one_source_out"] = loso
    clf = LogisticRegression(C=C, max_iter=5000).fit(Xd, y, sample_weight=w)
    logit_thr = float(np.log(thr / (1 - thr)))
    return dict(w=clf.coef_[0].tolist(), b=float(clf.intercept_[0]) - logit_thr), report


# ---------- stage 1: rash vs healthy ----------
def fit_from(X, y, sw, w0, b0, lam, epochs=400):
    X, y, sw = torch.tensor(X, dtype=torch.float32), torch.tensor(y, dtype=torch.float32), torch.tensor(sw, dtype=torch.float32)
    w0t = torch.tensor(w0, dtype=torch.float32)
    w, b = w0t.clone().requires_grad_(True), torch.tensor(float(b0), requires_grad=True)
    opt = torch.optim.Adam([w, b], lr=0.05)
    for _ in range(epochs):
        loss = (torch.nn.functional.binary_cross_entropy_with_logits(X @ w + b, y, reduction="none") * sw).sum() / sw.sum() \
               + lam * ((w - w0t) ** 2).sum()
        opt.zero_grad(); loss.backward(); opt.step()
    return w.detach().numpy(), float(b.detach())


def stage1_report(df, y, p, offsets=None):
    """caught/cleared per source and, on SCIN, per skin type (with optional per-group offsets in logits)."""
    if offsets:
        lg = np.log(p / (1 - p)) + df["fst_group"].map(offsets).fillna(0).values
        p = 1 / (1 + np.exp(-lg))
    scin = (df["source"] == "scin").values
    return dict(scin=rates(y[scin], p[scin]), skindisnet=rates(y[~scin], p[~scin]),
                scin_by_fitzpatrick={g: rates(y[m], p[m]) for g in FST for m in [scin & (df["fst_group"] == g).values]})


def stage1(X, df, heads):
    y, w = df["rash"].values, source_weights(df, "rash")
    v1 = np.load(ROOT / "experiments" / "clip_rash_head" / "head.npz")  # always the original head, so reruns don't stack
    w0, b0 = v1["w"].astype(np.float64), float(v1["b"])
    heads = dict(heads, rash=dict(heads["rash"], fst_offsets=json.loads((ROOT / "experiments" / "clip_rash_head" / "fst_offsets.json").read_text())))
    sig = lambda z: 1 / (1 + np.exp(-z))
    current = stage1_report(df, y, sig(X @ w0 + b0))  # in-sample on SCIN: it was trained on all of it
    current_cal = stage1_report(df, y, sig(X @ w0 + b0), heads["rash"]["fst_offsets"])
    results = {}
    for lam in (1e-3, 3e-4, 1e-4):
        p = np.zeros(len(df))
        for tr, te in folds(df, "rash"):
            wf, bf = fit_from(X[tr], y[tr], w[tr], w0, b0, lam)
            p[te] = sig(X[te] @ wf + bf)
        results[lam] = (p, stage1_report(df, y, p))
    # The strongest pull (closest to the current head) that passes 80% of SkinDisNet rashes and
    # still clears 80% of healthy SCIN photos. Weaker pulls pass more SkinDisNet rashes, but every
    # healthy example is from SCIN, so they risk learning "South Asian clinic photo = rash" and
    # flagging healthy brown skin, which this data can't measure.
    ok = [l for l in results if results[l][1]["scin"]["cleared"] >= 0.80 and results[l][1]["skindisnet"]["caught"] >= 0.80]
    lam = max(ok) if ok else max(results)
    p, rep = results[lam]
    # skin-type offsets: lift a group's rash sensitivity to the others' level, as before
    offsets = {g: 0.0 for g in FST}
    lg, scin = np.log(p / (1 - p)), (df["source"] == "scin").values
    target = np.mean([rep["scin_by_fitzpatrick"][g]["caught"] for g in ("I-II", "III-IV")])
    for g in ("V-VI",):
        m = scin & (df["fst_group"] == g).values & (y == 1)
        if rep["scin_by_fitzpatrick"][g]["caught"] < target - 0.03:
            offsets[g] = round(float(-np.quantile(lg[m], 1 - target)), 3)
    wf, bf = fit_from(X, y, w, w0, b0, lam)
    return dict(w=wf.tolist(), b=bf, fst_offsets=offsets), dict(
        chosen_l2_pull=lam, current_head_in_sample=current, current_head_in_sample_calibrated=current_cal,
        new_head_cv={str(l): r for l, (_, r) in results.items()},
        new_head_cv_calibrated=stage1_report(df, y, p, offsets), fst_offsets=offsets), rep, stage1_report(df, y, p, offsets)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true", help="update web/models/heads.json")
    a = ap.parse_args()
    X, df = data()
    heads = json.loads(HEADS.read_text())
    ecz, rep2 = stage2(X, df)
    rash, rep1, before, after = stage1(X, df, heads)
    summary = dict(stage2_eczema=rep2, stage1_rash=rep1)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))
    if a.write:
        heads["rash"] = rash
        heads["eczema"] = ecz
        # the keys the About tab's fairness table reads (before: one shared threshold; after: with offsets)
        ui = lambda r: dict(auc=r["auc"], positive_caught=r["caught"], normal_cleared=r["cleared"], n_positive=r["n_pos"], n_healthy=r["n_neg"])
        heads["fairness"] = dict(
            before={g: ui(before["scin_by_fitzpatrick"][g]) for g in FST},
            after={g: ui(after["scin_by_fitzpatrick"][g]) for g in FST},
            note="SCIN phone photos, 5-fold CV by case, rash head v2 (scripts/train_web_heads_v2.py)")
        HEADS.write_text(json.dumps(heads))
        print("wrote", HEADS)


if __name__ == "__main__":
    main()
