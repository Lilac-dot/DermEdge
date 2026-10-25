"""
Records labelled 800 Hz accelerometer clips from the band over USB, for tools/tune.py.

    python3 tools/record.py check              # 10 s: sample rate, dropped samples, noise floor
    python3 tools/record.py session --person P03 --hand right   # guided: 8 activities x 20 s, twice
    python3 tools/record.py session --person P03 --into P03_20261026-1500 --only fingers,waving   # redo some

--person is an anonymous ID (P01, P02, ...), never a name: keep the ID-to-name list on paper or
not at all. tools/tune.py tests on each person with a model trained on everyone else.

Wear the add-on board on the back of the hand you scratch with. The band streams raw samples
while this runs ("raw on" on its serial line) and keeps working normally otherwise. Clips go to
~/DermEdge-recordings/<date-time>/<activity>_<round>.npz, outside the repo: they stay on this
computer and can't be committed by accident.
"""
import argparse
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

import numpy as np
import serial

RECORDINGS = Path.home() / "DermEdge-recordings"
FS = 800
LSB_PER_G = 256.0

# activity, what to do, scratching?
ACTIVITIES = [
    ("rest", "Rest your hand still on the table.", False),
    ("nails", "Scratch your forearm with your nails, the way you would an itch.", True),
    ("fingers", "Scratch with your fingertips only, keeping your wrist as still as you can.", True),
    ("waving", "Wave your hand side to side, fast, without touching anything.", False),
    ("typing", "Type on the keyboard: anything at all.", False),
    ("phone", "Tap and swipe on your phone.", False),
    ("rubbing", "Rub or stroke your forearm gently with your palm, like putting on moisturiser.", False),
    ("marching", "March on the spot, swinging your arms (stay within the USB cable's reach).", False),
]


class Band:
    """The band's raw stream: frames of A5 5A, count, overrun flag, sample number, count x int16 xyz.

    A background thread reads the port all the time, even while waiting at a prompt: macOS's
    serial buffer is small, and bytes it drops would split frames. A frame only counts if its
    sample number carries on exactly from the last one, so a stray A5 5A inside the data can't
    start a fake frame, and values outside the sensor's +-2 g range are refused."""

    MAX_COUNT = 600  # +-2 g is +-512 at 256 LSB/g; a little slack for the sensor's own offset

    def __init__(self, port):
        self.ser = serial.Serial()
        self.ser.port, self.ser.baudrate, self.ser.timeout = port, 115200, 0.05
        self.ser.dtr = self.ser.rts = False  # don't reset the ESP32 by opening the port
        self.ser.open()
        self.buf, self.next_seq = bytearray(), None
        self.gaps = self.overruns = self.rejected = 0
        self.samples, self.lock, self.running = [], threading.Lock(), True
        self.thread = threading.Thread(target=self._pump, daemon=True)
        self.thread.start()

    def _pump(self):
        while self.running:
            try:
                data = self.ser.read(4096)
            except serial.SerialException:
                break
            if data:
                with self.lock:
                    self.buf += data
                    self._parse()

    def _parse(self):
        while True:
            i = self.buf.find(b"\xa5\x5a")
            if i < 0:
                del self.buf[:-1]
                return
            if len(self.buf) < i + 6:
                del self.buf[:i]
                return
            n, ovr, seq = self.buf[i + 2], self.buf[i + 3], self.buf[i + 4] | self.buf[i + 5] << 8
            end = i + 6 + 6 * n
            if not 1 <= n <= 32 or ovr > 1:
                del self.buf[:i + 1]
                continue
            if len(self.buf) < end:
                del self.buf[:i]
                return
            # the next frame must start right after this one with the next sample number
            if self.next_seq is not None and seq != self.next_seq:
                self.gaps += 1
                self.next_seq = None  # resync: take the next frame that follows on cleanly
                del self.buf[:i + 1]
                continue
            x = np.frombuffer(bytes(self.buf[i + 6:end]), dtype="<i2").reshape(n, 3)
            if np.abs(x).max() > self.MAX_COUNT:
                self.rejected += 1
                self.next_seq = None
                del self.buf[:i + 1]
                continue
            follows = len(self.buf) == end or self.buf[end:end + 2] in (b"\xa5\x5a", b"\xa5")
            if not follows and len(self.buf) >= end + 2:  # garbage right after: probably not a real frame
                self.rejected += 1
                del self.buf[:i + 1]
                continue
            self.next_seq = (seq + n) & 0xFFFF
            self.overruns += ovr
            self.samples.append(x)
            del self.buf[:end]

    def start(self, wait=8.0):
        end = time.time() + wait
        while time.time() < end:
            self.ser.write(b"raw on\n")
            time.sleep(1)
            if len(self.take()):
                return
        sys.exit("No samples from the band. Is it plugged in and running the band firmware?")

    def stop(self):
        self.running = False
        self.thread.join(timeout=1)
        self.ser.write(b"raw off\n")
        self.ser.close()

    def take(self):
        """Samples that have arrived since the last call, as an (n, 3) int16 array."""
        with self.lock:
            out, self.samples = self.samples, []
        return np.concatenate(out) if out else np.zeros((0, 3), np.int16)

    def capture(self, seconds, live=None):
        self.take()  # drop what queued up while waiting
        got, t0 = [], time.time()
        while time.time() - t0 < seconds:
            time.sleep(0.05)
            got.append(self.take())
            if live:
                live(time.time() - t0)
        return np.concatenate(got)

    def lost(self):
        return self.gaps + self.overruns + self.rejected


def port_guess():
    for p in ["/dev/cu.SLAB_USBtoUART", "/dev/cu.usbserial-0001"]:
        if Path(p).exists():
            return p
    return "/dev/cu.SLAB_USBtoUART"


def check(band):
    print("Keep the band still for 10 s...")
    l0 = band.lost()
    t0 = time.time()
    x = band.capture(10)
    secs = time.time() - t0
    g = x / LSB_PER_G
    print(f"  {len(x)} samples in {secs:.1f} s = {len(x) / secs:.0f} Hz (want {FS})")
    print(f"  dropped: {band.lost() - l0} (gaps {band.gaps}, FIFO overruns {band.overruns}, bad frames {band.rejected})")
    print(f"  gravity: {np.linalg.norm(g.mean(0)):.3f} g (want about 1)")
    print(f"  noise: {', '.join(f'{a} {s * 1000:.1f} mg' for a, s in zip('xyz', g.std(0)))} RMS")
    hp = np.diff(g, axis=0)  # crude: the sample-to-sample jitter is mostly high frequency
    print(f"  high-frequency floor: {np.mean(hp ** 2) / 2:.1e} g^2 (the rule's vibration threshold is 1e-4)")


def session(band, rounds, seconds, person, hand, into=None, only=None):
    out = RECORDINGS / (into or f"{person}_{datetime.now().strftime('%Y%m%d-%H%M')}")
    out.mkdir(parents=True, exist_ok=True)
    acts = [a for a in ACTIVITIES if not only or a[0] in only]
    print(f"Saving to {out}\nWear the board on the BACK of the hand you scratch with, same way round every time.\n")
    for rnd in rounds:
        for name, what, scratch in acts:
            while True:
                input(f"[round {rnd}] {name.upper()}: {what}\n  Press Enter, then start when it says GO. ")
                for c in "321":
                    print(f"  {c}...", end="", flush=True)
                    time.sleep(1)
                print("  GO")
                bar = lambda t: print(f"\r  recording {'#' * int(t)}{'.' * (seconds - int(t))} ", end="", flush=True)
                l0 = band.lost()
                x = band.capture(seconds, bar)
                lost = band.lost() - l0
                print(f"\r  done: {len(x)} samples{f', {lost} dropouts' if lost else ''}" + " " * 20)
                again = input("  Keep it? [Enter = yes, r = redo] ").strip().lower()
                if again != "r":
                    break
            np.savez_compressed(out / f"{name}_{rnd}.npz", xyz=x, fs=FS, lsb_per_g=LSB_PER_G, label=name,
                                scratch=scratch, dropouts=lost, recorded=time.time(), person=person, hand=hand)
    print(f"\nAll done: {len(acts) * len(rounds)} clips in {out}. Next: python3 tools/tune.py")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["check", "session"])
    ap.add_argument("--port", default=port_guess())
    ap.add_argument("--rounds", default="1,2", help="which rounds to record, e.g. 1,2 or 2")
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--into", help="an existing session folder name, to redo clips in it")
    ap.add_argument("--only", help="comma-separated activities to record, e.g. fingers,waving")
    ap.add_argument("--person", help="anonymous ID like P03 (session only; never a name)")
    ap.add_argument("--hand", choices=["right", "left"], help="the hand wearing the board (session only)")
    a = ap.parse_args()
    if a.mode == "session":
        import re
        if not (a.person and re.fullmatch(r"P\d{2,3}", a.person)):
            sys.exit("Give an anonymous ID: --person P01, P02, ... (not a name)")
        if not a.hand:
            sys.exit("Say which hand wears the board: --hand right or --hand left")
    band = Band(a.port)
    try:
        band.start()
        if a.mode == "check":
            check(band)
        else:
            session(band, [int(r) for r in a.rounds.split(",")], a.seconds, a.person, a.hand, a.into, a.only and a.only.split(","))
    finally:
        band.stop()


if __name__ == "__main__":
    main()
