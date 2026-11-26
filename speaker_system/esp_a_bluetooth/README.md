# ESP A - Bluetooth source

ESP A handles Bluetooth A2DP/AVRCP, the OLED, encoders, playback controls, and
the visualizer. In hub-DSP mode it sends raw stereo I2S to ESP B and forwards
sample-rate and DSP-control updates over UART2.

## Connections

- PCM5102 BCLK/LCK/DIN: GPIO 18/19/23.
- OLED SDA/SCL: GPIO 21/22.
- ESP A -> ESP B: I2S GPIO 18/19/23 and GND; UART2 TX GPIO 17 -> ESP B RX
  GPIO 16, 115200 baud.
- Disconnect the ESP A DAC amplifier while using hub-DSP mode. ESP A's DAC
  receives raw, unprocessed audio.

## Build, flash, and monitor

Open this folder as its own ESP-IDF project. Select ESP32, the board's COM
port, then run **Build, Flash and Monitor**. At startup expect
`firmware: MR-M0.2` and `UART2 rate link ready`.

The audio and DSP handoff requires ESP B to be running. Flash all device
projects as described in [`../README.md`](../README.md).
