# Handoff

**Last updated:** 2026-11-26  
**Project:** Four-board ESP32 Bluetooth and multiroom speaker system  
**Status:** Firmware implementation builds; hardware integration is not yet verified.

## Physical pin map

| Board | GPIO | Connect to / function |
|---|---:|---|
| ESP A | 18 | I2S BCLK to PCM5102 and ESP B |
| ESP A | 19 | I2S WS/LCK to PCM5102 and ESP B |
| ESP A | 23 | I2S data out to PCM5102 DIN and ESP B DIN |
| ESP A | 17 | UART2 TX -> ESP B GPIO 16 RX |
| ESP A | 21 | OLED SDA |
| ESP A | 22 | OLED SCL |
| ESP A | 33 / 27 / 14 | Encoder 1 CLK / DT / switch |
| ESP A | 32 / 25 / 26 | Encoder 2 CLK / DT / switch |
| ESP B | 18 / 19 / 23 | I2S BCLK / WS / data in from ESP A |
| ESP B | 16 | UART2 RX from ESP A GPIO 17 |
| Each box | 18 / 19 / 23 | I2S BCLK / WS / data out to PCM5102 |
| Each box | 27 | Role select: open = LEFT; connect to GND = RIGHT |

Use a common ground between ESP A and ESP B. ESP A is the I2S clock master;
ESP B is the I2S slave receiver. Each speaker box is an I2S master for its
DAC. The firmware uses 16-bit stereo I2S. Check the actual board pinout before
wiring; some ESP32 modules reserve GPIO 16/17 for PSRAM. Keep I2S leads short.

## Update rule

Whenever project code, configuration, or documentation changes, update this
file in the same work session. Refresh the date, summarize the change and its
validation, and keep the unverified items explicit.

## Current architecture

- **ESP A** handles Bluetooth audio, controls, OLED, settings, and visualizer
  input. It sends raw stereo I2S to ESP B and forwards sample-rate and DSP
  parameters over UART2.
- **ESP B** receives I2S and UART, runs shared DSP, starts the `SPEAKER-NET`
  Wi-Fi access point, registers boxes, and sends audio over UDP.
- **The LEFT and RIGHT boxes** automatically join the hub, select their
  respective stereo channel, synchronize packet playback, apply an LR4
  crossover, and output to their DACs.
- One box firmware image is used for both roles. GPIO 27 open selects LEFT;
  connect GPIO 27 to GND for RIGHT.

## Important firmware settings

- Shared definitions are in `common/protocol/include/protocol.h`.
- Firmware label/version: `MR-M0.2` / protocol version `2`.
- ESP B access point: SSID `SPEAKER-NET`, example password
  `change-me-1234`, channel 1, hub address `192.168.4.1`, country `AT`.
  The password is public example text, not a secret.
- ESP A -> ESP B UART2 runs at 115200 baud. Frames start with `0xA5`, carry
  type/length/payload, and end with CRC-8 (polynomial `0x07`). Sample-rate
  payloads are 4 bytes; DSP-parameter payloads are 18 bytes.
- Audio is 16-bit stereo, 256 frames per packet. Each UDP audio datagram is
  1056 bytes total (32-byte header plus 1024-byte PCM payload). Audio uses
  UDP port 5004; box registration and clock sync use UDP port 5005.
- The hub uses a 120 ms box playout delay. Boxes send registration heartbeats
  and clock-sync requests automatically; no static IP configuration is
  needed.
- `DSP_ON_HUB` is enabled: ESP A sends raw audio and controls, ESP B does the
  shared DSP/limiting, and boxes do channel selection plus crossover.
- Box crossover is LR4 at 2500 Hz. DAC left is the high-frequency output;
  DAC right is the low-frequency output. Confirm the crossover against the
  actual drivers before connecting amplifiers.

## Key files

- [`common/protocol/include/protocol.h`](common/protocol/include/protocol.h) —
  shared protocol, firmware version, network settings, and packet definitions.
- [`esp_a_bluetooth/main/main.c`](esp_a_bluetooth/main/main.c) and
  [`esp_a_bluetooth/main/link_tx.c`](esp_a_bluetooth/main/link_tx.c) — ESP A
  audio path and UART transmitter.
- [`esp_b_wifi_hub/main/main.c`](esp_b_wifi_hub/main/main.c) and the other
  files under `esp_b_wifi_hub/main/` — ESP B hub, AP, UART receiver, DSP input,
  registration, and UDP sender.
- [`esp_box/main/main.c`](esp_box/main/main.c) and
  [`esp_box/main/crossover.c`](esp_box/main/crossover.c) — box networking,
  buffering, synchronization, playback, and crossover.
- [`tools/udp_listener.py`](tools/udp_listener.py) — optional PC packet
  monitor when connected to the hub WLAN.
- [`README.md`](README.md) — wiring, safety, flash order, and troubleshooting.

The previously tagged path
`speaker_system/esp_a_bluetooth/esp_b_wifi_hub/main/main.c` does not exist.
The ESP B main file is `speaker_system/esp_b_wifi_hub/main/main.c`; ESP A's
main file is `speaker_system/esp_a_bluetooth/main/main.c`.

## Configuration and safety

- Version label: `MR-M0.2`.
- Hub SSID: `SPEAKER-NET`; example password: `change-me-1234`; hub IP:
  `192.168.4.1`; Wi-Fi country: `AT`.
- The example password is public and must not be used as a personal secret.
  Update shared settings in `common/protocol/include/protocol.h`, then rebuild
  and flash ESP B and both boxes together.
- Disconnect the amplifier from ESP A's DAC during hub-DSP testing; its DAC
  output is raw, unprocessed audio.
- Box crossover defaults to 2500 Hz LR4. Verify it against the actual drivers
  and amplifiers before connecting speaker hardware.
- Wiring and complete flash directions are in [`README.md`](README.md).
- The repository [`.gitignore`](../.gitignore) excludes generated `build/`
  directories, local `sdkconfig` files, and `.vscode/` settings. If any were
  staged before being ignored, unstage them before committing; ignore rules do
  not remove already-staged files.

## Verification completed

- ESP A, ESP B, and speaker-box projects each built successfully with ESP-IDF
  6.1.
- `tools/udp_listener.py` passed Python syntax parsing and its `--help` check.
- `git diff --check` completed without whitespace errors.
- Pin assignments and protocol constants in this handoff were checked against
  the current firmware source.

## Not yet verified

- The new multi-ESP firmware has not been flashed to all four boards. A
  previous ESP A flash predates this implementation.
- End-to-end Bluetooth-to-speaker playback, AP joining on the actual boards,
  Wi-Fi throughput/recovery, packet synchronization, sound quality, and
  extended playback still require hardware testing.
- Host-side C tests were not run because no native C compiler was available.
- Do not push the current changes unless explicitly requested.

## Next steps

1. Review the actual board pinouts and wire ESP A to ESP B as documented.
2. Flash ESP A, ESP B, and the same box binary to both boxes; set the RIGHT
   role jumper before powering that box.
3. Keep ESP A's amplifier disconnected, monitor each board, and verify AP
   startup, box registration, and incoming audio packets.
4. Test playback at low amplifier gain only after confirming the crossover
   against the driver datasheets. Record hardware results here.

## Handoff update log

- **2026-11-26:** Added the source-verified pin map, protocol/network constants,
  signal routing and safety reminders, and Git generated-file staging guidance.
