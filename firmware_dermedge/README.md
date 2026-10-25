# DermEdge firmware (ESP-IDF)

| Project | Board | What it does |
|---|---|---|
| `band/` | Plain ESP32 dev board + 1.8" ST7735 TFT + add-on board (ADXL345, touch pads) | **The DermEdge band.** Detects scratching from the accelerometer, starts and stops recordings from a touch pad or the app, shows its state in the website's palette and fonts, and talks to the app over encrypted Bluetooth LE. See [band/README.md](band/README.md). |
| `esp32cam_screen/` | AI-Thinker ESP32-CAM | Captures a photo every 3 s, screens it for eczema with a 300 KB INT8 model on TFLite Micro (or asks the hub), lights the red LED, and sends positives to the hub over Wi-Fi for the full analysis (eczema vs look-alikes, severity, flare risk). |

`components/dermedge_net/` is the camera's Wi-Fi connection and HTTP calls to the hub. The
first Wi-Fi GSR wristband is in `../archive/gsr_wristband_wifi/`.

**Status:** both build with ESP-IDF v5.4.1 (band 625 KB, camera 1.4 MB). The band runs on a
breadboard; the app side (pairing, start/stop, relaying readings to the hub) is in progress,
see [../docs/DERMEDGE_APP_GUIDE.md](../docs/DERMEDGE_APP_GUIDE.md).

## The band

```
cd firmware_dermedge/band
./idf.sh build && ./idf.sh -p /dev/cu.SLAB_USBtoUART flash monitor
sh test/run.sh       # host tests: scratch detector, touch gestures, state machine
sh test/preview.sh   # renders every screen to test/preview.png
```

`idf.sh` activates ESP-IDF v5.4.1 installed by Espressif's installer (`~/.espressif`); with the
VS Code extension, open `band/` and use Build / Flash instead.

## The camera: build and flash with the VS Code ESP-IDF extension

1. Install the **ESP-IDF** extension (Espressif) in VS Code and run
   **ESP-IDF: Configure ESP-IDF Extension** → Express → ESP-IDF **v5.2 or newer**.
   It downloads ESP-IDF and its tools (a few GB, ~15 min).
2. **File → Open Folder** → `firmware_dermedge/esp32cam_screen`.
   Open the project on its own, not the parent folder.
3. Status bar: set the target to **esp32**, and pick the board's serial port.
4. Status bar ⚙ **SDK Configuration Editor** (menuconfig):
   - **DermEdge network**: WiFi name and password, and the hub URL
     `http://<laptop IP>:8000`. A macOS hotspot (Internet Sharing) usually gives the laptop
     `192.168.2.1`; otherwise see System Settings → Wi-Fi → Details.
   - **DermEdge camera**: the options below.
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
