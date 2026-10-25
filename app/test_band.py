"""Tests for app/band.py on a throwaway database:  python3 app/test_band.py"""
import sqlite3
import sys
import tempfile
import unittest
from pathlib import Path

from fastapi import FastAPI
from fastapi.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parent))
from band import make_router  # noqa: E402

PW = "correct horse battery"


class BandTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db = Path(self.tmp.name) / "test.sqlite"
        self.scratches, self.alert = [], dict(level="low", index=0.1, triggers=[], tips=[])
        app = FastAPI()
        app.include_router(make_router(self.db, on_scratch=lambda n, ts: self.scratches.append(ts),
                                       flare_alert=lambda: self.alert))
        self.app = app
        self.c = self.client("tishya")

    def tearDown(self):
        self.tmp.cleanup()

    def client(self, username):
        c = TestClient(self.app)
        r = c.post("/api/auth/register", json=dict(username=username, password=PW))
        self.assertEqual(r.status_code, 200, r.text)
        return c

    def pair_and_start(self, c=None, device_id="AA:BB:CC:DD:EE:01"):
        c = c or self.c
        token = c.post("/api/band/devices", json=dict(device_id=device_id)).json()["device_token"]
        s = c.post("/api/band/sessions/start", json=dict(device_id=device_id)).json()
        return token, s

    # --- accounts ----------------------------------------------------------------------
    def test_needs_login(self):
        anon = TestClient(self.app)
        for method, url in (("get", "/api/band/sessions"), ("get", "/api/band/alerts"),
                            ("post", "/api/band/sessions/1/stop"), ("get", "/api/auth/me")):
            self.assertEqual(getattr(anon, method)(url).status_code, 401, url)

    def test_login_logout(self):
        c = TestClient(self.app)
        self.assertEqual(c.post("/api/auth/login", json=dict(username="tishya", password="wrong password")).status_code, 401)
        self.assertEqual(c.post("/api/auth/login", json=dict(username="TISHYA", password=PW)).status_code, 200)
        self.assertEqual(c.get("/api/auth/me").json()["username"], "tishya")
        c.post("/api/auth/logout")
        self.assertEqual(c.get("/api/auth/me").status_code, 401)

    def test_duplicate_username_and_weak_password(self):
        anon = TestClient(self.app)
        self.assertEqual(anon.post("/api/auth/register", json=dict(username="Tishya", password=PW)).status_code, 409)
        self.assertEqual(anon.post("/api/auth/register", json=dict(username="new", password="short")).status_code, 422)

    def test_no_plaintext_secrets_in_db(self):
        token, _ = self.pair_and_start()
        dump = "\n".join(sqlite3.connect(self.db).iterdump())
        self.assertNotIn(PW, dump)
        self.assertNotIn(token, dump)
        self.assertNotIn(self.c.cookies.get("dermedge_session"), dump)

    # --- devices and sessions ----------------------------------------------------------
    def test_session_flow(self):
        _, s = self.pair_and_start()
        again = self.c.post("/api/band/sessions/start", json=dict(device_id="AA:BB:CC:DD:EE:01")).json()
        self.assertEqual(again["id"], s["id"], "start while recording reuses the open session")
        r = self.c.post(f"/api/band/sessions/{s['id']}/readings", json=dict(
            scratch_events=[dict(t=5, duration_s=3.2, intensity=0.8), dict(t=9, duration_s=1.5, intensity=0.4)],
            motion=[dict(t=1, activity=0.2, scratch_energy=0.05)],
            temperatures=[dict(t=1, lesion_c=34.1, reference_c=33.2)],
            gsr=[dict(t=1, microsiemens=4.2)]))
        self.assertEqual(r.json()["stored"]["scratch_events"], 2)
        self.assertEqual(len(self.scratches), 2, "scratches reach the live trigger model")
        stopped = self.c.post(f"/api/band/sessions/{s['id']}/stop").json()
        self.assertIsNotNone(stopped["stopped_at"])
        self.assertEqual(stopped["scratch_count"], 2)
        full = self.c.get(f"/api/band/sessions/{s['id']}").json()
        self.assertEqual(full["temperatures"][0]["lesion_c"], 34.1)
        self.assertEqual(self.c.post(f"/api/band/sessions/{s['id']}/readings", json={}).status_code, 409)

    def test_device_token(self):
        token, s = self.pair_and_start()
        band = TestClient(self.app, headers={"Authorization": f"Bearer {token}"})
        r = band.post(f"/api/band/sessions/{s['id']}/readings", json=dict(gsr=[dict(t=2, microsiemens=3.0)]))
        self.assertEqual(r.status_code, 200)
        self.assertEqual(band.get("/api/band/sessions").status_code, 403, "a band can't read history")
        self.assertEqual(band.post("/api/band/devices", json=dict(device_id="XX:01")).status_code, 403)
        # re-pairing replaces the token, and unpairing kills it
        new = self.c.post("/api/band/devices", json=dict(device_id="AA:BB:CC:DD:EE:01")).json()
        self.assertEqual(band.post(f"/api/band/sessions/{s['id']}/stop").status_code, 401)
        band2 = TestClient(self.app, headers={"Authorization": f"Bearer {new['device_token']}"})
        self.assertEqual(band2.post(f"/api/band/sessions/{s['id']}/stop").status_code, 200)
        self.c.delete(f"/api/band/devices/{new['id']}")
        self.assertEqual(band2.post("/api/band/sessions/start", json=dict(device_id="AA:BB:CC:DD:EE:01")).status_code, 401)

    def test_users_are_isolated(self):
        _, s = self.pair_and_start()
        other = self.client("someone")
        self.assertEqual(other.get(f"/api/band/sessions/{s['id']}").status_code, 404)
        self.assertEqual(other.post(f"/api/band/sessions/{s['id']}/stop").status_code, 404)
        self.assertEqual(other.get("/api/band/sessions").json(), [])
        self.assertEqual(other.post("/api/band/devices", json=dict(device_id="AA:BB:CC:DD:EE:01")).status_code, 409,
                         "a band paired to someone else can't be taken over")

    def test_timestamps_clamped_to_session(self):
        _, s = self.pair_and_start()
        self.c.post(f"/api/band/sessions/{s['id']}/readings",
                    json=dict(gsr=[dict(ts=0, microsiemens=1), dict(ts=4e9, microsiemens=2)]))
        rows = self.c.get(f"/api/band/sessions/{s['id']}").json()["gsr"]
        for row in rows:
            self.assertGreaterEqual(row["ts"], s["started_at"])

    def test_raw_recording(self):
        _, s = self.pair_and_start()
        r = self.c.post(f"/api/band/sessions/{s['id']}/raw",
                        json=dict(label="scratching", samples=[[0.01, -0.02, 1.0]] * 200))
        self.assertEqual(r.json()["n"], 200)
        self.assertEqual(self.c.post(f"/api/band/sessions/{s['id']}/raw",
                                     json=dict(label="x", samples=[[1, 2]])).status_code, 422)

    # --- alerts ------------------------------------------------------------------------
    def test_alert_raised_once_then_cooldown(self):
        _, s = self.pair_and_start()
        url = f"/api/band/sessions/{s['id']}/readings"
        self.assertIsNone(self.c.post(url, json={}).json()["alert"], "no alert while risk is low")
        self.alert = dict(level="high", index=0.8, triggers=["night scratching (wrist IMU)"],
                          tips=["Keep nails short."])
        a = self.c.post(url, json={}).json()["alert"]
        self.assertEqual(a["reasons"], ["night scratching (wrist IMU)"])
        self.assertIsNone(self.c.post(url, json={}).json()["alert"], "cooldown stops repeats")
        self.assertEqual(self.c.post(f"/api/band/alerts/{a['id']}/dismiss").status_code, 200)
        self.assertIsNotNone(self.c.get("/api/band/alerts").json()[0]["dismissed_at"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
