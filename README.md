# ESP speaker system

The four-device multiroom firmware, wiring diagram, flash order, and known
hardware-test limitations are documented in
[`speaker_system/README.md`](speaker_system/README.md).

Open each ESP-IDF project folder separately in VS Code:

- [`esp_a_bluetooth/`](speaker_system/esp_a_bluetooth/) — Bluetooth source,
  controls, and display.
- [`esp_b_wifi_hub/`](speaker_system/esp_b_wifi_hub/) — Wi-Fi hub and shared
  audio DSP.
- [`esp_box/`](speaker_system/esp_box/) — one firmware for the LEFT and RIGHT
  speaker boxes.
