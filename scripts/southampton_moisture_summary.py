"""
Unpacks the Southampton e-textile patient tables and summarises eczema vs healthy skin for each
patient: the real data behind the wristband's moisture patch and the hub's hydration-deficit
trigger (app/wearable.py).

Input: dataset/southampton_moisture/Dataset_EtexSensAtopicDermatitis.zip from
https://eprints.soton.ac.uk/511063/ (Todorov 2026, DOI 10.5258/SOTON/D3873, CC-BY 4.0). The
repository blocks scripted downloads, so download it in a browser. Its README.txt lists the
related publications.

Writes, under dataset/southampton_moisture/ (git-ignored):
  PatientTest/          13 patients (NOMAD study), lesional vs non-lesional skin:
                        *_sensor.csv  e-textile capacitive sensor, 30 one-second readings
                        *_corneo.csv  Corneometer (clinical hydration standard), 10 readings
                        *_TEWL.csv    transepidermal water loss, 7 patients
                        PatientNumbering.txt maps study IDs (S-02...) to thesis numbers
  README.txt            the dataset's own README
  moisture_patient_summary.csv
Only these files are extracted; the zip also holds PCB designs, ethics forms and a GUI .exe,
none of which is needed (don't run the .exe).
"""
import zipfile

import pandas as pd

from paths import DATASET_DIR

DIR = DATASET_DIR / "southampton_moisture"
ZIP_PATH = DIR / "Dataset_EtexSensAtopicDermatitis.zip"
ROOT_IN_ZIP = "Dataset_EtexSensAtopicDermatitis/"
PATIENT_DIR_IN_ZIP = ROOT_IN_ZIP + "Chapter6_PatientTesting/PatientTest/"


def extract():
    z = zipfile.ZipFile(ZIP_PATH)
    for name in z.namelist():
        if name.startswith(PATIENT_DIR_IN_ZIP) and not name.endswith("/") and ".." not in name:
            out = DIR / "PatientTest" / name[len(PATIENT_DIR_IN_ZIP):]
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(z.read(name))
    (DIR / "README.txt").write_bytes(z.read(ROOT_IN_ZIP + "README.txt"))


def means(kind, index_col):
    p = DIR / "PatientTest"
    return [pd.read_csv(p / f"{side}_{kind}.csv").drop(columns=index_col).mean()
            for side in ("lesional", "nonlesional")]


def main():
    extract()
    ls, ns = means("sensor", "Time (s)")
    lc, nc = means("corneo", "Reading Number")
    lt, nt = means("TEWL", "Reading Number")
    df = pd.DataFrame(dict(sensor_eczema=ls, sensor_healthy=ns, sensor_deficit_pct=(1 - ls / ns) * 100,
                           corneo_eczema=lc, corneo_healthy=nc, corneo_deficit_pct=(1 - lc / nc) * 100,
                           TEWL_eczema=lt, TEWL_healthy=nt)).round(2)
    df.index.name = "patient"
    out = DIR / "moisture_patient_summary.csv"
    df.to_csv(out)
    pd.set_option("display.width", 200)
    print(df.to_string())
    n, t = len(df), df.dropna(subset=["TEWL_eczema"])
    print(f"\nsensor lower on eczema skin: {int((df.sensor_deficit_pct > 0).sum())}/{n} "
          f"(by {df.sensor_deficit_pct.min()}-{df.sensor_deficit_pct.max()} %)")
    print(f"Corneometer lower on eczema skin: {int((df.corneo_deficit_pct > 0).sum())}/{n}")
    print(f"TEWL higher on eczema skin: {int((t.TEWL_eczema > t.TEWL_healthy).sum())}/{len(t)}")
    print(f"sensor vs Corneometer deficit, r = {df.sensor_deficit_pct.corr(df.corneo_deficit_pct):.2f}")
    print("wrote", out)


if __name__ == "__main__":
    main()
