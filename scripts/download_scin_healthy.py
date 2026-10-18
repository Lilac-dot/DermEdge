"""
Downloads the SCIN cases that show healthy skin, as the negative class for a Stage 1
"healthy skin vs skin condition" gate in front of the eczema-vs-look-alikes model.

Why SCIN: the healthy and the condition images come from the same source (US consumers
photographing their own skin with their phones), so camera, lighting and framing can't
separate the classes the way stock "Normal" photos did in the original Eczema-vs-Normal
dataset (docs/PROJECT_HISTORY.md section 1.1).

A case counts as healthy when EITHER
  - a dermatologist graded it "image quality sufficient, no discernible pathology", OR
  - the submitter chose "looks healthy" AND no dermatologist gave it a condition label AND
    the image quality was not graded insufficient.
"Looks healthy" alone isn't enough: 157 of 160 such cases got a condition label from the
dermatologists (folliculitis, tinea, ...).

Writes:
  dataset/SCIN/images_healthy/<case_id>_<n>.png
  dataset/healthy/scin_healthy_rows.csv  (one row per downloaded image, metadata attached)
"""
from concurrent.futures import ThreadPoolExecutor

import pandas as pd

from download_scin_skintone import derm_fst, download_one, self_fst
from paths import SCIN_DIR, DATASET_DIR
from skintone_labels import fst_group

OUT_DIR = SCIN_DIR / "images_healthy"
ROWS_PATH = DATASET_DIR / "healthy" / "scin_healthy_rows.csv"
NO_PATHOLOGY = "YES_IMAGE_QUALITY_SUFFICIENT_NO_DISCERNIBLE_PATHOLOGY"
INSUFFICIENT = "NO_IMAGE_QUALITY_INSUFFICIENT"


def healthy_cases(df):
    grade = df["dermatologist_gradable_for_skin_condition_1"]
    no_label = df["weighted_skin_condition_label"].fillna("{}").eq("{}")
    looks_healthy = df["related_category"].eq("LOOKS_HEALTHY")
    return df[grade.eq(NO_PATHOLOGY) | (looks_healthy & no_label & grade.ne(INSUFFICIENT))]


def main():
    cases = pd.read_csv(SCIN_DIR / "scin_cases.csv", dtype={"case_id": str})
    labels = pd.read_csv(SCIN_DIR / "scin_labels.csv", dtype={"case_id": str})
    df = healthy_cases(cases.merge(labels, on="case_id"))
    print(f"Healthy SCIN cases: {len(df)}")

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
                group_id=f"SCIN_{r['case_id']}", orig_label="Healthy",
                disease_class="Healthy", label=0,
                fst=fst, fst_source="dermatologist" if fst_d is not None else (
                    "self_report" if fst_s is not None else None),
                fst_self=fst_s, mst=mst if mst == mst else None, fst_group=fst_group(fst),
                shot_type=r.get(f"image_{i}_shot_type"),
                healthy_reason="no_discernible_pathology"
                if r["dermatologist_gradable_for_skin_condition_1"] == NO_PATHOLOGY
                else "looks_healthy_unlabelled",
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
