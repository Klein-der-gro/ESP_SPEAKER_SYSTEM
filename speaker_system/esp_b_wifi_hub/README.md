# ESP B - Wi-Fi and DSP hub

ESP B receives raw stereo I2S and UART2 control frames from ESP A, runs shared
DSP, starts the `SPEAKER-NET` access point, and sends audio to registered
speaker boxes by unicast UDP.

## Connections

- ESP A BCLK GPIO 18 -> GPIO 18; WS GPIO 19 -> GPIO 19; data GPIO 23 -> GPIO
  23; GND -> GND.
- ESP A UART2 TX GPIO 17 -> ESP B RX GPIO 16 (115200 baud); connect GND.
- Keep I2S wires short. GPIO 16 may not be available on modules that use it
  for PSRAM.

## Network

Default settings are SSID `SPEAKER-NET`, password `change-me-1234`, channel
1, and hub address `192.168.4.1`. The password is a public example, not a
personal credential. Change shared settings in
`../common/protocol/include/protocol.h` and rebuild ESP B and both boxes
together.

## Build and flash

Open this folder as its own ESP-IDF project, select ESP32 and ESP B's COM
port, then run **Build, Flash and Monitor**. Verify `AP up`,
`I2S slave RX ready`, and both box registrations in the monitor. See
[`../README.md`](../README.md) for the full power-up sequence and safety
notes.
