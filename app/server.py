"""
SkinCheck local hub: the dashboard and API, served from this laptop only (no internet, no
cloud -- photos never leave the machine).

    python3 app/server.py          then open http://localhost:8000

Endpoints
  GET  /                  dashboard (live camera, history, triggers, report)
  POST /api/screen        image -> stage 1 only (normal vs rash), for the live feed
  POST /api/analyze       image -> all stages; saved to history. The ESP32-CAM can POST its
                          JPEG here too when its own screen says rash. ?site=<body site> tags it
                          for before/after; a blurry or badly lit photo returns verdict "retake"
                          (not saved) unless ?force=true.
  POST /api/wearable/window    JSON {eda, temp, bvp, acc} -- one raw 30-60 s window from the
                               GSR wristband (EDA/TEMP 4 Hz, BVP 64 Hz, ACC 32 Hz as [x,y,z]);
                               the first 3 calibrate the stress model to the wearer
  POST /api/wearable/moisture  JSON {patch: [...], reference: [...]} -- skin moisture patch
  POST /api/wearable/scratch   JSON {count} -- scratch bursts detected by the wrist IMU
  POST /api/sim                JSON {scenario: calm|stress|itchy_night|flare|off} -- replay
                               recorded data instead of hardware (app/wearable.py)
  POST /api/journal       JSON {stress 0-10, sleep_hours, new_product, notes}
  GET  /api/state         wearable markers + journal + trigger scores
  GET/POST /api/settings  {fitzpatrick: 1-6 or null} -- self-reported skin type; stage 1 then uses
                          that group's calibrated threshold
  GET  /api/fairness      stage-1 performance by skin type, before and after calibration
  GET  /api/history       all past analyses
  GET  /api/sites         first vs latest severity per body site
  POST /api/demo/seed     add four weeks of sample check-ins (marked sample) / clear them
  GET  /report            printable doctor report (browser: Print -> Save as PDF)

Data lives in app/data/ (SQLite + images), which git ignores.
"""
import io
import os
import json
import sqlite3
import sys
import time
from html import escape
from pathlib import Path
from typing import List, Optional

import uvicorn
from fastapi import FastAPI, File, UploadFile
from fastapi.responses import FileResponse, HTMLResponse
from fastapi.staticfiles import StaticFiles
from PIL import Image, ImageOps
from pydantic import BaseModel

sys.path.insert(0, str(Path(__file__).resolve().parent))
import features  # noqa: E402
from pipeline import SkinPipeline, flare_risk, trigger_scores  # noqa: E402
from wearable import Simulator, Wearable  # noqa: E402

APP_DIR = Path(__file__).resolve().parent
DATA_DIR = APP_DIR / "data"
IMG_DIR = DATA_DIR / "images"
DB_PATH = DATA_DIR / "history.sqlite"
SETTINGS_PATH = DATA_DIR / "settings.json"
RASH_SUMMARY = APP_DIR.parent / "experiments" / "clip_rash_head" / "summary.json"
IMG_DIR.mkdir(parents=True, exist_ok=True)

app = FastAPI(title="SkinCheck hub")
app.mount("/static", StaticFiles(directory=APP_DIR / "static"), name="static")
app.mount("/images", StaticFiles(directory=IMG_DIR), name="images")
pipeline = SkinPipeline()
wearable = Wearable()
simulator = Simulator(wearable)
state = {"journal": {}, "journal_at": None}
settings = json.loads(SETTINGS_PATH.read_text()) if SETTINGS_PATH.exists() else {"fitzpatrick": None}


def db():
    con = sqlite3.connect(DB_PATH)
    con.execute("""CREATE TABLE IF NOT EXISTS analyses (
        id INTEGER PRIMARY KEY AUTOINCREMENT, ts REAL, source TEXT, verdict TEXT,
        result TEXT, image TEXT, heatmap TEXT)""")
    return con


def read_image(upload):
    img = Image.open(io.BytesIO(upload.file.read()))
    return ImageOps.exif_transpose(img).convert("RGB")


class Window(BaseModel):
    eda: List[float]
    temp: List[float]
    bvp: List[float]
    acc: List[List[float]]


class Moisture(BaseModel):
    patch: List[float]
    reference: List[float]


class Scratch(BaseModel):
    count: int = 1


class Sim(BaseModel):
    scenario: str


class Settings(BaseModel):
    fitzpatrick: Optional[int] = None


class Journal(BaseModel):
    stress: Optional[float] = None
    sleep_hours: Optional[float] = None
    new_product: bool = False
    notes: str = ""


@app.get("/")
def index():
    return FileResponse(APP_DIR / "static" / "index.html")


@app.post("/api/screen")
def screen(image: UploadFile = File(...)):
    img = read_image(image)
    return dict(pipeline.screen(img, settings["fitzpatrick"]), quality=features.quality(img))


RETAKE_ADVICE = {"blurry": "The photo is blurry. Hold the camera still, 10-15 cm from the skin.",
                 "too_dark": "The photo is too dark. Move to better light.",
                 "too_bright": "The photo is washed out. Avoid direct light or the flash glare."}


def save_result(result, heat, img, ts, source):
    stem = f"{int(ts * 1000)}"
    img.save(IMG_DIR / f"{stem}.jpg", quality=90)
    heat_name = None
    if heat is not None:
        heat_name = f"{stem}_heat.jpg"
        heat.save(IMG_DIR / heat_name, quality=90)
    result.update(image=f"/images/{stem}.jpg", heatmap=f"/images/{heat_name}" if heat_name else None)
    with db() as con:
        cur = con.execute("INSERT INTO analyses (ts, source, verdict, result, image, heatmap) "
                          "VALUES (?, ?, ?, ?, ?, ?)",
                          (ts, source, result["verdict"], json.dumps(result), result["image"],
                           result["heatmap"]))
        result["id"] = cur.lastrowid
    result["ts"] = ts
    return result


@app.post("/api/analyze")
def analyze(image: UploadFile = File(...), source: str = "webcam", site: Optional[str] = None,
            force: bool = False):
    img = read_image(image)
    q = features.quality(img)
    if not q["ok"] and not force:
        return dict(verdict="retake", headline="Please retake the photo", quality=q,
                    advice=" ".join(RETAKE_ADVICE[i] for i in q["issues"]))
    result, heat = pipeline.analyze(img, wearable.triggers(), state["journal"], settings["fitzpatrick"])
    site = site if site in features.BODY_SITES else None
    result.update(site=site, quality=q, compare=features.compare_with_previous(history(), site, result))
    if result.get("triggers") is not None:
        result["alerts"] = features.alerts(result["triggers"], result["flare"]["trigger_index"])
    return save_result(result, heat, img, time.time(), source)


@app.post("/api/wearable/window")
def wearable_window(w: Window):
    wearable.window(w.eda, w.temp, w.bvp, w.acc)
    return api_state()


@app.post("/api/wearable/moisture")
def wearable_moisture(m: Moisture):
    wearable.moisture(m.patch, m.reference)
    return api_state()


@app.post("/api/wearable/scratch")
def wearable_scratch(s: Scratch):
    wearable.scratch(s.count)
    return api_state()


@app.post("/api/sim")
def sim(s: Sim):
    if s.scenario == "off":
        simulator.stop()
    else:
        simulator.start(s.scenario)
    return api_state()


@app.post("/api/journal")
def journal(j: Journal):
    state["journal"] = j.model_dump()
    state["journal_at"] = time.time()
    return api_state()


@app.get("/api/state")
def api_state():
    trig = trigger_scores(wearable.triggers(), state["journal"])
    index = flare_risk(1.0, trig)["trigger_index"]
    return dict(wearable=wearable.snapshot(), sim=simulator.status(), journal=state["journal"],
                journal_at=state["journal_at"], triggers=trig, trigger_index=index,
                alerts=features.alerts(trig, index))


@app.get("/api/settings")
def get_settings():
    return settings


@app.post("/api/settings")
def set_settings(s: Settings):
    settings["fitzpatrick"] = s.fitzpatrick if s.fitzpatrick in range(1, 7) else None
    SETTINGS_PATH.write_text(json.dumps(settings))
    return settings


@app.get("/api/fairness")
def fairness():
    s = json.loads(RASH_SUMMARY.read_text())
    return dict(before=s["by_fitzpatrick"], after=s["by_fitzpatrick_calibrated"],
                offsets=s["fitzpatrick_offsets"])


@app.get("/api/history")
def history():
    with db() as con:
        rows = con.execute("SELECT id, ts, source, result FROM analyses ORDER BY ts").fetchall()
    return [dict(json.loads(r[3]), id=r[0], ts=r[1], source=r[2]) for r in rows]


@app.get("/api/sites")
def sites():
    return dict(sites=features.site_summary(history()), all_sites=features.BODY_SITES)


@app.post("/api/demo/seed")
def demo_seed():
    demo_clear()
    try:
        entries = features.sample_history(pipeline)
    except FileNotFoundError as e:
        return dict(added=0, error=str(e))
    for ts, res, heat, img in entries:
        save_result(res, heat, img, ts, "sample")
    return dict(added=len(entries))


@app.post("/api/demo/clear")
def demo_clear():
    with db() as con:
        rows = con.execute("SELECT id, image, heatmap FROM analyses WHERE source = 'sample'").fetchall()
        con.execute("DELETE FROM analyses WHERE source = 'sample'")
    for _, image, heat in rows:
        for url in (image, heat):
            if url:
                (IMG_DIR / Path(url).name).unlink(missing_ok=True)
    return dict(removed=len(rows))


@app.get("/report", response_class=HTMLResponse)
def report():
    items = [h for h in history() if h["verdict"] != "no_skin"]
    rows = []
    for h in reversed(items):
        when = time.strftime("%d %b %Y %H:%M", time.localtime(h["ts"]))
        sev = f"{h['severity']['band']} ({h['severity']['score']}/12)" if h.get("severity") else "-"
        flare = f"{h['flare']['risk']:.2f}" if h.get("flare") else "-"
        heat = f'<img src="{h["heatmap"]}">' if h.get("heatmap") else ""
        site = escape(h["site"].replace("_", " ")) if h.get("site") else "-"
        sample = " <i>(sample)</i>" if h.get("sample") else ""
        rows.append(f"<tr><td>{when}{sample}</td><td>{site}</td><td><img src=\"{h['image']}\">{heat}</td>"
                    f"<td>{escape(h['headline'])}</td><td>{sev}</td><td>{flare}</td></tr>")
    trig = api_state()["triggers"]
    trig_txt = ", ".join(f"{k} {v:.2f}" for k, v in trig.items()) or "none logged"
    return f"""<!doctype html><html><head><meta charset="utf-8"><title>SkinCheck report</title>
<style>body{{font-family:-apple-system,Segoe UI,sans-serif;margin:32px;color:#1d2433}}
h1{{margin:0 0 4px}}.muted{{color:#667085}}table{{border-collapse:collapse;width:100%;margin-top:16px}}
td,th{{border-bottom:1px solid #e4e7ec;padding:8px;text-align:left;vertical-align:top;font-size:14px}}
img{{height:90px;border-radius:6px;margin-right:6px}}.note{{margin-top:24px;font-size:12px;color:#667085}}</style>
</head><body><h1>SkinCheck - skin history report</h1>
<div class="muted">Generated {time.strftime('%d %b %Y %H:%M')} - {len(items)} checks - current triggers: {escape(trig_txt)}</div>
<table><tr><th>When</th><th>Body site</th><th>Photo / heatmap</th><th>Result</th><th>Severity</th><th>Flare risk</th></tr>
{''.join(rows) or '<tr><td colspan=6>No checks yet.</td></tr>'}</table>
<p class="note">Prototype screening aid produced by an AI model on the patient's own device. Not a diagnosis.
Severity is a 0-12 proxy of four EASI signs (erythema, papulation, excoriation, lichenification) from one
photo, not a full EASI score.</p><script>window.onload=()=>setTimeout(()=>window.print(),400)</script>
</body></html>"""


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=int(os.environ.get("PORT", 8000)))
