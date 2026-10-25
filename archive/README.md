# Archive

Code DermEdge no longer builds or ships, kept for reference.

- `gsr_wristband_wifi/`: the first wearable, an ESP32 that sampled GSR, skin temperature, PPG
  and motion at the WESAD rates and posted 30 s windows to the hub's stress model over Wi-Fi
  (simulated sensors by default). The DermEdge band (`firmware_dermedge/band/`, Bluetooth to the
  app) replaced it. Its GSR and stress code comes back into the band when the GSR sensor is
  added. It still builds with `idf.py build` from its folder.

## gsr_wristband_wifi options (menuconfig: DermEdge wristband)
- **GSR source**: simulated, or a Grove GSR module on ADC1 channel 6 (GPIO34).
  The Grove conversion is approximate; check the module's no-skin reading (see `signals.c`).
- **Moisture patch**: simulated (6% drier on the eczema patch), the ESP32's touch pins as crude
  capacitive electrodes (GPIO4 on the eczema-prone area, GPIO15 on healthy skin), or off.
- Skin temperature, PPG and accelerometer are simulated until those sensors are wired.
- Start calm: the hub calibrates the stress model on the first 3 windows (90 s).
