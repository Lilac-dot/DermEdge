"""Live plot of the ECG + LM35 bring-up test's serial stream.

    python3 tools/plot.py                         # /dev/cu.SLAB_USBtoUART
    python3 tools/plot.py /dev/cu.usbserial-0001

Shows the last 5 s of ECG (grey where an electrode is off) and the latest temperature and heart
rate in the title. Close idf.py monitor first: only one program can hold the port.
"""
import sys
from collections import deque

import matplotlib.pyplot as plt
import serial
from matplotlib.animation import FuncAnimation

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.SLAB_USBtoUART"
FS = 250
WINDOW = 5 * FS

ecg = deque([0] * WINDOW, maxlen=WINDOW)
leads = deque([1] * WINDOW, maxlen=WINDOW)
summary = ["waiting for data..."]

port = serial.Serial(PORT, 115200, timeout=0)
buf = b""

fig, ax = plt.subplots(figsize=(10, 4))
t = [i / FS for i in range(WINDOW)]
(line_on,) = ax.plot(t, list(ecg), color="#0F7C74", lw=1)
(line_off,) = ax.plot(t, list(ecg), color="#BBBBBB", lw=1)
ax.set_xlabel("seconds")
ax.set_ylabel("AD8232 output (mV)")


def update(_):
    global buf
    buf += port.read(port.in_waiting or 1)
    *lines, buf = buf.split(b"\n")
    for raw in lines:
        text = raw.decode(errors="replace").strip()
        if text.startswith("E "):
            parts = text.split()
            if len(parts) == 3 and parts[1].lstrip("-").isdigit():
                ecg.append(int(parts[1]))
                leads.append(int(parts[2]))
        elif text.startswith("#"):
            summary[0] = text.lstrip("# ")
            print(summary[0])
    y = list(ecg)
    line_on.set_ydata([v if on else float("nan") for v, on in zip(y, leads)])
    line_off.set_ydata([float("nan") if on else v for v, on in zip(y, leads)])
    lo, hi = min(y), max(y)
    ax.set_ylim(lo - 50, hi + 50)
    ax.set_title(summary[0])
    return line_on, line_off


anim = FuncAnimation(fig, update, interval=50, cache_frame_data=False)
plt.tight_layout()
plt.show()
