# ESP_SPEAKER_SYSTEM

The handoff firmware is organized under [`speaker_system/`](speaker_system/).
Each ESP-IDF project is independently buildable; open the specific device
folder in VS Code before building, flashing, or monitoring:

- [`esp_a_bluetooth/`](speaker_system/esp_a_bluetooth/) — Bluetooth source and controls.
- [`esp_b_wifi_hub/`](speaker_system/esp_b_wifi_hub/) — Wi-Fi hub.
- [`esp_box/`](speaker_system/esp_box/) — left/right speaker firmware.

Shared components are in [`common/`](speaker_system/common/). See the
[multiroom README](speaker_system/README.md) for wiring and milestone status.
