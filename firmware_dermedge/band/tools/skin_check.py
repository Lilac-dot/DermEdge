"""
Checks the band's GSR module and thermistor over USB, for wiring them up:

    python3 tools/skin_check.py           # 15 s of live readings, with a verdict per sensor
    python3 tools/skin_check.py --cal     # calibrate the GSR's no-skin reading (electrodes OFF)

The band keeps running normally; this just turns on its once-a-second "skin" lines.
"""
import argparse
import re
import sys
import time
from pathlib import Path

import serial


def port_guess():
    for p in ["/dev/cu.SLAB_USBtoUART", "/dev/cu.usbserial-0001"]:
        if Path(p).exists():
            return p
    return "/dev/cu.SLAB_USBtoUART"


def open_band(port):
    """Opens the port and waits for the band to answer: on some boards opening it restarts the
    ESP32, which then takes a couple of seconds to boot and would miss a command sent at once."""
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.1
    s.dtr = s.rts = False
    s.open()
    end = time.time() + 10
    while time.time() < end:
        s.write(b"raw off\nskin on\n")
        if any(l.startswith("skin:") for l in lines(s, 1.2)):
            s.write(b"skin off\n")
            time.sleep(0.3)
            s.reset_input_buffer()
            return s
    sys.exit("No answer from the band. Is it plugged in and running the band firmware?")


def lines(s, seconds):
    end, buf = time.time() + seconds, b""
    while time.time() < end:
        buf += s.read(4096)
        *done, buf = buf.split(b"\n")
        for l in done:
            yield l.decode(errors="replace").strip()


def check(s, seconds):
    print(f"Reading the skin sensors for {seconds} s (put the GSR electrodes on two fingers, tape the thermistor to skin)...\n")
    s.write(b"skin on\n")
    gsr, temp = [], []
    for l in lines(s, seconds):
        if l.startswith("skin:"):
            print(" ", l[5:].strip())
            m = re.search(r"([\d.]+) uS, ([\d.]+) C", l)
            if m:
                gsr.append(float(m.group(1))), temp.append(float(m.group(2)))
            if "-> on skin" not in l:
                gsr.append(None)
            if "| thermistor" in l and "-> ok" not in l:
                temp.append(None)
    s.write(b"skin off\n")
    ok_g = [g for g in gsr if g is not None]
    ok_t = [t for t in temp if t is not None]
    print("\nGSR:", f"OK, {min(ok_g):.2f}-{max(ok_g):.2f} uS (a resting hand is usually 0.5-10)" if len(ok_g) > len(gsr) / 2 else
          "NOT READING. 'NOTHING CONNECTED': check SIG to GPIO 32, VCC to 3V3, GND. 'not on skin': electrodes snug on two fingers?")
    print("Skin temperature:", f"OK, {min(ok_t):.1f}-{max(ok_t):.1f} C (skin is usually 28-35; give it a minute to settle)" if len(ok_t) > len(temp) / 2 else
          "NOT READING. 'unplugged or shorted': check 3V3 -> 10 kOhm -> GPIO 34 -> thermistor -> GND. 'settling': wait a few seconds.")


def calibrate(s):
    input("GSR calibration. Take the electrodes OFF the skin (touching nothing), then press Enter. ")
    s.write(b"gsr cal\n")
    for l in lines(s, 3):
        if "GSR cal" in l:
            print(" ", re.sub(r"^.*?band: ", "", l))
            return
    print("  No answer from the band. Is it plugged in and running the band firmware?")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=port_guess())
    ap.add_argument("--seconds", type=int, default=15)
    ap.add_argument("--cal", action="store_true", help="calibrate the GSR's no-skin reading")
    a = ap.parse_args()
    try:
        s = open_band(a.port)
    except serial.SerialException as e:
        sys.exit(f"Can't open {a.port}: {e}. Close any serial monitor using it.")
    try:
        calibrate(s) if a.cal else check(s, a.seconds)
    finally:
        s.close()


if __name__ == "__main__":
    main()
