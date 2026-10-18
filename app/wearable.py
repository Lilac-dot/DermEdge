"""
Wearable side of the DermEdge hub: eczema-flare markers from a GSR wristband + skin patch,
and a simulator that replays real recorded data while the hardware doesn't exist yet.

Markers
  stress          GSR (EDA) + skin temperature + PPG + accelerometer, 30 s windows, through
                  the deployed WESAD LightGBM with personal-baseline calibration
                  (scripts/fusion_pipeline.py stage_a_stress_predict; LOSO AUC 0.92-0.94).
                  The wearer's first calm windows calibrate it, as on a real device.
  heart rate      dominant PPG frequency; skin conductance and skin temperature: window means
  hydration       capacitive moisture patch over an eczema-prone area vs a reference patch on
  deficit         healthy skin. In the Southampton e-textile patient data (13 patients) the
                  sensor read lower on lesional skin in 13/13 (1.7-10.2% lower).
  night scratch   wrist-IMU scratch bursts. SIMULATED ONLY: there is no public scratch dataset
                  (dead_ends/negative_results/scratch_dataset_search_2026-08-26.md).

Simulator scenarios replay WESAD subject S16 and Southampton patients S-08 (1.7% deficit)
/ S-05 (10.2%), at 15x speed (a 30 s window every 2 s).
"""
import ctypes
import os
import threading
import time
from collections import deque

import numpy as np
import torch

# LightGBM needs OpenMP; macOS without Homebrew has none, but PyTorch ships libomp.
ctypes.CDLL(os.path.join(os.path.dirname(torch.__file__), "lib", "libomp.dylib"), mode=ctypes.RTLD_GLOBAL)

import sys  # noqa: E402
from pathlib import Path  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
from fusion_pipeline import stage_a_stress_predict  # noqa: E402
from wesad_calibration import compute_feature_baseline_profile  # noqa: E402
from wesad_features import extract_window_features  # noqa: E402

WESAD_WINDOWS = ROOT / "dataset" / "WESAD" / "wesad_raw_windows.npz"
MOISTURE_DIR = ROOT / "dataset" / "southampton_moisture" / "PatientTest"
SIM_SUBJECT = "S16"
CALIBRATION_WINDOWS = 3
STRESS_CALM, STRESS_HIGH = 0.25, 0.45  # typical calibrated model output, calm vs TSST stress
HYDRATION_FULL = 8.0  # % deficit that counts as a full-strength trigger
SCRATCH_FULL = 30  # scratch bursts per night that count as a full-strength trigger


def bvp_heart_rate(bvp, fs=64):
    """Beats per minute from the median beat-to-beat interval of the band-passed PPG. (The
    dominant-FFT-frequency proxy in wesad_features locked onto sub-harmonics: 44 bpm at rest.)"""
    from scipy.signal import butter, filtfilt, find_peaks
    x = np.asarray(bvp, dtype=np.float64).ravel()
    b, a = butter(2, [0.7 / (fs / 2), 3.5 / (fs / 2)], "band")
    x = filtfilt(b, a, x)
    peaks, _ = find_peaks(x, distance=int(fs * 0.33), prominence=np.std(x) * 0.5)
    return float(60 * fs / np.median(np.diff(peaks))) if len(peaks) > 2 else None


class Wearable:
    def __init__(self):
        self.lock = threading.Lock()
        self.reset()

    def reset(self):
        self.calib_feats, self.profile = [], None
        self.stress = deque(maxlen=3)  # smoothed over the last 3 windows (~90 s)
        self.series = deque(maxlen=120)  # (ts, stress, hr) for the live chart
        self.markers = {}
        self.scratches = deque()  # timestamps of scratch bursts
        self.updated = None

    def window(self, eda, temp, bvp, acc):
        """One 30-60 s raw window (EDA/TEMP 4 Hz, BVP 64 Hz, ACC 32 Hz x 3)."""
        sig = {"EDA": np.asarray(eda, float), "TEMP": np.asarray(temp, float),
               "BVP": np.asarray(bvp, float), "ACC": np.asarray(acc, float).reshape(-1, 3)}
        with self.lock:
            m = self.markers
            m.update(skin_conductance=round(float(sig["EDA"].mean()), 2),
                     skin_temp=round(float(sig["TEMP"].mean()), 2),
                     heart_rate=round(bvp_heart_rate(sig["BVP"]) or 0))
            if self.profile is None:
                self.calib_feats.append(extract_window_features(sig))
                m["calibration"] = f"{len(self.calib_feats)}/{CALIBRATION_WINDOWS} calm windows"
                if len(self.calib_feats) >= CALIBRATION_WINDOWS:
                    self.profile = compute_feature_baseline_profile(self.calib_feats)
                    m["calibration"] = "done"
            else:
                p = stage_a_stress_predict(sig["EDA"], sig["TEMP"], sig["BVP"], sig["ACC"], self.profile)
                self.stress.append(p)
                m["stress_p"] = round(float(np.mean(self.stress)), 3)
                self.series.append((time.time(), m["stress_p"], m["heart_rate"]))
            self.updated = time.time()

    def moisture(self, patch, reference):
        with self.lock:
            deficit = (1 - float(np.mean(patch)) / float(np.mean(reference))) * 100
            self.markers["hydration_deficit"] = round(deficit, 1)
            self.updated = time.time()

    def scratch(self, n=1, ts=None):
        with self.lock:
            for _ in range(n):
                self.scratches.append(ts or time.time())
            cutoff = time.time() - 8 * 3600
            while self.scratches and self.scratches[0] < cutoff:
                self.scratches.popleft()
            self.markers["night_scratches"] = len(self.scratches)
            self.updated = time.time()

    def triggers(self):
        """Marker -> 0..1 trigger strength, for the noisy-OR flare risk."""
        m, t = self.markers, {}
        if "stress_p" in m:
            t["stress (GSR wristband)"] = float(np.clip((m["stress_p"] - STRESS_CALM) /
                                                        (STRESS_HIGH - STRESS_CALM), 0, 1))
        if "hydration_deficit" in m:
            t["dry skin (moisture patch)"] = float(np.clip(m["hydration_deficit"] / HYDRATION_FULL, 0, 1))
        if m.get("night_scratches"):
            t["night scratching (IMU, simulated)"] = float(np.clip(m["night_scratches"] / SCRATCH_FULL, 0, 1))
        return t

    def snapshot(self):
        with self.lock:
            return dict(markers=dict(self.markers), updated=self.updated,
                        series=[dict(t=t, stress=s, hr=h) for t, s, h in self.series])


def _moisture_series(patient):
    import pandas as pd
    les = pd.read_csv(MOISTURE_DIR / "lesional_sensor.csv")[patient].values
    non = pd.read_csv(MOISTURE_DIR / "nonlesional_sensor.csv")[patient].values
    return les, non


class Simulator:
    """Feeds a Wearable from recorded data in a background thread."""
    SCENARIOS = {
        "calm": dict(conditions=(1,), moisture="S-08", scratch_rate=0.0,
                     label="Calm day: baseline recording, near-normal skin hydration"),
        "stress": dict(conditions=(2,), moisture="S-08", scratch_rate=0.0,
                       label="Stressful day: WESAD stress-task recording (public speaking + arithmetic)"),
        "itchy_night": dict(conditions=(1,), moisture="S-05", scratch_rate=0.6,
                            label="Itchy night: calm physiology, dry patch, scratching bursts"),
        "flare": dict(conditions=(2,), moisture="S-05", scratch_rate=0.6,
                      label="Flare conditions: stress + dry skin + night scratching"),
    }

    def __init__(self, wearable, period=2.0):
        self.w, self.period = wearable, period
        self.scenario, self.thread, self.stop_flag = None, None, threading.Event()
        # The recordings can't be redistributed, so a fresh clone runs without them: the
        # dashboard then says so, and real hardware can still post to the hub.
        missing = [p for p in (WESAD_WINDOWS, MOISTURE_DIR / "lesional_sensor.csv") if not p.exists()]
        self.missing = (f"Replay data not found ({', '.join(str(p.relative_to(ROOT)) for p in missing)}). "
                        "See README: Wearable simulation.") if missing else None
        if missing:
            return
        z = np.load(WESAD_WINDOWS, allow_pickle=True)
        idx = np.flatnonzero(z["subject"] == SIM_SUBJECT)
        self.win = {k: z[k][idx] for k in ("EDA", "TEMP", "BVP", "ACC")}
        self.cond = z["condition"][idx]

    def start(self, scenario):
        self.stop()
        if scenario not in self.SCENARIOS or self.missing:
            return
        self.w.reset()
        self.scenario, self.stop_flag = scenario, threading.Event()
        self.thread = threading.Thread(target=self._run, args=(self.SCENARIOS[scenario], self.stop_flag),
                                       daemon=True)
        self.thread.start()

    def stop(self):
        self.stop_flag.set()
        self.scenario = None

    def _run(self, cfg, stop):
        rng = np.random.default_rng(0)
        calm = np.flatnonzero(self.cond == 1)
        main = np.flatnonzero(np.isin(self.cond, cfg["conditions"]))
        order = list(calm[:CALIBRATION_WINDOWS]) + list(main)
        les, non = _moisture_series(cfg["moisture"])
        k = 0
        while not stop.is_set():
            i = order[CALIBRATION_WINDOWS + (k - CALIBRATION_WINDOWS) % len(main)] \
                if k >= CALIBRATION_WINDOWS else order[k]
            self.w.window(*(self.win[m][i] for m in ("EDA", "TEMP", "BVP", "ACC")))
            j = k % len(les)
            self.w.moisture(les[max(0, j - 5):j + 1], non[max(0, j - 5):j + 1])
            if cfg["scratch_rate"] and rng.random() < cfg["scratch_rate"]:
                self.w.scratch(int(rng.integers(1, 4)))
            k += 1
            stop.wait(self.period)

    def status(self):
        s = self.scenario
        return dict(scenario=s, available=not self.missing, missing=self.missing,
                    label=self.SCENARIOS[s]["label"] if s else None,
                    source=f"Replaying WESAD {SIM_SUBJECT} + Southampton patient moisture data at 15x speed"
                    if s else None)
