"""
Stage C: fuse Stage A (wearable stress) and Stage B (image severity) into one composite
score. This is DECISION-LEVEL (late) fusion, not a jointly-trained model -- deliberately,
because no dataset exists anywhere that has both wearable sensor data and skin images
from the same patients/sessions, so there is nothing to train a joint model on (see
the honors project plan discussion; same missing-data wall as the scratch-detection and
lesion-thermography ideas). This mirrors how real clinical severity indices like SCORAD/
EASI work: independently-assessed components combined by an explicit rule, not fit to a
giant labeled dataset.

Composite score = w_b * (Stage B image severity) + w_a * (Stage A stress score)

Both sub-scores are already probabilities in [0, 1] (sigmoid/softmax outputs), so no
further normalization is needed before combining. Weights default to equal (0.5/0.5) --
stated explicitly as a PROPOSED weighting, not one fit to data, since there's no ground
truth for the combined task to fit against.

This module exposes two independently-callable, independently-validated pieces:
  stage_b_predict(image_path)      -> Eczema probability, from the curated CNN
  stage_a_stress_predict(eda,temp,bvp,acc,baseline_profile) -> stress probability, from
    the WESAD LightGBM model
and a fuse() function that combines already-computed sub-scores.

Run this file directly for a small demo on real (but arbitrarily paired -- see caveat in
demo()) example data, to sanity-check the fused score behaves sensibly.
"""
import numpy as np
import lightgbm as lgb
import torch
import torch.nn as nn
from torchvision import transforms, models
from PIL import Image

from paths import ROOT
from wesad_features import extract_window_features
from wesad_calibration import compute_feature_baseline_profile, apply_feature_baseline

STAGE_B_MODEL_PATH = ROOT / "models" / "curated_resnet18_balanced.pt"
# LightGBM, not the CNN+attention model used previously -- after adding per-subject
# baseline-relative features, LightGBM's mean AUC rose to 0.9405 (from 0.871) with its
# per-fold decision threshold range collapsing from [0.01, 0.88] to [0.23, 0.66], while the
# same change left the CNN's mean AUC roughly flat (0.889) with WORSE threshold instability
# (range widened to [0.01, 0.97], plus a new AUC=0.0 fold). LightGBM is now both the more
# accurate and the far more stable Stage A model. See
# docs/wesad_stress_and_fusion_2026-08-27.md for the full comparison.
STAGE_A_MODEL_PATH = ROOT / "models" / "wesad_stress_lightgbm.txt"
IMG_SIZE = 224
DEVICE = torch.device("cpu")


# ---------------------------------------------------------------------------
# Stage B: image severity (Eczema probability)
# ---------------------------------------------------------------------------

_stage_b_model = None
_stage_b_transform = transforms.Compose([
    transforms.Resize((IMG_SIZE, IMG_SIZE)),
    transforms.ToTensor(),
    transforms.Normalize(mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225]),
])


def _load_stage_b():
    global _stage_b_model
    if _stage_b_model is None:
        model = models.resnet18(weights=None)
        model.fc = nn.Linear(model.fc.in_features, 2)
        model.load_state_dict(torch.load(STAGE_B_MODEL_PATH, map_location=DEVICE))
        model.eval()
        _stage_b_model = model
    return _stage_b_model


def stage_b_predict(image_path):
    """Returns P(Eczema) in [0,1] for one image."""
    model = _load_stage_b()
    img = Image.open(image_path).convert("RGB")
    x = _stage_b_transform(img).unsqueeze(0)
    with torch.no_grad():
        probs = torch.softmax(model(x), dim=1)
    return float(probs[0, 1])


# ---------------------------------------------------------------------------
# Stage A: wearable stress score, from the LightGBM model on personal-baseline-relative
# window features (see STAGE_A_MODEL_PATH note above for why LightGBM, not the CNN)
# ---------------------------------------------------------------------------

_stage_a_model = None


def _load_stage_a():
    global _stage_a_model
    if _stage_a_model is None:
        _stage_a_model = lgb.Booster(model_file=str(STAGE_A_MODEL_PATH))
    return _stage_a_model


def stage_a_stress_predict(eda, temp, bvp, acc, baseline_profile):
    """eda, temp: 4Hz window. bvp: 64Hz window. acc: (N,3) 32Hz window (any window length
    is fine -- extract_window_features works off actual array length, not a fixed shape;
    training used ~60s windows). Returns P(stress) in [0,1].

    baseline_profile: this person's OWN per-feature baseline average, from
    wesad_calibration.compute_feature_baseline_profile() applied to a handful of their own
    calm-period windows recorded before monitoring starts. The model was trained on
    features expressed relative to each subject's own baseline
    (see docs/wesad_stress_and_fusion_2026-08-27.md), so there is no fixed global
    normalization that would be valid for a new person -- this calibration step is
    required, not optional, and has to come from the wearer themselves, not from any of
    the WESAD training subjects."""
    model = _load_stage_a()
    sig_slices = {
        "EDA": np.asarray(eda), "TEMP": np.asarray(temp),
        "BVP": np.asarray(bvp), "ACC": np.asarray(acc),
    }
    feats = apply_feature_baseline(extract_window_features(sig_slices), baseline_profile)
    x = np.array([[feats[name] for name in model.feature_name()]])
    return float(model.predict(x)[0])


# ---------------------------------------------------------------------------
# Stage A-sleep: sleep-disruption score, from the AAUWSS LightGBM model on
# personal-baseline-relative window features -- same architecture as Stage A-stress,
# trained on the same E4 sensor set. "Baseline" here means the wearer's own ASLEEP-state
# average, not a calm-wake state, since sleep disruption is scored as deviation from a
# subject's own typical asleep signal -- see build_aauwss_raw_windows.py's docstring.
#
# NEGATIVE RESULT -- NOT USED IN THE DEPLOYED PIPELINE. LOSO-CV mean AUC is 0.4642
# (pooled AUC 0.3867, i.e. WORSE than chance), and two independent literature-standard
# actigraphy formulas (Cole-Kripke 1992, Sadeh 1994) applied to the same data also score
# at chance -- see docs/aauwss_sleep_model_results_2026-09-14.md for the full result and
# the diagnostic checks run to confirm this isn't an implementation bug. This function is
# kept callable for completeness/future reference only; no default code path in this
# module calls it, and trigger_index() should not be fed this model's output in
# production -- a below-chance signal is worse than no signal at all when combined via
# noisy-OR, since any elevated reading (real or not) raises trigger_index.
# ---------------------------------------------------------------------------

STAGE_A_SLEEP_MODEL_PATH = ROOT / "models" / "aauwss_sleep_lightgbm.txt"

_stage_a_sleep_model = None


def _load_stage_a_sleep():
    global _stage_a_sleep_model
    if _stage_a_sleep_model is None:
        _stage_a_sleep_model = lgb.Booster(model_file=str(STAGE_A_SLEEP_MODEL_PATH))
    return _stage_a_sleep_model


def stage_a_sleep_predict(eda, temp, bvp, acc, baseline_profile):
    """Same signature and calibration contract as stage_a_stress_predict(), but
    baseline_profile here must come from a handful of the wearer's own ASLEEP-period
    windows (not a calm-wake period) -- that is the reference state Stage A-sleep was
    trained against. Returns P(Wake / sleep-disrupted) in [0,1]."""
    model = _load_stage_a_sleep()
    sig_slices = {
        "EDA": np.asarray(eda), "TEMP": np.asarray(temp),
        "BVP": np.asarray(bvp), "ACC": np.asarray(acc),
    }
    feats = apply_feature_baseline(extract_window_features(sig_slices), baseline_profile)
    x = np.array([[feats[name] for name in model.feature_name()]])
    return float(model.predict(x)[0])


# ---------------------------------------------------------------------------
# Fusion
# ---------------------------------------------------------------------------

def fuse(image_score, stress_score, w_b=0.5, w_a=0.5):
    """Proposed, literature-motivated weighting (SCORAD/EASI-style additive composite),
    NOT fit to data -- no dataset exists with both modalities on the same patients to fit
    weights against. Equal weighting by default; change w_b/w_a to explore sensitivity.

    Kept for comparison -- see trigger_index()/flare_risk() below for the design that
    superseded this as the primary fusion rule (docs/stress_moderated_fusion_design_2026-09-14.md).
    A flat average lets a calm stress reading drag down a photo that clearly shows eczema,
    and lets a high stress reading inflate the score for a photo that shows nothing -- neither
    is clinically sensible, since stress is a documented flare TRIGGER, not an independent
    severity signal."""
    assert abs(w_b + w_a - 1.0) < 1e-6, "weights should sum to 1"
    return w_b * image_score + w_a * stress_score


def trigger_index(**trigger_scores):
    """Combine one or more independent trigger-signal scores (each already a probability
    in [0,1]) into one trigger_index via NOISY-OR: trigger_index = 1 - product(1 -
    score_i). In practice this is currently called with `stress` only -- Stage A-sleep
    was built and trained but scores at chance (see stage_a_sleep_predict()'s docstring
    and docs/aauwss_sleep_model_results_2026-09-14.md) and is deliberately not fed in
    here. The function still accepts multiple named scores with no interface change,
    should a working second trigger become available later. This is how
    multiple independent risk factors are conventionally combined in Bayesian
    medical-diagnosis networks (Pearl, 1988; e.g. QMR-DT), and fits this use case better
    than a plain average: if EITHER stress OR sleep disruption is high, trigger_index
    should rise toward that level, not get diluted toward the middle by a calmer second
    trigger. With exactly one trigger score, 1-(1-x) = x, so this is numerically
    identical to before until a second trigger (Stage A-sleep) is trained and passed in.
    Same status as fuse()'s weights: PROPOSED, not fit to data -- no dataset pairs
    multiple trigger signals with real flare outcomes to fit against. Pass named scores,
    e.g. trigger_index(stress=0.8, sleep=0.3). See
    docs/stress_moderated_fusion_design_2026-09-14.md for why a plain mean was replaced
    with this."""
    if not trigger_scores:
        raise ValueError("trigger_index needs at least one trigger score")
    complement_product = 1.0
    for v in trigger_scores.values():
        complement_product *= (1.0 - v)
    return 1.0 - complement_product


def flare_risk(image_score, trigger_idx, gamma=1.0):
    """Gated fusion: flare_risk = image_score * trigger_idx**gamma (Baron & Kenny 1986's
    'moderator' framing -- a trigger should scale a risk of worsening, not blend into a
    current-severity number). If Stage B says 'not eczema', image_score is low, so
    flare_risk stays near zero regardless of trigger levels: there's nothing to flare.

    gamma (default 1.0, i.e. unchanged behaviour: flare_risk = image_score * trigger_idx)
    is an optional softening exponent on trigger_idx, offered because Kittler et al.
    (1998) found pure product combination of scores more fragile to one noisy/
    miscalibrated input than blended alternatives. gamma < 1 pulls a low trigger_idx
    toward 1 (e.g. 0.01**0.5 = 0.1), so one unusually low trigger reading suppresses
    flare_risk less harshly. A SYMMETRIC geometric mean of image_score and trigger_idx
    was considered instead and deliberately rejected: it can push flare_risk ABOVE
    image_score whenever trigger_idx > image_score, silently breaking the 'flare_risk
    can never exceed image_score' gating property this design depends on. Softening only
    trigger_idx's own exponent (as done here) keeps that property intact for any
    gamma > 0, since trigger_idx**gamma <= 1 always. gamma is left at 1.0 by default
    since, like every other weight in this pipeline, there is no paired dataset to tune
    it against -- change it deliberately, not as a default improvement. Still a rule,
    not a trained model. See docs/stress_moderated_fusion_design_2026-09-14.md for the
    full reasoning and the literature this is based on."""
    assert gamma > 0, "gamma must be positive"
    return image_score * (trigger_idx ** gamma)


# ---------------------------------------------------------------------------
# Demo
# ---------------------------------------------------------------------------

def demo():
    """Runs the real Stage B model on real test images, and the real Stage A model on
    real held-out WESAD windows, then fuses each combination. IMPORTANT CAVEAT: the image
    and the wearable window are NOT from the same person or moment -- there is no dataset
    that provides that pairing (see module docstring). This demo only checks that the
    fusion architecture runs correctly and produces sensible, monotonic output given its
    two inputs; it is not a claim about any real patient's combined risk."""
    import csv

    test_manifest = ROOT / "SkinDisease" / "manifest_curated_v3_test.csv"
    with open(test_manifest, newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    eczema_img = next(r["path"] for r in rows if r["label"] == "1")
    other_img = next(r["path"] for r in rows if r["label"] == "0")

    npz = np.load(ROOT / "dataset" / "WESAD" / "wesad_raw_windows.npz", allow_pickle=True)
    subject = npz["subject"]
    label = npz["label"]
    condition = npz["condition"]
    # pick one real stress window and one real non-stress window (from the same subject,
    # arbitrarily S17, just so the two example windows are at least internally consistent)
    s = "S17"
    idx_stress = np.where((subject == s) & (label == 1))[0][0]
    idx_calm = np.where((subject == s) & (label == 0))[0][0]

    # This person's own calibration: features extracted from each of their own
    # baseline-condition windows, averaged into one profile -- standing in for the short
    # calm recording a real deployment would need from a new wearer. Using S17's own
    # baseline (not any other subject's, and not a pooled statistic) is what the model was
    # actually trained to expect -- see stage_a_stress_predict's docstring.
    baseline_idx = np.where((subject == s) & (condition == 1))[0]
    baseline_feats_list = [
        extract_window_features({
            "EDA": npz["EDA"][i].squeeze(-1), "TEMP": npz["TEMP"][i].squeeze(-1),
            "BVP": npz["BVP"][i].squeeze(-1), "ACC": npz["ACC"][i],
        })
        for i in baseline_idx
    ]
    baseline_profile = compute_feature_baseline_profile(baseline_feats_list)

    def stress_score(idx):
        eda = npz["EDA"][idx].squeeze(-1)
        temp = npz["TEMP"][idx].squeeze(-1)
        bvp = npz["BVP"][idx].squeeze(-1)
        acc = npz["ACC"][idx]
        return stage_a_stress_predict(eda, temp, bvp, acc, baseline_profile)

    img_score_eczema = stage_b_predict(eczema_img)
    img_score_other = stage_b_predict(other_img)
    stress_prob_high = stress_score(idx_stress)
    stress_prob_low = stress_score(idx_calm)

    print("Stage B (image) scores:")
    print(f"  Eczema image  -> P(Eczema) = {img_score_eczema:.3f}")
    print(f"  Other image   -> P(Eczema) = {img_score_other:.3f}")
    print(f"\nStage A (wearable) scores, subject {s} (calibrated against {len(baseline_idx)} of their own baseline windows):")
    print(f"  Known-stress window     -> P(stress) = {stress_prob_high:.3f}")
    print(f"  Known-not-stress window -> P(stress) = {stress_prob_low:.3f}")

    print("\nFused composite scores (illustrative pairings only -- see demo() docstring):")
    combos = [
        ("Eczema image + high stress", img_score_eczema, stress_prob_high),
        ("Eczema image + low stress", img_score_eczema, stress_prob_low),
        ("Other image + high stress", img_score_other, stress_prob_high),
        ("Other image + low stress", img_score_other, stress_prob_low),
    ]
    print(f"  {'':32s}    fuse() flat avg   flare_risk() gated")
    for name, img_s, stress_s in combos:
        flat = fuse(img_s, stress_s)
        gated = flare_risk(img_s, trigger_index(stress=stress_s))
        print(f"  {name:32s} -> {flat:15.3f}   {gated:15.3f}")
    print(
        "\nNote the gated column: flare_risk scales multiplicatively with the image score, "
        "so it never exceeds what Stage B itself reports for that image -- unlike fuse(), "
        "which can let a high stress reading pull the composite score above the image "
        "score alone even when Stage B's own read on the photo is weak. (The 'Other image' "
        "row above uses a known Stage B misclassification, left as-is per this project's "
        "no-cherry-picking practice -- see the module docstring.)"
    )

    print(
        "\ntrigger_index()'s noisy-OR vs. a plain mean, illustrated with a SYNTHETIC "
        "second trigger score (a worked example of the formula's shape only -- Stage "
        "A-sleep was trained but scores at chance and is deliberately not used as a real "
        "second trigger; see docs/aauwss_sleep_model_results_2026-09-14.md):"
    )
    demo_pairs = [
        ("high stress, calm sleep", stress_prob_high, 0.15),
        ("mild stress, disrupted sleep", 0.3, 0.85),
    ]
    print(f"  {'':32s}    mean (old)   noisy-OR (now)")
    for name, s_score, sleep_score in demo_pairs:
        mean_val = (s_score + sleep_score) / 2
        noisy_or_val = trigger_index(stress=s_score, sleep=sleep_score)
        print(f"  {name:32s} -> {mean_val:15.3f}   {noisy_or_val:15.3f}")
    print(
        "Note the second row: a plain mean of a low stress reading and a high sleep "
        "reading dilutes toward the middle (0.575); noisy-OR instead tracks the more "
        "elevated of the two triggers (0.895) -- the intended behaviour, since either "
        "trigger alone being high is reason for concern, not something a calmer second "
        "trigger should average away."
    )


if __name__ == "__main__":
    demo()
