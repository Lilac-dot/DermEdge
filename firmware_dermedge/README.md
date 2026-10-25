# DermEdge firmware (ESP-IDF)

Two ESP-IDF projects that talk to the laptop hub (`app/server.py`) over local WiFi:

| Project | Board | What it does |
|---|---|---|
| `esp32cam_screen/` | AI-Thinker ESP32-CAM | Captures a photo every 3 s, screens it for eczema with a 300 KB INT8 model on TFLite Micro (or asks the hub), lights the red LED, and sends positives to the hub for the full analysis (eczema vs look-alikes, severity, flare risk). |
| `wristband/` | Any plain ESP32 dev board | Samples GSR, skin temperature, PPG and motion at the WESAD rates and posts 30 s windows to the hub's stress model; posts skin-moisture readings. Simulated by default: the BOOT button toggles calm/stressed. |

`components/dermedge_net/` is shared: WiFi connection and the HTTP calls to the hub.

Wristband sensors, wiring (including the thermistor), datasets and test steps: `HARDWARE_GUIDE.md`.

**Status:** both projects build with ESP-IDF v5.4.1 (wristband 866 KB, camera 1.4 MB).

## Build and flash with the VS Code ESP-IDF extension

1. Install the **ESP-IDF** extension (Espressif) in VS Code and run
   **ESP-IDF: Configure ESP-IDF Extension** → Express → ESP-IDF **v5.2 or newer**.
   It downloads ESP-IDF and its tools (a few GB, ~15 min).
2. **File → Open Folder** → `firmware_dermedge/esp32cam_screen` (or `wristband`).
   Open each project on its own, not the parent folder.
3. Status bar: set the target to **esp32**, and pick the board's serial port.
4. Status bar ⚙ **SDK Configuration Editor** (menuconfig):
   - **DermEdge network**: WiFi name and password, and the hub URL
     `http://<laptop IP>:8000`. A macOS hotspot (Internet Sharing) usually gives the laptop
     `192.168.2.1`; otherwise see System Settings → Wi-Fi → Details.
   - **DermEdge camera** / **DermEdge wristband**: the options below.
5. **Build** (🔧), **Flash** (⚡), **Monitor** (🖥) — or the single **Build, Flash and Monitor** button.
   The first build fetches `esp32-camera` and `esp-tflite-micro` through the component manager.
6. On the laptop: `python3 app/server.py`, then open http://localhost:8000.

### ESP32-CAM wiring for flashing
The AI-Thinker board has no USB. Use the ESP32-CAM-MB adapter, or a USB-serial adapter
(5V→5V, GND→GND, TX→U0R, RX→U0T) with **GPIO0 tied to GND while flashing**; remove it and
press reset to run.

### Camera options (DermEdge camera)
- **Where the screen runs**: *on device* (default; works without WiFi) or *on the hub*
  (every frame goes to the laptop's CLIP screen, AUC 0.93 on real phone photos).
- Capture period, positives needed in a row, cooldown, threshold, flash LED.
- **Swap red and blue**: leave on. esp32-camera decodes to B,G,R; the model wants R,G,B.

### Wristband options (DermEdge wristband)
- **GSR source**: simulated, or a Grove GSR module on ADC1 channel 6 (GPIO34).
  The Grove conversion is approximate; check the module's no-skin reading (see `signals.c`).
- **Moisture patch**: simulated (6% drier on the eczema patch), the ESP32's touch pins as crude
  capacitive electrodes (GPIO4 on the eczema-prone area, GPIO15 on healthy skin), or off.
- **Skin temperature**: simulated, an NTC thermistor, or an LM35, on ADC1 channel 7 (GPIO35).
  - Thermistor (our choice): 3V3 → 10k resistor → GPIO35 → thermistor → GND. Set its 25 °C
    resistance and B value in the same menu (defaults 10k, B3950).
  - LM35: flat side facing you, left pin to 5V/VIN (it needs at least 4 V, not 3V3), middle pin
    to GPIO35, right pin to GND.
  - The start-up log prints the first reading; wrist skin should be about 30-35 C.
- PPG and accelerometer are simulated until those sensors are wired.
- Start calm: the hub calibrates the stress model on the first 3 windows (90 s).
