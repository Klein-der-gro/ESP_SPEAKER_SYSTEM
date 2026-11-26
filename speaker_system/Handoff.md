# Handoff

**Last updated:** 2026-11-26  
**Project:** Four-board ESP32 Bluetooth and multiroom speaker system  
**Status:** ESP A, ESP B, and the LEFT box have been flashed. COM5 is currently
on the LEFT box; monitoring confirms that it boots and registers, but received
PCM was exactly zero in the captured run. ESP B captured nonzero I2S input and
zero post-DSP output while DSP controls were stale (`params=0 stale=1`). ESP A
now sends DSP controls independently of Bluetooth audio; this update still
needs end-to-end UART and Bluetooth verification. Physical sound output remains
unverified.

**Current source:** Firmware label `NR-5.0.26.11`. The ESP B UART parser
frame-type fix is in source and was built successfully; it still needs to be
flashed and verified on hardware.

## Physical pin map

| Board | GPIO | Connect to / function |
|---|---:|---|
| ESP A | 18 | I2S BCLK to PCM5102 and ESP B |
| ESP A | 19 | I2S WS/LCK to PCM5102 and ESP B |
| ESP A | 23 | I2S data out to PCM5102 DIN and ESP B DIN |
| ESP A | 17 | UART2 TX -> ESP B GPIO 16 RX |
| ESP A | 4 | UART2 RX <- ESP B GPIO 4 TX (box-status return) |
| ESP A | 21 | OLED SDA |
| ESP A | 22 | OLED SCL |
| ESP A | 33 / 27 / 14 | Encoder 1 CLK / DT / switch |
| ESP A | 32 / 25 / 26 | Encoder 2 CLK / DT / switch |
| ESP B | 18 / 19 / 23 | I2S BCLK / WS / data in from ESP A |
| ESP B | 16 | UART2 RX from ESP A GPIO 17 |
| ESP B | 4 | UART2 TX -> ESP A GPIO 4 RX (box-status return) |
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

## Handoff checklist

Before handing the project to a new operator or returning after a break:

- Read this file and [`README.md`](README.md).
- Verify that the source and flashed firmware use the same
`FIRMWARE_VERSION_NAME` and protocol constants.
- Confirm ESP A GPIO 17 -> ESP B GPIO 16, ESP B GPIO 4 -> ESP A GPIO 4, and
common ground.
- Confirm ESP A I2S GPIO 18/19/23 and ESP B I2S GPIO 18/19/23 share the same
clock/data direction.
- Confirm no amplifier is connected while testing raw ESP A DAC output.
- Set the LEFT box GPIO 27 open/pulled high and the RIGHT box GPIO 27 to GND.
- Flash ESP A, ESP B, LEFT, and RIGHT as separate ESP-IDF projects.
- Perform a Bluetooth pairing check, a PCM-control check, and a physical DAC
output check before leaving amplifiers connected.

Do not mark the system complete based only on successful builds. Completion
requires both monitor evidence and safe playback through the speaker hardware.

## System data flow

```text
Bluetooth phone -> ESP A A2DP stream -> ESP A I2S task
                                    -> 16-bit stereo I2S to ESP B
ESP A UART2 -> sample rate/DSP settings -> ESP B UART receiver
ESP B I2S receiver -> shared DSP -> UDP audio queue -> LEFT/RIGHT boxes
ESP B control UDP -> box registration/clock sync -> ESP B
ESP B box-status UART -> ESP A OLED
```

ESP A has two independent control paths: A2DP/AVRCP carries playback and
metadata over Bluetooth, while UART2 carries sample rate and DSP settings.
ESP B does not receive DSP settings through Bluetooth or Wi-Fi. A broken UART
wire or baud/configuration mismatch causes the ESP B stale-control fail-safe
and silent post-DSP audio even if Wi-Fi and I2S counters look healthy.

Each box has another independent control path. UDP port 5005 carries
registration heartbeats and NTP-style clock-sync requests, while UDP port 5004
carries 1056-byte audio packets. Audio and clock/control traffic must reach
the same ESP B address.

## Packet and frame reference

| Transport | From | To | Data |
|---|---|---|---|
| I2S | ESP A | ESP B | 16-bit stereo raw PCM |
| UART2 | ESP A | ESP B | 44.1/48 kHz rate and DSP parameters |
| UART2 | ESP B | ESP A | LEFT/RIGHT status byte once per second |
| UDP 5005 | Boxes | ESP B | Hello, heartbeat, clock sync request |
| UDP 5005 | ESP B | Boxes | Hello ACK, clock sync response |
| UDP 5004 | ESP B | Box | 32-byte header plus 1024 PCM bytes |

An audio packet has 256 stereo frames. The header contains protocol magic,
type, flags, payload length, sequence number, sample rate, frame index, and
hub timestamp. A 120 ms playout delay gives boxes a small UDP jitter buffer.
The box reports packet loss and queue drops separately; both counters should
be evaluated together.

## Startup sequence

1. ESP A initializes NVS, restores DSP settings, starts I2S and Bluetooth,
 initializes UART2, and sends its current sample rate/settings.
2. ESP B initializes NVS and the audio queue, starts Wi-Fi AP mode, UART,
 I2S slave, UDP control, and UDP audio sender.
3. A box boots with GPIO 27 selecting LEFT or RIGHT, joins `SPEAKER-NET`,
 receives an IP, registers on UDP 5005, and starts I2S clock playback.
4. ESP B forwards the registered role bits over UART once per second.
5. ESP A pairs with the phone, receives PCM, and writes raw I2S without
 requiring a box to play for the Bluetooth link to remain active.

## Diagnostics and acceptance signals

Use one ESP-IDF monitor project at a time so the ELF and COM port match the
device being observed.

### ESP A

- `UART2 full-duplex link ready`
- Bluetooth device name `ESP32-Speaker`
- No UART frame-write errors
- OLED status shows `L+` / `R+` only after ESP B status is received

### ESP B

- `AP up: SSID=SPEAKER-NET ... IP=192.168.4.1`
- `I2S slave RX ready`
- `UART2 full-duplex ready`
- `Registered LEFT box` / `Registered RIGHT box`
- Per-second `boxes=`, `packets/s=`, `queue_overruns=`, and `rate=`
- Raw I2S and post-DSP `avg_abs`/`peak` should be nonzero when music plays
- `params=1 stale=0` is required for valid DSP controls

### LEFT and RIGHT boxes

- `Speaker box ready, role=...`
- `Connected to hub Wi-Fi`
- `<ROLE> box registered with hub`
- `registered=1`, no increasing queue drops, and `playing=1`
- `pcm_avg_abs` and `pcm_peak` should be nonzero for a non-silent track

An acceptance run should use a known stereo test track and verify:

1. ESP B has both valid raw I2S and valid DSP controls.
2. ESP B has zero or bounded queue overruns.
3. Each box remains registered for at least several minutes.
4. Left-only and right-only test content reaches the intended DAC channel.
5. No late-packet, watchdog, reset, or I2S-write errors occur.
6. Playback remains stable after Wi-Fi reconnection and a sample-rate change.

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
- Firmware label/version: `NR-5.0.26.11` / protocol version `2`.
- ESP B access point: SSID `SPEAKER-NET`, example password
  `change-me-1234`, channel 1, hub address `192.168.4.1`, country `AT`.
  The password is public example text, not a secret.
- ESP A -> ESP B UART2 runs at 115200 baud. Frames start with `0xA5`, carry
  type/length/payload, and end with CRC-8 (polynomial `0x07`). Sample-rate
  payloads are 4 bytes; DSP-parameter payloads are 18 bytes.
- ESP B returns a CRC-protected UART box-status byte to ESP A once per second.
  Connect ESP B GPIO 4 TX -> ESP A GPIO 4 RX and share GND. On ESP A's OLED
  home page, `L+` and `R+` mean ESP B recently received that role's UDP
  registration heartbeat; `L-` / `R-` mean it has not, or the UART status
  frame is stale. Page 8 (`BOX STATUS`) displays whether the status link is
  fresh, the connected-box count, and each role; `-- / 2` means the status
  frame is stale or unavailable. Registration indicators do not confirm audio
  playback.
- Audio is 16-bit stereo, 256 frames per packet. Each UDP audio datagram is
  1056 bytes total (32-byte header plus 1024-byte PCM payload). Audio uses
  UDP port 5004; box registration and clock sync use UDP port 5005.
- The hub uses a 120 ms box playout delay. Boxes send registration heartbeats
  and clock-sync requests automatically; no static IP configuration is
  needed.
- The box uses a 1 kHz FreeRTOS tick to schedule sub-10 ms audio packets
  without keeping the idle core from running.
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
- [`speaker_system.code-workspace`](speaker_system.code-workspace) — VS Code
  workspace containing the three independent ESP-IDF projects.

The previously tagged path
`speaker_system/esp_a_bluetooth/esp_b_wifi_hub/main/main.c` does not exist.
The ESP B main file is `speaker_system/esp_b_wifi_hub/main/main.c`; ESP A's
main file is `speaker_system/esp_a_bluetooth/main/main.c`.

## Configuration and safety

- Version label: `NR-5.0.26.11`.
- Hub SSID: `SPEAKER-NET`; example password: `change-me-1234`; hub IP:
  `192.168.4.1`; Wi-Fi country: `AT`.
- The example password is public and must not be used as a personal secret.
  Update shared settings in `common/protocol/include/protocol.h`, then rebuild
  and flash ESP B and both boxes together.
- The LEFT box and ESP A both reported 4 MB physical flash at boot, and their
  project defaults/local configurations now match. ESP B also confirmed 4 MB
  at boot after flashing with a 4 MB image.
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

- Fixed the ESP B UART frame parser to use `frame[0]` for frame type instead of
  the payload-length byte. The ESP B project rebuilt successfully and produced
  `build/esp_b_wifi_hub.bin`; the corrected firmware has not yet been flashed
  or hardware-tested.
- Bumped the shared firmware label to `NR-5.0.26.11`. Rebuilt ESP A, ESP B,
  and the speaker-box project successfully with ESP-IDF 6.1.
- Updated ESP A OLED page 8 to show `STATUS LINK: OK` or `STATUS LINK: LOST`
  above the current LEFT/RIGHT registration statuses. The display change
  compiled in the ESP A build; hardware display behavior is not yet verified.
- ESP A, ESP B, and speaker-box projects each built successfully with ESP-IDF
  6.1.
- ESP A, ESP B, and the box were rebuilt with full-duplex box-status reporting
  and immediate STA connection. The box build now generates flash commands
  declaring 4 MB, matching the reported chip detection.
- ESP B and speaker-box projects rebuilt successfully after moving Wi-Fi
  maximum TX-power configuration until after `esp_wifi_start()`.
- Box STA connection now starts on `WIFI_EVENT_STA_START`, retries after a
  disconnect, and logs disconnect reasons/IP to diagnose AP and password
  problems.
- Box project flash-size configuration now matches the reported 4 MB device.
- Flashed ESP A on COM5. Boot confirmed 4 MB flash, found the OLED at I2C
  address `0x3C`, and initialized UART2 full-duplex at 115200 baud on TX GPIO
  17 / RX GPIO 4.
- Fixed ESP A's UART startup failure: its RX ring buffer was 128 bytes, equal
  to the UART FIFO size, but ESP-IDF requires a larger buffer. Increased it to
  256 bytes; the flashed firmware now reports `UART2 full-duplex link ready`.
- Bluetooth A2DP starts and pairing is enabled. The monitor showed A2DP
  connection attempts failing/disconnecting, so phone pairing and Bluetooth
  audio are not yet verified.
- Flashed the LEFT box on COM5. Boot confirmed 4 MB flash; it joined
  `SPEAKER-NET` at `192.168.4.2`, registered with the hub, and reported
  `playing=1` with zero queue drops over an approximately 39-second monitor
  capture.
- Flashed ESP B on COM5 (MAC `70:4b:ca:83:4a:a8`). The generated flash command
  used `--flash-size 4MB`; boot confirmed 4 MB flash and the application
  started successfully. During a roughly 70-second monitor capture, the LEFT
  box registered and audio traffic sustained about 173 packets/s at 44.1 kHz
  with zero queue overruns. DSP average/maximum was about 626/804 us against
  a 2902 us half-chunk budget. No runtime errors, watchdog warnings, or resets
  were observed.
- Added and flashed temporary PCM average-absolute/peak diagnostics on the
  LEFT box via COM5. Over an approximately 88-second capture, it stayed
  registered and `playing=1`, but every sampled packet reported
  `pcm_avg_abs=0` and `pcm_peak=0`; queue drops remained zero. Thus packets
  reach the box, but the sampled payload was digital silence while music was
  reported playing. Follow-up ESP B diagnostics on COM5 found nonzero raw I2S
  (`avg_abs` about 260-338, `peak` about 1034-1081) but zero post-DSP samples.
  The logs consistently reported `params=0 stale=1`; the UART receiver is not
  getting valid DSP controls, so the hub's stale-control fail-safe is muting
  audio before forwarding it. Audio traffic remained about 173 packets/s with
  zero hub queue overruns and DSP within budget. Physical DAC output remains
  untested.
- Correctly monitored COM5 from the `esp_box` project. The LEFT box booted
  without an assertion, connected to the hub, registered, and played incoming
  packets; its PCM diagnostics still read `pcm_avg_abs=0 pcm_peak=0`.
  Monitoring from the ESP B project while the box was connected had previously
  misidentified the backtrace because it used the wrong ELF.
- Fixed the box audio-receive task stack overflow by increasing its stack from
  4096 to 8192 bytes. Playback now avoids comparing hub timestamps to the
  box's local clock until clock synchronization is valid.
- The first live playback capture showed task-watchdog warnings because a
  100 Hz RTOS tick could not schedule the 5.8 ms audio chunks precisely. The
  box now uses a 1 kHz tick; the final 34-second capture had no watchdog
  warnings or resets, with typical playout error of 1-2 us.
- UART2 is full-duplex: ESP B sends registered LEFT/RIGHT status to ESP A;
  ESP A home OLED renders the two status indicators.
- ESP B rebuilt after the DSP optimization change. Its generated compiler
  command was checked and includes `-O2` for `dsp.c`.
- ESP B defaults to ESP-IDF performance optimization (`-O2`) instead of the
  local debug optimization setting; the shared DSP component also explicitly
  uses `-O2` so existing local debug `sdkconfig` files cannot leave this
  performance-critical code at `-Og`. Shared DSP skips the vocal-band filter
  chain while vocal mode is OFF.
- `tools/udp_listener.py` passed Python syntax parsing and its `--help` check.
- `git diff --check` completed without whitespace errors.
- Pin assignments and protocol constants in this handoff were checked against
  the current firmware source.
- The multi-project VS Code workspace JSON was parsed, and all three listed
  project folders were confirmed to contain a `CMakeLists.txt`; no build was
  run from the new workspace.

## Not yet verified

- ESP B's corrected UART parser has been built but not flashed or
  hardware-verified. It must be flashed before `params=1 stale=0` can be
  confirmed.
- ESP A, ESP B, and the LEFT box have the latest firmware. The RIGHT box
  still needs it.
- Physical sound output and complete Bluetooth-to-speaker playback were not
  confirmed by listening; longer playback and Wi-Fi recovery still need
  hardware testing.
- The box's PCM diagnostic reads exact zero samples. ESP B previously reported
  nonzero I2S but `params=0 stale=1` and zero post-DSP samples. ESP A's control
  sender has been changed to send its current settings at startup and from the
  UI task even when Bluetooth has no audio frames; its build passed, but this
  firmware still needs to be flashed and UART reception verified. Connect
  ESP A GPIO 17 TX -> ESP B GPIO 16 RX with common GND.
- GPIO 4 is only the optional ESP B status return (B GPIO 4 TX -> A GPIO 4 RX);
  it does not carry audio or the DSP controls. The user reports connecting
  GPIO 4 to GPIO 4.
- ESP A's latest logs show Bluetooth connection attempts timing out with HCI
  status `0x0b`. A phone/player connection and nonzero A2DP PCM are not yet
  verified; valid DSP controls alone cannot produce music without an audio
  source.
- The earlier `wifi:enable tsf doesnt work` line was not evidence of failure.
  The flashed firmware subsequently associated, acquired an IP, and registered
  with ESP B.
- Host-side C tests were not run because no native C compiler was available.
- Do not push the current changes unless explicitly requested.

## Next steps

1. Flash the newly built ESP A firmware, with the control wire ESP A GPIO 17
   TX -> ESP B GPIO 16 RX and common GND connected. Confirm B reports fresh DSP
   parameters (`params=1 stale=0`) and nonzero post-DSP samples.
2. Pair a phone/player with ESP A and verify A2DP plus nonzero audio at B and
   nonzero PCM at the box. GPIO 4 may remain connected for status only.
3. Flash the same box binary to the RIGHT box; set its role jumper to GND
   before powering it, then verify both registrations and packet flow.
4. Test physical playback at low amplifier gain only after confirming the
   crossover against the driver datasheets. Record hardware results here.

## Handoff update log

- **2026-11-26:** Added the source-verified pin map, protocol/network constants,
  signal routing and safety reminders, and Git generated-file staging guidance.
- **2026-11-26:** Added a multi-project VS Code workspace so ESP-IDF builds use
  an actual project directory rather than the parent repository folder.
- **2026-11-26:** Fixed ESP-IDF Wi-Fi TX-power setup ordering in ESP B and box
  firmware. The API requires Wi-Fi to be started first; if optional TX-power
  tuning still fails, firmware logs a warning and continues at driver default.
- **2026-11-26:** Addressed ESP B DSP budget warnings by selecting performance
  compiler optimization in its tracked defaults and explicitly compiling the
  DSP component with `-O2`; also skips unused vocal filters when vocal mode is
  OFF. ESP B build passed and the generated `dsp.c` command contains `-O2`.
  Flash and hardware timing verification are still required.
- **2026-11-26:** Started box Wi-Fi connection immediately on station-start,
  added disconnect-reason/IP logging and automatic reconnect; added a
  CRC-protected ESP B-to-ESP A UART status return and OLED `L+`/`R+`
  registration indicators. Updated the box flash-size default to 4 MB based
  on the reported board detection. Rebuilt all three firmware projects;
  wiring and hardware verification are still required.
- **2026-11-26:** Fixed the box audio-receive stack overflow, avoided applying
  hub timestamps before clock sync, and raised the box FreeRTOS tick to 1 kHz
  for audio scheduling. Flashed the LEFT box on COM5; it joined the hub,
  registered, and played with zero queue drops and no watchdog warnings over
  an approximately 39-second monitor capture. Physical sound output and the
  other boards remain unverified.
- **2026-11-26:** Flashed ESP A on COM5. The boot confirmed 4 MB flash, OLED
  discovery at `0x3C`, and UART2 full-duplex initialization. Fixed the RX
  buffer-size check by increasing it from 128 to 256 bytes. Bluetooth starts,
  but captured A2DP attempts failed/disconnected; phone pairing remains
  unverified.
- **2026-11-26:** Flashed ESP B on COM5 after correcting its flash-size
  configuration to 4 MB. Boot confirmed 4 MB, and a roughly 70-second runtime
  capture showed the LEFT box registered with sustained 44.1 kHz audio traffic,
  zero queue overruns, and DSP comfortably within budget. GPIO 4 status
  wiring remains deferred; physical sound and end-to-end Bluetooth playback
  are still unverified.
- **2026-11-26:** Added temporary PCM average/peak diagnostics to the box and
  flashed it on COM5. Over roughly 88 seconds it remained registered and
  playing, but received PCM was exactly zero (`pcm_avg_abs=0`, `pcm_peak=0`).
  The user confirmed music was playing and that ESP A/B were wired only for
  I2S/common ground. ESP B intentionally mutes when DSP controls are missing
  or stale, so the absent ESP A GPIO 17 -> ESP B GPIO 16 UART link is the
  leading cause. A subsequent COM5 capture identified ESP B and showed
  continued I2S input and forwarding, with zero hub queue overruns. B's raw
  versus post-DSP PCM levels still need checking after the forward control
  wire is added. GPIO 4 remains deferred.
- **2026-11-26:** Flashed ESP B's raw/post-DSP level diagnostics on COM5.
  Input was nonzero (`avg_abs` about 260-338; `peak` about 1034-1081), while
  post-DSP PCM remained exactly zero and logs consistently showed
  `params=0 stale=1`. This confirms ESP B receives audio over I2S but does not
  receive valid DSP control frames over UART; its stale-control fail-safe
  mutes the stream sent to the box. Verify A GPIO 17 TX -> B GPIO 16 RX and
  common GND. GPIO 4 remains deferred.
- **2026-11-26:** Monitored COM5 from the correct ESP box project. The box
  booted normally, joined the hub, registered, and played packets; its
  received PCM remained zero. Fixed ESP A so current DSP controls are sent at
  startup and periodically by the UI task, independent of incoming Bluetooth
  audio frames; made the first DSP UART frame send immediately. ESP A build
  passed. It has not been flashed or verified because COM5 is connected to the
  box. The A2DP logs still show connection timeouts (`0x0b`), so a working
  Bluetooth audio source remains a separate requirement.
- **2026-11-26:** Added an OLED page 8 box-status display showing registered
  boxes out of two and LEFT/RIGHT connection states. It shows unknown
  (`-- / 2`) when the UART status frame is stale. ESP A build passed; flashing
  and on-device display verification remain pending.
