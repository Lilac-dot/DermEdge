"""
Downloads every SCIN image (up to 3 per case) for cases whose top-weighted condition maps
onto the Stage B task (scripts/skintone_labels.py SCIN_MAP), for the skin-tone
compression audit. Unlike build_scin_manifest.py (image_1 only, 488+488 sample, for
zero-shot external validation), this keeps all images of every relevant case: training
now happens on SCIN, and all images of a case are kept on the same side of the
case-level split later, so extra same-case images add data without adding leakage.

Cases where two conditions tie for the top weight are dropped (no single label).

Writes:
  dataset/SCIN/images_all/<case_id>_<n>.png
  dataset/skintone/scin_rows.csv  (one row per downloaded image, metadata attached)
"""
import ast
from concurrent.futures import ThreadPoolExecutor
import statistics
import urllib.request

import pandas as pd

from paths import SCIN_DIR, DATASET_DIR
from skintone_labels import SCIN_MAP, fst_group

BUCKET = "https://storage.googleapis.com/dx-scin-public-data/"
OUT_DIR = SCIN_DIR / "images_all"
ROWS_PATH = DATASET_DIR / "skintone" / "scin_rows.csv"


def parse_top(s):
    """Returns (top_condition, top_weight), or (None, None) when missing or tied."""
    try:
        d = ast.literal_eval(s)
    except Exception:
        return None, None
    if not d:
        return None, None
    ranked = sorted(d.items(), key=lambda kv: kv[1], reverse=True)
    if len(ranked) > 1 and ranked[0][1] == ranked[1][1]:
        return None, None
    return ranked[0]


def derm_fst(row):
    """Median of the (up to 3) dermatologist Fitzpatrick labels, ignoring non-gradable."""
    vals = []
    for i in (1, 2, 3):
        v = row.get(f"dermatologist_fitzpatrick_skin_type_label_{i}")
        if isinstance(v, str) and v.startswith("FST"):
            vals.append(int(v[3:]))
    return int(round(statistics.median(vals))) if vals else None


def self_fst(v):
    return int(v[3:]) if isinstance(v, str) and v.startswith("FST") else None


def download_one(job):
    url_path, out_path = job
    if out_path.exists():
        return True
    try:
        with urllib.request.urlopen(BUCKET + url_path, timeout=60) as resp:
            out_path.write_bytes(resp.read())
        return True
    except Exception as e:
        print(f"  FAILED {out_path.name}: {e}")
        return False


def main():
    cases = pd.read_csv(SCIN_DIR / "scin_cases.csv", dtype={"case_id": str})
    labels = pd.read_csv(SCIN_DIR / "scin_labels.csv", dtype={"case_id": str})
    df = cases.merge(labels, on="case_id")
    df[["top_condition", "top_weight"]] = df["weighted_skin_condition_label"].apply(
        lambda s: pd.Series(parse_top(s)))
    df["disease_class"] = df["top_condition"].map(SCIN_MAP)
    df = df.dropna(subset=["disease_class"])
    print(f"Relevant SCIN cases: {len(df)}")
    print(df["disease_class"].value_counts().to_string())

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    rows, jobs = [], []
    for r in df.to_dict("records"):
        fst_d = derm_fst(r)
        fst_s = self_fst(r["fitzpatrick_skin_type"])
        fst = fst_d if fst_d is not None else fst_s
        mst = r.get("monk_skin_tone_label_us")
        for i in (1, 2, 3):
            p = r.get(f"image_{i}_path")
            if not isinstance(p, str):
                continue
            out = OUT_DIR / f"{r['case_id']}_{i}.png"
            jobs.append((p, out))
            rows.append(dict(
                path=str(out.relative_to(DATASET_DIR.parent)), source="SCIN",
                group_id=f"SCIN_{r['case_id']}", orig_label=r["top_condition"],
                disease_class=r["disease_class"], label=int(r["disease_class"] == "Eczema"),
                fst=fst, fst_source="dermatologist" if fst_d is not None else (
                    "self_report" if fst_s is not None else None),
                fst_self=fst_s, mst=mst if mst == mst else None, fst_group=fst_group(fst),
                shot_type=r.get(f"image_{i}_shot_type"), label_weight=r["top_weight"],
                sex=r["sex_at_birth"] if r["sex_at_birth"] in ("FEMALE", "MALE") else None,
                age_group=r.get("age_group"),
            ))

    print(f"Downloading {len(jobs)} images...")
    with ThreadPoolExecutor(max_workers=16) as ex:
        ok = list(ex.map(download_one, jobs))
    rows = [row for row, good in zip(rows, ok) if good]

    ROWS_PATH.parent.mkdir(parents=True, exist_ok=True)
    pd.DataFrame(rows).to_csv(ROWS_PATH, index=False)
    print(f"Wrote {len(rows)} rows ({len(jobs) - len(rows)} failed) -> {ROWS_PATH}")


if __name__ == "__main__":
    main()
