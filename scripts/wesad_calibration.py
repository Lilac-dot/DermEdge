"""Per-subject baseline calibration for the WESAD wearable stress models.

Physiological signals like EDA and skin temperature have large, genuine between-person
differences in absolute level -- some people simply run with higher resting EDA or a
cooler baseline temperature than others. The original normalization z-scored every
subject against one pooled group mean/std (computed fresh per LOSO fold, but still
shared across every subject in that fold), which leaves those between-person differences
sitting in the model's input. That is a real, evidenced contributor to the per-subject
threshold-transfer failures documented in docs/wesad_stress_and_fusion_2026-08-27.md (a
fold can have test AUC=1.0 -- the ranking is fine -- yet F1=0.00, because the specific
learned decision boundary lands in the wrong place for that person's absolute signal
scale).

Fix: normalize each subject's own windows against THEIR OWN baseline-condition (calm,
pre-stress-task) mean/std, not a pooled group statistic. This uses only that subject's
own unlabeled-for-training-purposes baseline recording -- no test-outcome information --
and mirrors how a real wearable would have to work anyway: calibrate briefly against the
wearer's own calm state, then monitor deviations from it.
"""
import numpy as np


def compute_subject_baseline_stats(EDA, TEMP, BVP, ACC, condition, subject, baseline_condition=1):
    """Per-subject (mean, std) for each modality, computed only from that subject's own
    baseline-condition windows. Used for the training subjects during LOSO-CV -- every
    subject (train, val, and test alike) is normalized against their OWN baseline, never
    another subject's, so this is not test-information leakage."""
    stats = {}
    for s in np.unique(subject):
        mask = (subject == s) & (condition == baseline_condition)
        if not mask.any():
            raise ValueError(f"No baseline-condition windows found for subject {s}")
        stats[s] = {
            "EDA": (float(EDA[mask].mean()), float(EDA[mask].std()) + 1e-8),
            "TEMP": (float(TEMP[mask].mean()), float(TEMP[mask].std()) + 1e-8),
            "BVP": (float(BVP[mask].mean()), float(BVP[mask].std()) + 1e-8),
            "ACC": (float(ACC[mask].mean()), float(ACC[mask].std()) + 1e-8),
        }
    return stats


def compute_baseline_stats_from_window(eda, temp, bvp, acc):
    """Same per-modality (mean, std) computation, but from a single new user's own short
    calm-period recording, for use at inference time -- a deployed device has no access to
    any of the 15 WESAD training subjects' stats, only whatever calibration recording the
    wearer themselves provides."""
    return {
        "EDA": (float(np.mean(eda)), float(np.std(eda)) + 1e-8),
        "TEMP": (float(np.mean(temp)), float(np.std(temp)) + 1e-8),
        "BVP": (float(np.mean(bvp)), float(np.std(bvp)) + 1e-8),
        "ACC": (float(np.mean(acc)), float(np.std(acc)) + 1e-8),
    }


def normalize_by_subject(arr, subject_ids, stats, modality):
    """Vectorized version of normalize_single for a batch spanning multiple subjects --
    each row is normalized using its OWN subject's stats, not a shared statistic."""
    out = np.empty(arr.shape, dtype=np.float32)
    for s in np.unique(subject_ids):
        mask = subject_ids == s
        mu, sd = stats[s][modality]
        out[mask] = (arr[mask] - mu) / sd
    return out


def normalize_single(arr, stats, modality):
    mu, sd = stats[modality]
    return (np.asarray(arr, dtype=np.float32) - mu) / sd


def compute_feature_baseline_profile(baseline_feats_list):
    """Average a list of per-window feature dicts (as returned by
    wesad_features.extract_window_features), one per baseline-condition window, into a
    single per-feature baseline profile -- the personal reference point every window's
    features get compared against. Mirrors build_wesad_features.py's
    add_personal_baseline_features(), which averages the same way per subject."""
    keys = baseline_feats_list[0].keys()
    return {k: float(np.mean([f[k] for f in baseline_feats_list])) for k in keys}


def apply_feature_baseline(feats, baseline_profile):
    """Return the original absolute features plus their `_rel` (baseline-relative)
    counterparts, matching build_wesad_features.py's column naming exactly so a LightGBM
    model trained on that CSV's columns can be fed this dict directly."""
    out = dict(feats)
    for k, v in feats.items():
        out[f"{k}_rel"] = v - baseline_profile[k]
    return out
