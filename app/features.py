"""
Product features around the SkinCheck models: photo quality gate, body-site before/after,
flare alerts with tips, and sample history for demos.
"""
import random
import time
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.ndimage import laplace

ROOT = Path(__file__).resolve().parent.parent
SAMPLE_PHOTOS = Path(__file__).resolve().parent / "sample_photos"

# Sharpness = variance of the Laplacian on a 256-px greyscale copy. Measured on real photos:
# 95% of SCIN phone photos score >= 26 (curated atlas photos >= 128), while the same photos
# with a light Gaussian blur score <= 8. Brightness is the mean grey level (0-255); 90% of
# real photos fall between 62 and 161.
BLUR_MIN = 15.0
DARK_MAX = 40.0
BRIGHT_MIN = 225.0

BODY_SITES = ["face", "neck", "inner_elbow", "hand", "wrist", "back_of_knee", "leg", "foot", "torso", "other"]


def quality(img):
    """-> {ok, issues: [key...], sharpness, brightness}."""
    g = img.convert("L")
    g.thumbnail((256, 256))
    a = np.asarray(g, dtype=np.float32)
    sharpness, brightness = float(laplace(a).var()), float(a.mean())
    issues = []
    if brightness < DARK_MAX:
        issues.append("too_dark")  # darkness also lowers sharpness, so don't call it blurry too
    elif brightness > BRIGHT_MIN:
        issues.append("too_bright")
    elif sharpness < BLUR_MIN:
        issues.append("blurry")
    return dict(ok=not issues, issues=issues, sharpness=round(sharpness, 1), brightness=round(brightness, 1))


# --- flare alerts ---------------------------------------------------------------------
# One tip per trigger, from standard atopic-dermatitis self-care advice (NICE CG57 /
# AAD patient guidance): emollients, avoiding irritants, cooling, sleep and stress care.
STRESS_TIP = "Stress is up. Take a 5-minute breathing break; stress often makes itch worse."
TIPS = {
    "stress (GSR wristband)": STRESS_TIP,
    "stress (journal)": STRESS_TIP,
    "dry skin (moisture patch)": "Your eczema patch is drier than healthy skin. Apply moisturiser now and after bathing.",
    "night scratching (IMU, simulated)": "Scratching was detected overnight. Keep nails short and consider cotton gloves at night.",
    "poor sleep": "Short sleep. Aim for a cool bedroom and a regular bedtime tonight.",
    "new soap / product": "You used a new product. If the skin reacts, stop using it and note it for your doctor.",
}
ALERT_HIGH, ALERT_MODERATE = 0.6, 0.3


def alerts(triggers, trigger_index):
    """-> {level: high|moderate|low, index, triggers: [...], tips: [text...]} for the strongest triggers."""
    level = "high" if trigger_index >= ALERT_HIGH else "moderate" if trigger_index >= ALERT_MODERATE else "low"
    ranked = sorted(((v, k) for k, v in triggers.items() if v >= 0.25), reverse=True)
    tips = []
    for _, k in ranked:
        tip = TIPS.get(k)
        if tip and tip not in tips:
            tips.append(tip)
    return dict(level=level, index=round(trigger_index, 2), triggers=[k for _, k in ranked][:3], tips=tips[:3])


# --- body-site comparison --------------------------------------------------------------
def compare_with_previous(history, site, result):
    """The last earlier eczema result at the same body site, and the change in severity."""
    if not site or not result.get("severity"):
        return None
    prev = [h for h in history if h.get("site") == site and h.get("severity")]
    if not prev:
        return None
    p = prev[-1]
    delta = round(result["severity"]["score"] - p["severity"]["score"], 1)
    return dict(prev_score=p["severity"]["score"], prev_band=p["severity"]["band"], prev_ts=p["ts"],
                prev_image=p["image"], delta=delta,
                trend="better" if delta <= -0.5 else "worse" if delta >= 0.5 else "same")


def site_summary(history):
    """Latest and first eczema severity per body site."""
    out = {}
    for h in history:
        if h.get("site") and h.get("severity"):
            s = out.setdefault(h["site"], dict(site=h["site"], first=h["severity"]["score"], first_ts=h["ts"], checks=0))
            s.update(latest=h["severity"]["score"], latest_ts=h["ts"], latest_image=h["image"])
            s["checks"] += 1
    return list(out.values())


# --- sample history --------------------------------------------------------------------
def sample_history(pipeline, n_days=28, seed=7):
    """Four weeks of example check-ins for demos, built from real eczema photos run through the
    real models; dates, sites and triggers are synthetic. Every entry is marked sample=True and
    the dashboard labels it.

    Photos come from app/sample_photos/ (any .jpg/.png you add) or, failing that, the curated
    eczema validation split if the research dataset is present. Raises FileNotFoundError if
    neither exists."""
    from pipeline import flare_risk
    rng = random.Random(seed)
    paths = sorted(p for p in SAMPLE_PHOTOS.glob("*") if p.suffix.lower() in (".jpg", ".jpeg", ".png"))
    manifest = ROOT / "SkinDisease" / "manifest_curated_v3_val.csv"
    if not paths and manifest.exists():
        import pandas as pd
        cur = pd.read_csv(manifest)
        paths = [ROOT / p for p in cur[cur["disease_class"] == "Eczema"]["path"]]
    if not paths:
        raise FileNotFoundError("No sample photos: add a few eczema photos to app/sample_photos/")
    rng.shuffle(paths)
    scored = []
    for p in paths[:40]:
        img = Image.open(p).convert("RGB")
        res, heat = pipeline.analyze(img, {}, {}, None)
        if res.get("severity"):
            scored.append((res, heat, img))
        if len(scored) >= 12:
            break
    # A plausible arc for the story: flaring early, improving with treatment.
    scored.sort(key=lambda r: -r[0]["severity"]["score"])
    now = time.time()
    sites = ["inner_elbow", "hand", "back_of_knee"]
    entries = []
    for i, (res, heat, img) in enumerate(scored):
        day = n_days - int(i * n_days / len(scored))
        ts = now - day * 86400 + rng.randint(8, 20) * 3600
        stressed = i < len(scored) / 2
        trig = {"stress (journal)": round(rng.uniform(0.5, 0.8) if stressed else rng.uniform(0.0, 0.3), 2),
                "poor sleep": round(rng.uniform(0.5, 0.9) if stressed else rng.uniform(0.0, 0.3), 2),
                "dry skin (moisture patch)": round(rng.uniform(0.4, 1.0) if i % 3 else rng.uniform(0.0, 0.3), 2)}
        p_ecz = res["p_eczema"] or 0.0
        res = dict(res, triggers=trig, site=sites[i % len(sites)], sample=True,
                   flare=flare_risk(p_ecz * res["severity"]["score"] / 12, trig))
        entries.append((ts, res, heat, img))
    return entries
