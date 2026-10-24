"""
Unpacks the WESAD wristband tables and summarises calm vs stress for each participant, as a
readable check that the data behind the stress model (Stage A) separates the two states.

Input: dataset/WESAD/WESAD.zip, the official ICMI'18 download (2.25 GB) from
https://ubi29.informatik.uni-siegen.de/usi/data_wesad.html (academic, non-commercial use; cite
Schmidt et al., ICMI 2018). Not redistributable, so it isn't in the repo.

Writes, all under dataset/WESAD/ (git-ignored):
  subjects/S*/wrist_E4/{EDA,TEMP,BVP,HR,ACC,IBI}.csv  Empatica E4 tables: row 1 start time
                                                       (unix UTC), row 2 sample rate (Hz), then
                                                       readings; units in info.txt
  subjects/S*/S*_quest.csv, S*_readme.txt              phase times, questionnaires, demographics
  wesad_wrist_summary.csv                              one row per participant
Only these small files are extracted. The S*.pkl files (~13 GB) stay in the zip and are never
unpickled: loading a downloaded pickle can run code.

Alignment: the phase times in S*_quest.csv count from the chest strap (RespiBAN) start, and the
wristband started 15-24 min earlier. Each participant's offset comes from the two devices'
start times (the RespiBAN header is local German time, CEST). Without it heart rate comes out
lower under stress for all 15, which is backwards; with it, EDA and HR rise for 14/15.
"""
import io
import json
import zipfile
from datetime import datetime, timedelta, timezone

import numpy as np
import pandas as pd

from paths import WESAD_DIR

ZIP_PATH = WESAD_DIR / "WESAD.zip"
OUT_DIR = WESAD_DIR / "subjects"
SUMMARY_PATH = WESAD_DIR / "wesad_wrist_summary.csv"
CEST = timezone(timedelta(hours=2))  # the study ran in Germany in summer 2017
PHASES = (("Base", "calm"), ("TSST", "stress"))  # TSST: public speaking + mental arithmetic


def extract(z):
    for name in z.namelist():
        parts = name.split("/")
        if len(parts) != 3 or ".." in name:
            continue
        subj, fname = parts[1], parts[2]
        d = OUT_DIR / subj
        if fname.endswith(("_quest.csv", "_readme.txt")):
            d.mkdir(parents=True, exist_ok=True)
            (d / fname).write_bytes(z.read(name))
        elif fname.endswith("_E4_Data.zip"):
            inner = zipfile.ZipFile(io.BytesIO(z.read(name)))
            (d / "wrist_E4").mkdir(parents=True, exist_ok=True)
            for f in inner.namelist():
                if "/" not in f and "\\" not in f and ".." not in f:
                    (d / "wrist_E4" / f).write_bytes(inner.read(f))


def chest_start(z, s):
    """RespiBAN start as a unix time, from the JSON on its file's second header line."""
    with z.open(f"WESAD/{s}/{s}_respiban.txt") as f:
        f.readline()
        h = next(iter(json.loads(f.readline().decode()[2:]).values()))
    hh, mm, ss = h["time"].split(":")
    y, mo, d = map(int, h["date"].split("-"))
    return datetime(y, mo, d, int(hh), int(mm), int(float(ss)), tzinfo=CEST).timestamp()


def load_e4(path):
    raw = pd.read_csv(path, header=None)
    return float(raw.iloc[0, 0]), float(raw.iloc[1, 0]), raw.iloc[2:, 0].to_numpy(float)


def mmss(x):
    """Quest times are minutes.seconds, e.g. 39.55 = 39 min 55 s."""
    m = int(x)
    return 60 * m + round((x - m) * 100)


def phases(quest_path):
    q = {}
    for line in quest_path.read_text().splitlines():
        parts = line.split(";")
        if parts[0] in ("# ORDER", "# START", "# END"):
            q[parts[0][2:]] = [p for p in parts[1:] if p]
    return {n: (mmss(float(a)), mmss(float(b))) for n, a, b in zip(q["ORDER"], q["START"], q["END"])}


def summarise(z):
    rows = []
    for d in sorted(OUT_DIR.iterdir(), key=lambda p: int(p.name[1:])):
        s = d.name
        info = dict(line.split(":", 1) for line in (d / f"{s}_readme.txt").read_text().splitlines()
                    if ":" in line)
        sig = {k: load_e4(d / "wrist_E4" / f"{k}.csv") for k in ("EDA", "TEMP", "HR")}
        offset = chest_start(z, s) - sig["EDA"][0]
        ph = phases(d / f"{s}_quest.csv")
        row = dict(subject=s, age=info.get("Age", "").strip(), gender=info.get("Gender", "").strip(),
                   wrist_lead_min=round(offset / 60, 1),
                   minutes=round(len(sig["EDA"][2]) / sig["EDA"][1] / 60, 1))
        for phase, label in PHASES:
            a, b = (t + offset for t in ph[phase])
            for k, (t0, fs, x) in sig.items():
                seg = x[int((a + sig["EDA"][0] - t0) * fs):int((b + sig["EDA"][0] - t0) * fs)]
                if k == "TEMP":
                    seg = seg[seg < 45]  # the E4 logs a few ~380 C values while warming up
                row[f"{k}_{label}"] = round(float(np.mean(seg)), 2) if len(seg) else None
        rows.append(row)
    return pd.DataFrame(rows)


def main():
    z = zipfile.ZipFile(ZIP_PATH)
    extract(z)
    df = summarise(z)
    df.to_csv(SUMMARY_PATH, index=False)
    pd.set_option("display.width", 200)
    print(df.to_string(index=False))
    n = len(df)
    print(f"\nunder stress: EDA up {int((df.EDA_stress > df.EDA_calm).sum())}/{n}, "
          f"HR up {int((df.HR_stress > df.HR_calm).sum())}/{n}, "
          f"skin temp down {int((df.TEMP_stress < df.TEMP_calm).sum())}/{n}")
    print("wrote", SUMMARY_PATH)


if __name__ == "__main__":
    main()
