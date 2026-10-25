"""
Trains the band's scratch model on recordings from tools/record.py, tests it on the round it
didn't see, and (with --write) puts it in main/scratch_model.h:

    python3 tools/tune.py                 # evaluate: motion only vs vibration only vs both
    python3 tools/tune.py --write         # then train on everything and write the model

Defaults are what worked on the first session (2026-10-25, one wearer, 16 clips): motion
features only, threshold 0.7, an episode ends after 4 calm seconds. That found 4/4 scratch clips
with 0 false episodes in the 12 others, tested on the round it didn't see. Vibration added
nothing there: at 800 Hz the ADXL345's noise (14-22 mg) buries the skin vibration.

Features come from tools/replay.c, which runs the band's own main/scratch.c on each clip, so the
model sees exactly what the band computes. The first 3 s of every clip are dropped (the detector
needs 2 s to fill, and nobody starts on the word GO). Only summaries are written to the model
file: no recording leaves ~/DermEdge-recordings.
"""
import argparse
import subprocess
import tempfile
import warnings
from datetime import date
from pathlib import Path

import numpy as np
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import balanced_accuracy_score
from sklearn.preprocessing import StandardScaler

ROOT = Path(__file__).resolve().parents[1]
RECORDINGS = Path.home() / "DermEdge-recordings"
FEATURES = ["rms", "rhythm", "p2_5", "p5_20", "p20_50", "p50_80", "p80_140", "p140_250", "p250_400",
            "vib_rhythm", "vib_ratio"]  # main/scratch.h's F_* order
SETS = {"motion only": ["rms", "rhythm", "p2_5", "p5_20"],
        "vibration only": ["p80_140", "p140_250", "p250_400", "vib_rhythm", "vib_ratio"],
        "all features": FEATURES}
SKIP_S = 3.0
START, END = 2, 4  # scratchy seconds in a row that start / calm ones that end an episode
# numpy 2.0 on Apple's Accelerate warns about overflow in matrix products that come out fine
# (checked by hand against the probabilities); hide those, not real problems
warnings.filterwarnings("ignore", category=RuntimeWarning, module="sklearn")


def build_replay(tmp):
    exe = Path(tmp) / "replay"
    for sdk in [None, "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", "/Library/Developer/CommandLineTools/SDKs/MacOSX15.sdk"]:
        # the rule, always: features don't depend on the model, and the rule column stays the rule
        cmd = ["cc", "-O2", "-DSCRATCH_FORCE_RULE", "-o", str(exe), str(ROOT / "tools/replay.c"), str(ROOT / "main/scratch.c"), "-lm"]
        if sdk:
            if not Path(sdk).exists():
                continue
            cmd[1:1] = ["-isysroot", sdk]
        if subprocess.run(cmd, capture_output=True).returncode == 0:
            return exe
    raise SystemExit("couldn't build tools/replay.c")


def load(sessions, exe):
    rows = []
    for f in sorted(p for s in sessions for p in s.glob("*.npz")):
        z = np.load(f)
        out = subprocess.run([str(exe)], input=z["xyz"].astype("<i2").tobytes(), capture_output=True, check=True).stdout
        lines = out.decode().strip().splitlines()[1:]
        name, rnd = f.stem.rsplit("_", 1)
        for line in lines:
            v = [float(x) for x in line.split(",")]
            if v[0] < SKIP_S:
                continue
            rows.append(dict(clip=f"{f.parent.name}/{f.stem}", activity=name, round=int(rnd), y=int(z["scratch"]),
                             t=v[0], rule=int(v[2]), f=np.array(v[5:])))
    return rows


def episodes(flags):
    """Episodes the band would report from a clip's per-second decisions."""
    n, on, off, inside = 0, 0, 0, False
    for s in flags:
        if s:
            off, on = 0, on + 1
            if not inside and on >= START:
                inside, n = True, n + 1
        else:
            on = 0
            if inside:
                off += 1
                if off >= END:
                    inside, off = False, 0
    return n


def fit(X, y):
    sc = StandardScaler().fit(X)
    m = LogisticRegression(class_weight="balanced", C=0.5, max_iter=2000).fit(sc.transform(X), y)
    return sc, m


def evaluate(rows, cols):
    """Train on one round, test on the other, both ways. Returns test-set predictions."""
    idx = [FEATURES.index(c) for c in cols]
    pred = {}
    rounds = sorted({r["round"] for r in rows})
    for test in rounds:
        tr = [r for r in rows if r["round"] != test]
        te = [r for r in rows if r["round"] == test]
        sc, m = fit(np.array([r["f"][idx] for r in tr]), np.array([r["y"] for r in tr]))
        p = m.predict_proba(sc.transform(np.array([r["f"][idx] for r in te])))[:, 1]
        for r, pi in zip(te, p):
            pred[id(r)] = pi
    return pred


def report(rows, flags, title):
    y = np.array([r["y"] for r in rows])
    f = np.array([flags[id(r)] for r in rows]).astype(int)
    sens, spec = f[y == 1].mean(), 1 - f[y == 0].mean()
    print(f"\n{title}: balanced accuracy {balanced_accuracy_score(y, f):.2f} (scratch seconds caught {sens:.0%}, "
          f"other seconds left alone {spec:.0%})")
    acts = sorted({r["activity"] for r in rows}, key=lambda a: (-max(r["y"] for r in rows if r["activity"] == a), a))
    for a in acts:
        rs = [r for r in rows if r["activity"] == a]
        clips = sorted({r["clip"] for r in rs})
        eps = [episodes([flags[id(r)] for r in rows if r["clip"] == c]) for c in clips]
        frac = np.mean([flags[id(r)] for r in rs])
        kind = "scratch" if rs[0]["y"] else "not   "
        print(f"  {kind} {a:9s} flagged {frac:4.0%} of seconds, episodes per clip {eps}")


def write_model(rows, cols, threshold):
    idx = [FEATURES.index(c) for c in cols]
    X, y = np.array([r["f"][idx] for r in rows]), np.array([r["y"] for r in rows])
    sc, m = fit(X, y)
    # every feature gets a slot; unused ones get weight 0 (and mean 0, sd 1)
    mean, sd, w = np.zeros(len(FEATURES)), np.ones(len(FEATURES)), np.zeros(len(FEATURES))
    mean[idx], sd[idx], w[idx] = sc.mean_, sc.scale_, m.coef_[0]
    b = m.intercept_[0]
    lit = lambda x: (lambda t: t if any(c in t for c in ".en") else t + ".0")(f"{x:.6g}") + "f"  # C needs 0.0f, not 0f
    arr = lambda v: "{" + ", ".join(lit(x) for x in v) + "}"
    clips = sorted({r["clip"] for r in rows})
    text = f"""// The scratch model: logistic regression on main/scratch.h's features, standardised.
// Generated by tools/tune.py on {date.today()} from {len(clips)} clips ({len(rows)} one-second
// windows) of one wearer, board on the back of the hand; features: {', '.join(cols)}.
// Rerun it after recording more. Building with -DSCRATCH_FORCE_RULE uses the hand-set rule.
#pragma once

#ifndef SCRATCH_FORCE_RULE
#define SCRATCH_MODEL_TRAINED 1
#else
#define SCRATCH_MODEL_TRAINED 0
#endif
#define SCRATCH_MODEL_THRESHOLD {threshold:.3f}f
#define SCRATCH_MODEL_END_WINDOWS {END}
#define SCRATCH_MODEL_BIAS {lit(b)}
// order: {', '.join(FEATURES)}
static const float SCRATCH_MODEL_MEAN[] = {arr(mean)};
static const float SCRATCH_MODEL_SD[] = {arr(sd)};
static const float SCRATCH_MODEL_W[] = {arr(w)};
"""
    (ROOT / "main/scratch_model.h").write_text(text)
    print(f"\nwrote main/scratch_model.h ({', '.join(cols)}; threshold {threshold}); weights by size:")
    for i in np.argsort(-np.abs(w)):
        if w[i]:
            print(f"  {FEATURES[i]:10s} {w[i]:+.2f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sessions", nargs="*", help="session folders (default: all in ~/DermEdge-recordings)")
    ap.add_argument("--write", action="store_true", help="train on everything and write main/scratch_model.h")
    ap.add_argument("--features", choices=list(SETS), default="motion only")
    ap.add_argument("--threshold", type=float, default=0.7)
    ap.add_argument("--end", type=int, default=END, help="calm seconds that end an episode")
    a = ap.parse_args()
    globals()["END"] = a.end
    sessions = [Path(s) if Path(s).is_absolute() else RECORDINGS / s for s in a.sessions] or sorted(p for p in RECORDINGS.iterdir() if p.is_dir())
    with tempfile.TemporaryDirectory() as tmp:
        rows = load(sessions, build_replay(tmp))
    print(f"{len(rows)} one-second windows from {len({r['clip'] for r in rows})} clips, "
          f"{sum(r['y'] for r in rows)} of them scratching; trained on one round, tested on the other")
    report(rows, {id(r): r["rule"] for r in rows}, "Hand-set rule (what the band runs now)")
    for name, cols in SETS.items():
        p = evaluate(rows, cols)
        report(rows, {k: v >= a.threshold for k, v in p.items()}, f"Model, {name}")
    if a.write:
        write_model(rows, SETS[a.features], a.threshold)


if __name__ == "__main__":
    main()
