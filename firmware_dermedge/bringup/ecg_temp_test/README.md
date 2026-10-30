# ECG + LM35 bring-up test

Checks the AD8232, the LM35 and the touch pad on their own, before the band firmware.

| Part | ESP32 |
|---|---|
| AD8232 OUTPUT / LO+ / LO− / SDN | 32 / 33 / 34 / 3V3 (3.3v pin on **3V3**) |
| LM35 Vout (flat face toward you: left 5V, middle Vout, right GND) | 35 |
| Touch pad OUT1 | 27 |

```
./idf.sh -p /dev/cu.SLAB_USBtoUART flash monitor   # one line per ECG sample, a summary each second
python3 tools/plot.py                              # live ECG plot (close the monitor first)
```

| Check | Expected |
|---|---|
| LM35 in the air, then on the inside of the wrist | room temperature, then 31-34 °C within a minute |
| Electrodes off | `leads OFF` |
| Electrodes on, sitting still | a spike per beat, `hr` within a few bpm of your pulse |
| Touch the pad | `# touch pressed` / `# touch released` |

With electrodes on, run from a power bank or a laptop on battery, never one plugged into the wall.
