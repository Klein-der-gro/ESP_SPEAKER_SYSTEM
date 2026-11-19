# ESP B - Wi-Fi hub (M0 skeleton)

This milestone only verifies the independent ESP-IDF project and boot message. The SoftAP, I2S receiver, UART rate receiver, UDP audio, and control protocol are later milestones.

## Wiring

For M0, connect only USB power. When the UART/I2S milestones are implemented, connect ESP A BCK GPIO 18 to ESP B GPIO 18, WS GPIO 19 to GPIO 19, data GPIO 23 to GPIO 23, UART2 RX GPIO 16 to ESP A TX GPIO 17, and common ground. Keep the I2S wires short.

## Build and flash

Open this folder as the ESP-IDF project, select the ESP32 target and this ESP's COM port, then run `idf.py build` and `idf.py -p PORT flash monitor`.

## Expected serial output

`WIFI_HUB: ESP B Wi-Fi hub ready (milestone M0), firmware: MR-M0.2`
