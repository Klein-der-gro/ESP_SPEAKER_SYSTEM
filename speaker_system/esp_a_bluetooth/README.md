# ESP A - Bluetooth speaker

This is the existing Bluetooth/DSP/OLED/encoder firmware with the UART2 sample-rate link added. Its A2DP-to-I2S audio path and existing DAC remain unchanged.

## Wiring

Keep the existing PCM5102, encoder, and OLED wiring unchanged. Connect ESP A GPIO 17 (UART2 TX) to ESP B GPIO 16 (UART2 RX), and connect the ESP A and ESP B grounds. ESP A should use a WROOM module; GPIO 16/17 can be occupied by PSRAM on some WROVER modules.

## Build and flash

Open this folder as the ESP-IDF project, select the ESP32 target and the correct COM port, then run `idf.py build` and `idf.py -p PORT flash monitor` (or use the VS Code ESP-IDF build/flash controls).

## Expected serial output

The existing Bluetooth and OLED logs should appear, plus `LINK_TX: UART2 rate link ready`. ESP A sends 44100 Hz at startup, sends changes when the A2DP sample rate changes, and repeats the latest rate once per second.

The last OLED page displays the firmware build name. Edit `FIRMWARE_VERSION_NAME` in `../common/protocol/include/protocol.h` to change it for ESP A and ESP B.
