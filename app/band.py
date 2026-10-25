"""
DermEdge band: accounts, paired bands, recording sessions and their readings, in the hub's
SQLite. Every endpoint here needs a login, so on a shared network (school, hospital) nobody
else can read or write someone's data.

Who can call what
  user     logged in on the website (HttpOnly session cookie, or "Authorization: Bearer <token>")
  device   a paired band posting over Wi-Fi ("Authorization: Bearer <device token>"); it can only
           start/stop and fill its own sessions. Over Bluetooth the website relays for the band,
           as the logged-in user.

Endpoints
  POST /api/auth/register      {username, password} -> logged in
  POST /api/auth/login         {username, password} -> logged in
  POST /api/auth/logout
  GET  /api/auth/me
  POST /api/band/devices       {device_id, name} -> pairs the band, returns its token ONCE
  GET  /api/band/devices       DELETE /api/band/devices/{id} unpairs (the token stops working)
  POST /api/band/sessions/start        {device_id, label, started_by: website|touch}
  POST /api/band/sessions/{id}/stop
  GET  /api/band/sessions      GET /api/band/sessions/{id} (with readings)
  POST /api/band/sessions/{id}/readings  batch of scratch events, motion, temperatures, GSR,
                               moisture; each item has ts (epoch s) or t (s since the session
                               started, for a band with no clock). May return a new alert.
  POST /api/band/sessions/{id}/raw     opt-in 100 Hz accelerometer clip, for tuning the detector
  GET  /api/band/alerts        POST /api/band/alerts/{id}/dismiss

Passwords are salted PBKDF2-SHA256 hashes (600k rounds, OWASP's 2023 figure); session and device tokens are stored as SHA-256 hashes, so
a copied database file holds no usable secret.
"""
import hashlib
import hmac
import json
import secrets
import sqlite3
import struct
import time
from typing import Callable, List, Literal, Optional

from fastapi import APIRouter, Depends, HTTPException, Request, Response
from pydantic import BaseModel, Field

COOKIE = "dermedge_session"
SESSION_DAYS = 14
ALERT_COOLDOWN_S = 30 * 60  # one flare alert per half hour at most
MAX_RAW_SAMPLES = 100 * 120  # two minutes at 100 Hz per clip
PBKDF2_ROUNDS = 600_000  # macOS's system Python has no hashlib.scrypt

SCHEMA = """
CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT UNIQUE NOT NULL COLLATE NOCASE,
    pw_salt BLOB NOT NULL, pw_hash BLOB NOT NULL, created_at REAL NOT NULL);
CREATE TABLE IF NOT EXISTS auth_tokens (
    token_hash TEXT PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at REAL NOT NULL, expires_at REAL NOT NULL);
CREATE TABLE IF NOT EXISTS devices (
    id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    device_id TEXT UNIQUE NOT NULL, name TEXT NOT NULL, token_hash TEXT NOT NULL,
    paired_at REAL NOT NULL, last_seen REAL);
CREATE TABLE IF NOT EXISTS sessions (
    id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    device INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    started_at REAL NOT NULL, stopped_at REAL, label TEXT NOT NULL, started_by TEXT NOT NULL,
    notes TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS scratch_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT, session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    ts REAL NOT NULL, duration_s REAL NOT NULL, intensity REAL NOT NULL);
CREATE TABLE IF NOT EXISTS motion_summary (
    session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE, ts REAL NOT NULL,
    activity REAL NOT NULL, scratch_energy REAL NOT NULL, PRIMARY KEY (session_id, ts));
CREATE TABLE IF NOT EXISTS temperatures (
    session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE, ts REAL NOT NULL,
    lesion_c REAL NOT NULL, reference_c REAL, PRIMARY KEY (session_id, ts));
CREATE TABLE IF NOT EXISTS gsr (
    session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE, ts REAL NOT NULL,
    microsiemens REAL NOT NULL, PRIMARY KEY (session_id, ts));
CREATE TABLE IF NOT EXISTS moisture (
    session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE, ts REAL NOT NULL,
    patch REAL NOT NULL, reference REAL, PRIMARY KEY (session_id, ts));
CREATE TABLE IF NOT EXISTS raw_recordings (
    id INTEGER PRIMARY KEY AUTOINCREMENT, session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    ts REAL NOT NULL, fs REAL NOT NULL, label TEXT NOT NULL, n INTEGER NOT NULL, samples BLOB NOT NULL);
CREATE TABLE IF NOT EXISTS alerts (
    id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    session_id INTEGER REFERENCES sessions(id) ON DELETE SET NULL, ts REAL NOT NULL,
    level TEXT NOT NULL, risk REAL NOT NULL, reasons TEXT NOT NULL, tips TEXT NOT NULL,
    dismissed_at REAL);
CREATE INDEX IF NOT EXISTS sessions_user ON sessions(user_id, started_at);
CREATE INDEX IF NOT EXISTS scratch_session ON scratch_events(session_id, ts);
CREATE INDEX IF NOT EXISTS alerts_user ON alerts(user_id, ts);
"""


# --- request bodies --------------------------------------------------------------------
class Credentials(BaseModel):
    username: str = Field(min_length=3, max_length=40, pattern=r"^[A-Za-z0-9_.-]+$")
    password: str = Field(min_length=8, max_length=200)


class Pair(BaseModel):
    device_id: str = Field(min_length=4, max_length=64)  # the band's BLE MAC or chip id
    name: str = Field(default="DermEdge band", max_length=40)


class Start(BaseModel):
    device_id: str
    label: str = Field(default="day", max_length=40)  # day, sleep, test, ...
    started_by: Literal["website", "touch"] = "website"
    notes: str = Field(default="", max_length=200)


class When(BaseModel):
    ts: Optional[float] = None  # epoch seconds
    t: Optional[float] = None  # seconds since the session started


class ScratchEvent(When):
    duration_s: float = Field(ge=0, le=3600)
    intensity: float = Field(ge=0)


class Motion(When):
    activity: float = Field(ge=0)
    scratch_energy: float = Field(ge=0)


class Temperature(When):
    lesion_c: float = Field(ge=10, le=50)
    reference_c: Optional[float] = Field(default=None, ge=10, le=50)


class Gsr(When):
    microsiemens: float = Field(ge=0, le=100)


class MoistureReading(When):
    patch: float
    reference: Optional[float] = None


class Readings(BaseModel):
    scratch_events: List[ScratchEvent] = []
    motion: List[Motion] = []
    temperatures: List[Temperature] = []
    gsr: List[Gsr] = []
    moisture: List[MoistureReading] = []


class Raw(BaseModel):
    label: str = Field(max_length=40)  # scratching, typing, walking, resting, ...
    fs: float = Field(default=100, gt=0, le=1000)
    ts: Optional[float] = None
    samples: List[List[float]] = Field(max_length=MAX_RAW_SAMPLES)  # [[x, y, z] in g, ...]


# --- helpers ---------------------------------------------------------------------------
def hash_password(password, salt):
    return hashlib.pbkdf2_hmac("sha256", password.encode(), salt, PBKDF2_ROUNDS)


def token_hash(token):
    return hashlib.sha256(token.encode()).hexdigest()


def make_router(db_path, on_scratch: Optional[Callable] = None,
                flare_alert: Optional[Callable[[], dict]] = None):
    """on_scratch(n, ts) feeds scratch events to the live trigger model; flare_alert() returns the
    hub's current features.alerts(...) dict, which decides whether a new alert is raised."""
    router = APIRouter()

    def db():
        con = sqlite3.connect(db_path)
        con.row_factory = sqlite3.Row
        con.execute("PRAGMA foreign_keys = ON")
        return con

    with db() as con:
        con.executescript(SCHEMA)

    # --- who is calling ----------------------------------------------------------------
    def bearer(request):
        h = request.headers.get("authorization", "")
        return h[7:].strip() if h.lower().startswith("bearer ") else None

    def caller(request: Request):
        """-> dict(user_id, device) where device is the calling band's row id, or None for a user."""
        token = request.cookies.get(COOKIE) or bearer(request)
        if not token:
            raise HTTPException(401, "Log in first")
        th, now = token_hash(token), time.time()
        with db() as con:
            row = con.execute("SELECT user_id FROM auth_tokens WHERE token_hash = ? AND expires_at > ?",
                              (th, now)).fetchone()
            if row:
                return dict(user_id=row["user_id"], device=None)
            dev = con.execute("SELECT id, user_id FROM devices WHERE token_hash = ?", (th,)).fetchone()
            if dev:
                con.execute("UPDATE devices SET last_seen = ? WHERE id = ?", (now, dev["id"]))
                return dict(user_id=dev["user_id"], device=dev["id"])
        raise HTTPException(401, "Log in again")

    def user_only(who=Depends(caller)):
        if who["device"] is not None:
            raise HTTPException(403, "Only the website can do this")
        return who

    def own_session(con, who, session_id):
        s = con.execute("SELECT * FROM sessions WHERE id = ? AND user_id = ?",
                        (session_id, who["user_id"])).fetchone()
        if not s or (who["device"] is not None and s["device"] != who["device"]):
            raise HTTPException(404, "No such session")
        return s

    def login_response(response, user_id, username):
        token = secrets.token_urlsafe(32)
        now = time.time()
        with db() as con:
            con.execute("DELETE FROM auth_tokens WHERE expires_at < ?", (now,))
            con.execute("INSERT INTO auth_tokens VALUES (?, ?, ?, ?)",
                        (token_hash(token), user_id, now, now + SESSION_DAYS * 86400))
        # HttpOnly: page scripts can't read it; SameSite=Strict: other sites can't send it
        response.set_cookie(COOKIE, token, max_age=SESSION_DAYS * 86400, httponly=True, samesite="strict")
        return dict(id=user_id, username=username, token=token)

    # --- accounts ----------------------------------------------------------------------
    @router.post("/api/auth/register")
    def register(c: Credentials, response: Response):
        salt = secrets.token_bytes(16)
        try:
            with db() as con:
                cur = con.execute("INSERT INTO users (username, pw_salt, pw_hash, created_at) VALUES (?, ?, ?, ?)",
                                  (c.username, salt, hash_password(c.password, salt), time.time()))
        except sqlite3.IntegrityError:
            raise HTTPException(409, "That username is taken")
        return login_response(response, cur.lastrowid, c.username)

    @router.post("/api/auth/login")
    def login(c: Credentials, response: Response):
        with db() as con:
            u = con.execute("SELECT id, username, pw_salt, pw_hash FROM users WHERE username = ?",
                            (c.username,)).fetchone()
        # hash even for an unknown user, so the response time doesn't reveal which usernames exist
        salt = u["pw_salt"] if u else b"\0" * 16
        ok = hmac.compare_digest(hash_password(c.password, salt), u["pw_hash"] if u else b"")
        if not (u and ok):
            time.sleep(0.5)  # slows down password guessing
            raise HTTPException(401, "Wrong username or password")
        return login_response(response, u["id"], u["username"])

    @router.post("/api/auth/logout")
    def logout(request: Request, response: Response):
        token = request.cookies.get(COOKIE) or bearer(request)
        if token:
            with db() as con:
                con.execute("DELETE FROM auth_tokens WHERE token_hash = ?", (token_hash(token),))
        response.delete_cookie(COOKIE)
        return dict(ok=True)

    @router.get("/api/auth/me")
    def me(who=Depends(user_only)):
        with db() as con:
            u = con.execute("SELECT id, username, created_at FROM users WHERE id = ?", (who["user_id"],)).fetchone()
        return dict(u)

    # --- paired bands ------------------------------------------------------------------
    @router.post("/api/band/devices")
    def pair(p: Pair, who=Depends(user_only)):
        """Pairs (or re-pairs) a band with this account. The website gets the token once and writes
        it to the band over the encrypted Bluetooth link; re-pairing replaces the old token."""
        token = secrets.token_urlsafe(32)
        now = time.time()
        with db() as con:
            row = con.execute("SELECT id, user_id FROM devices WHERE device_id = ?", (p.device_id,)).fetchone()
            if row and row["user_id"] != who["user_id"]:
                raise HTTPException(409, "This band is paired with another account. Unpair it there first")
            if row:
                con.execute("UPDATE devices SET name = ?, token_hash = ?, paired_at = ? WHERE id = ?",
                            (p.name, token_hash(token), now, row["id"]))
                dev_id = row["id"]
            else:
                dev_id = con.execute("INSERT INTO devices (user_id, device_id, name, token_hash, paired_at) "
                                     "VALUES (?, ?, ?, ?, ?)",
                                     (who["user_id"], p.device_id, p.name, token_hash(token), now)).lastrowid
        return dict(id=dev_id, device_id=p.device_id, name=p.name, device_token=token)

    @router.get("/api/band/devices")
    def devices(who=Depends(user_only)):
        with db() as con:
            rows = con.execute("SELECT id, device_id, name, paired_at, last_seen FROM devices "
                               "WHERE user_id = ? ORDER BY paired_at", (who["user_id"],)).fetchall()
        return [dict(r) for r in rows]

    @router.delete("/api/band/devices/{dev_id}")
    def unpair(dev_id: int, who=Depends(user_only)):
        with db() as con:
            n = con.execute("DELETE FROM devices WHERE id = ? AND user_id = ?", (dev_id, who["user_id"])).rowcount
        if not n:
            raise HTTPException(404, "No such band")
        return dict(ok=True)

    # --- recording sessions ------------------------------------------------------------
    def session_dict(con, s, readings=False):
        d = dict(s)
        sid = s["id"]
        d["scratch_count"] = con.execute("SELECT COUNT(*) FROM scratch_events WHERE session_id = ?",
                                         (sid,)).fetchone()[0]
        end = s["stopped_at"] or time.time()
        d["duration_s"] = round(end - s["started_at"], 1)
        if readings:
            for table, cols in (("scratch_events", "ts, duration_s, intensity"),
                                ("motion_summary", "ts, activity, scratch_energy"),
                                ("temperatures", "ts, lesion_c, reference_c"),
                                ("gsr", "ts, microsiemens"), ("moisture", "ts, patch, reference")):
                rows = con.execute(f"SELECT {cols} FROM {table} WHERE session_id = ? ORDER BY ts", (sid,))
                d[table] = [dict(r) for r in rows]
            d["raw_recordings"] = [dict(r) for r in con.execute(
                "SELECT id, ts, fs, label, n FROM raw_recordings WHERE session_id = ? ORDER BY ts", (sid,))]
        return d

    @router.post("/api/band/sessions/start")
    def start(b: Start, who=Depends(caller)):
        """Starting while a session is already open returns that one, so a double tap on the band
        and a click on the website don't make two."""
        with db() as con:
            dev = con.execute("SELECT id FROM devices WHERE device_id = ? AND user_id = ?",
                              (b.device_id, who["user_id"])).fetchone()
            if not dev or (who["device"] is not None and dev["id"] != who["device"]):
                raise HTTPException(404, "Pair this band first")
            s = con.execute("SELECT * FROM sessions WHERE device = ? AND stopped_at IS NULL", (dev["id"],)).fetchone()
            if not s:
                sid = con.execute("INSERT INTO sessions (user_id, device, started_at, label, started_by, notes) "
                                  "VALUES (?, ?, ?, ?, ?, ?)",
                                  (who["user_id"], dev["id"], time.time(), b.label, b.started_by, b.notes)).lastrowid
                s = con.execute("SELECT * FROM sessions WHERE id = ?", (sid,)).fetchone()
            return session_dict(con, s)

    @router.post("/api/band/sessions/{session_id}/stop")
    def stop(session_id: int, who=Depends(caller)):
        with db() as con:
            s = own_session(con, who, session_id)
            if s["stopped_at"] is None:
                con.execute("UPDATE sessions SET stopped_at = ? WHERE id = ?", (time.time(), session_id))
            return session_dict(con, con.execute("SELECT * FROM sessions WHERE id = ?", (session_id,)).fetchone())

    @router.get("/api/band/sessions")
    def sessions(who=Depends(user_only)):
        with db() as con:
            rows = con.execute("SELECT * FROM sessions WHERE user_id = ? ORDER BY started_at DESC",
                               (who["user_id"],)).fetchall()
            return [session_dict(con, s) for s in rows]

    @router.get("/api/band/sessions/{session_id}")
    def session(session_id: int, who=Depends(user_only)):
        with db() as con:
            return session_dict(con, own_session(con, who, session_id), readings=True)

    @router.post("/api/band/sessions/{session_id}/readings")
    def readings(session_id: int, r: Readings, who=Depends(caller)):
        with db() as con:
            s = own_session(con, who, session_id)
            if s["stopped_at"] is not None:
                raise HTTPException(409, "This session has stopped")
            t0, now = s["started_at"], time.time()

            def ts(item):  # ts > t > now; never in the future, never before the session
                t = item.ts if item.ts is not None else t0 + item.t if item.t is not None else now
                return min(max(t, t0), now)

            con.executemany("INSERT INTO scratch_events (session_id, ts, duration_s, intensity) VALUES (?, ?, ?, ?)",
                            [(session_id, ts(e), e.duration_s, e.intensity) for e in r.scratch_events])
            con.executemany("INSERT OR REPLACE INTO motion_summary VALUES (?, ?, ?, ?)",
                            [(session_id, ts(m), m.activity, m.scratch_energy) for m in r.motion])
            con.executemany("INSERT OR REPLACE INTO temperatures VALUES (?, ?, ?, ?)",
                            [(session_id, ts(x), x.lesion_c, x.reference_c) for x in r.temperatures])
            con.executemany("INSERT OR REPLACE INTO gsr VALUES (?, ?, ?)",
                            [(session_id, ts(x), x.microsiemens) for x in r.gsr])
            con.executemany("INSERT OR REPLACE INTO moisture VALUES (?, ?, ?, ?)",
                            [(session_id, ts(x), x.patch, x.reference) for x in r.moisture])
        if on_scratch:
            for e in r.scratch_events:
                on_scratch(1, ts(e))
        alert = maybe_alert(who["user_id"], session_id)
        return dict(stored={k: len(v) for k, v in r.model_dump().items()}, alert=alert)

    @router.post("/api/band/sessions/{session_id}/raw")
    def raw(session_id: int, r: Raw, who=Depends(caller)):
        if any(len(s) != 3 for s in r.samples):
            raise HTTPException(422, "Each sample is [x, y, z]")
        # int16 milli-g: 6 bytes a sample instead of ~30 as JSON text
        blob = b"".join(struct.pack("<3h", *(max(-32768, min(32767, round(v * 1000))) for v in s))
                        for s in r.samples)
        with db() as con:
            own_session(con, who, session_id)
            rid = con.execute("INSERT INTO raw_recordings (session_id, ts, fs, label, n, samples) "
                              "VALUES (?, ?, ?, ?, ?, ?)",
                              (session_id, r.ts or time.time(), r.fs, r.label, len(r.samples), blob)).lastrowid
        return dict(id=rid, n=len(r.samples))

    # --- alerts ------------------------------------------------------------------------
    def maybe_alert(user_id, session_id):
        """Stores a new alert when the hub's flare triggers say high and the last one is old enough."""
        if not flare_alert:
            return None
        a = flare_alert()
        if a.get("level") != "high":
            return None
        now = time.time()
        with db() as con:
            last = con.execute("SELECT MAX(ts) FROM alerts WHERE user_id = ?", (user_id,)).fetchone()[0]
            if last and now - last < ALERT_COOLDOWN_S:
                return None
            aid = con.execute("INSERT INTO alerts (user_id, session_id, ts, level, risk, reasons, tips) "
                              "VALUES (?, ?, ?, ?, ?, ?, ?)",
                              (user_id, session_id, now, a["level"], a["index"], json.dumps(a["triggers"]),
                               json.dumps(a["tips"]))).lastrowid
        return dict(id=aid, ts=now, level=a["level"], risk=a["index"], reasons=a["triggers"], tips=a["tips"])

    @router.get("/api/band/alerts")
    def alerts(who=Depends(user_only)):
        with db() as con:
            rows = con.execute("SELECT * FROM alerts WHERE user_id = ? ORDER BY ts DESC", (who["user_id"],))
            return [dict(r, reasons=json.loads(r["reasons"]), tips=json.loads(r["tips"])) for r in rows]

    @router.post("/api/band/alerts/{alert_id}/dismiss")
    def dismiss(alert_id: int, who=Depends(caller)):
        with db() as con:
            n = con.execute("UPDATE alerts SET dismissed_at = COALESCE(dismissed_at, ?) WHERE id = ? AND user_id = ?",
                            (time.time(), alert_id, who["user_id"])).rowcount
        if not n:
            raise HTTPException(404, "No such alert")
        return dict(ok=True)

    return router
