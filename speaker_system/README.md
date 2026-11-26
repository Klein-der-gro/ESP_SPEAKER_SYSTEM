# Multiroom ESP32 speaker system

This firmware targets four ESP32 boards. ESP A is the Bluetooth source and
controller, ESP B is the Wi-Fi hub and shared-DSP processor, and one box
firmware runs on LEFT and RIGHT speaker boards.

The current firmware label is `NR-4.0.26.11`, and the protocol version is `2`.
The shared network and protocol settings are in
[`common/protocol/include/protocol.h`](common/protocol/include/protocol.h).
The example credentials are not secrets:

| Setting | Value |
|---|---|
| Wi-Fi SSID | `SPEAKER-NET` |
| Wi-Fi password | `change-me-1234` |
| Channel | 1 |
| Country code | `AT` |
| Hub IP | `192.168.4.1` |
| Audio UDP port | 5004 |
| Control/sync UDP port | 5005 |
| UART baud rate | 115200 |

Changing credentials or protocol constants requires rebuilding and flashing
every affected project.

## System overview

```text
Phone
  | Bluetooth A2DP/AVRCP
  v
ESP A
  | raw stereo I2S: GPIO 18/19/23
  | DSP settings/sample rate: UART2 GPIO 17 -> 16
  v
ESP B
  | shared DSP + limiter
  | Wi-Fi UDP unicast
  v
LEFT box  ----------------  RIGHT box
  | channel + LR4 crossover | channel + LR4 crossover
  v                         v
PCM5102 + left speaker      PCM5102 + right speaker
```

### Responsibilities

- **ESP A** pairs over Bluetooth A2DP, receives AVRCP status and metadata,
  reads encoders and button actions, renders the OLED, saves settings in NVS,
  forwards settings as DSP parameters, and sends raw stereo I2S to ESP B.
- **ESP B** serves the `SPEAKER-NET` AP, receives I2S and UART controls,
  applies bass, treble, vocal, balance, volume, mute, and limiting once,
  registers LEFT and RIGHT boxes, and sends unicast UDP audio.
- **The LEFT box** receives the shared stereo packet, extracts the left channel,
  applies the LR4 crossover, and sends the high and low bands to the DAC outputs.
- **The RIGHT box** performs the same job on the right channel. GPIO 27 chooses
  the role: open/pulled high means LEFT; connected to GND means RIGHT.

One compiled box binary is used for both boxes. Do not edit separate box
firmware for each role.

## How audio moves

1. ESP A receives stereo PCM from the Bluetooth player. A2DP frames are written
   to a 32 KB stream buffer.
2. ESP A's I2S task reads 1024-byte stereo chunks, updates pre-DSP visualizer
   data, and writes raw 16-bit stereo PCM to I2S. With hub DSP enabled, ESP A
   does not apply the final DSP chain locally.
3. ESP A sends a 4-byte sample-rate frame and an 18-byte DSP parameter frame
   over UART2 whenever settings change. A heartbeat resend keeps ESP B current.
4. ESP B receives I2S as the slave and 256-frame stereo chunks through a 4-entry
   queue. At startup or while controls are stale, the hub applies a mute
   fail-safe instead of forwarding unknown DSP state.
5. ESP B runs DSP and a final limiter, gives each stereo packet a hub timestamp,
   sequence number, frame index, sample rate, and stream-reset/silence flags,
   and sends it to the currently registered role address.
6. Each box maintains an audio jitter queue and waits for clock synchronization
   before playback. It then extracts its role channel and applies the 2500 Hz
   LR4 crossover.
7. The crossover writes two outputs to the PCM5102 in DAC left/right order:
   DAC left carries high frequencies and DAC right carries low frequencies.

Audio packets contain 256 stereo frames: 1024 PCM bytes plus a 32-byte header,
for 1056 bytes total. The theoretical packet rate is approximately 172.27 Hz
at 44.1 kHz and 187.50 Hz at 48 kHz.

## DSP and control behavior

DSP controls are packed in a fixed 18-byte structure:

```text
float bass_db
float treble_db
float vocal_db
int16 balance
int16 volume
uint8 muted
uint8 vocal_mode
```

The UART envelope is:

```text
0xA5 | type | payload length | payload | CRC-8
```

CRC-8 uses polynomial `0x07`. UART frame types are sample rate (`1`),
DSP parameters (`2`), and box status (`3`). ESP B considers DSP controls stale
after five seconds without a valid update and mutes the audio path. ESP A
hearbeats both sample-rate and parameter state once per second.

ESP A saves volume, bass, treble, vocal, balance, vocal mode, startup-chime,
and visualizer settings in NVS. Restarting ESP A restores the saved settings
and sends them to ESP B even if Bluetooth has no active stream.

### OLED pages

The second encoder changes the OLED page. The eight pages are:

1. `HOME` - Bluetooth, AVRCP, LEFT/RIGHT status, playback, volume, and VU levels.
2. `PROGRESS` - track position and metadata.
3. `BASS` - bass boost and waveform/spectrum visualization.
4. `TREBLE` - treble boost and waveform/spectrum visualization.
5. `VOCAL` - vocal boost, sensitivity, and extracted voice-band display.
6. `BALANCE` - LEFT/CENTER/RIGHT balance.
7. `SETUP` - presets, startup chime, and visualizer mode.
8. `BOX STATUS` - current LEFT/RIGHT registration count.

## Safety and wiring

**Disconnect the amplifier from ESP A's DAC before using hub-DSP mode.** ESP
A's I2S output is raw full-range audio and is wired in parallel to ESP A's DAC
and ESP B's I2S input. Do not feed ESP A's DAC output to a speaker amplifier.
The LEFT and RIGHT boxes are the speaker outputs.

### ESP A to ESP B

| ESP A | ESP B | Signal |
|---|---|---|
| GPIO 18 | GPIO 18 | I2S BCLK |
| GPIO 19 | GPIO 19 | I2S WS/LCK |
| GPIO 23 | GPIO 23 | I2S data |
| GPIO 17 | GPIO 16 | UART2 ESP A TX -> ESP B RX, 115200 baud |
| GPIO 4 | GPIO 4 | UART2 ESP B TX -> ESP A RX, 115200 baud |
| GND | GND | Common ground |

### ESP B pin map

| ESP B GPIO | Function | Connection |
|---:|---|---|
| 18 | I2S BCLK input | ESP A GPIO 18 |
| 19 | I2S WS/LRCK input | ESP A GPIO 19 |
| 23 | I2S DIN input | ESP A GPIO 23 |
| 16 | UART2 RX | ESP A GPIO 17 TX |
| 4 | UART2 TX | ESP A GPIO 4 RX |
| GND | Ground | ESP A GND |

ESP B has no separate DAC or control-button wiring in the current firmware.
The Wi-Fi access point uses the ESP32 radio internally and does not require
additional GPIO pins.

Keep I2S connections short (about 20 cm or less). If needed, add 100-ohm
series resistors to the I2S lines. ESP A is the I2S clock master; ESP B is the
I2S receiver. GPIO 4 carries returned box status. Check board pinouts before
wiring; GPIO 16/17 may be reserved for PSRAM on some modules.

ESP A's existing hardware remains unchanged: PCM5102 BCLK/LCK/DIN on
GPIO 18/19/23; OLED SDA/SCL on GPIO 21/22; the two encoders use
GPIO 33/27/14 and GPIO 32/25/26.

### Each speaker box

- GPIO 18 -> PCM5102 BCLK; GPIO 19 -> LCK; GPIO 23 -> DIN.
- GPIO 27 open (internal pull-up) selects LEFT; connect GPIO 27 to GND for
  RIGHT.
- DAC left output -> high-frequency amplifier input.
- DAC right output -> low-frequency amplifier input.
- Connect DAC analog ground to amplifier ground. Each ESP and amplifier needs
  a stable supply; do not power an amplifier from the ESP's 3.3 V pin.
- For common PCM5102 boards, check the board markings and datasheet for SCK,
  FMT, XSMT, FLT, and DEMP strap requirements before connecting power.

### ESP box pin map

| ESP box GPIO | PCM5102/role connection | Function |
|---:|---|---|
| 18 | PCM5102 BCLK / BCK | I2S bit clock output |
| 19 | PCM5102 LCK / LRCK / WSEL | I2S word clock output |
| 23 | PCM5102 DIN | I2S audio output |
| 27 | Open/pulled high or GND | LEFT when open/high; RIGHT when GND |
| GND | PCM5102 GND and role jumper ground | Common ground |

No UART or ESP-B-to-box wire is required. Each box receives registration and
audio packets over Wi-Fi.

The crossover is Linkwitz-Riley fourth order at 2500 Hz, with a 1500 Hz
firmware lower bound. Driver models and datasheets were not provided; verify
the tweeter's permitted crossover frequency and amplifier gain before
connecting the amplifiers. Never lower the crossover below the tweeter's
specified limit.

## Flash order

Build, flash, and monitor each project independently. The same box binary is
used for both speakers.

1. Open `speaker_system.code-workspace` in VS Code. It contains all three
   ESP-IDF projects; the ESP-IDF extension's current project must be the
   project being built. Select ESP A (`esp_a_bluetooth`) from the status bar
   or with **ESP-IDF: Pick a Workspace Folder**, select the board's COM port,
   and use **Build, Flash and Monitor**.
2. Select `esp_b_wifi_hub`, select ESP B's COM port, and flash/monitor.
3. Select `esp_box`, select the LEFT box's COM port, and flash/monitor.
4. Repeat step 3 for the RIGHT box, with GPIO 27 connected to GND.
5. Power all four devices. ESP B should advertise `SPEAKER-NET`; both boxes
   should connect and register without manual IP configuration.
6. Pair the phone with ESP A over Bluetooth and play a stereo test track.

Expected log fragments:

- ESP A: current firmware version and `UART2 full-duplex link ready`.
- ESP B: `AP up: SSID=SPEAKER-NET ... IP=192.168.4.1`; `I2S slave RX ready`;
  `Registered boxes: ...`; per-second packet/DSP status.
- Boxes: `Speaker box ready, role=LEFT/RIGHT`, `Connected to hub Wi-Fi`,
  `LEFT/RIGHT box registered with hub`, and per-second jitter/loss status.
- ESP A OLED home page: `L+` / `R+` means ESP B currently sees that box
  registered; `L-` / `R-` means not registered or status link is stale.
- OLED page 8 (`BOX STATUS`) shows the connected-box count and each role's
  status. It displays `-- / 2` when ESP A has not received a fresh status frame.

## Responsibilities and current validation

- ESP A does Bluetooth/A2DP decoding, controls, display, settings, and
  pre-DSP visualizer analysis. It sends raw stereo I2S plus sample rate and
  DSP parameters to ESP B over UART. ESP B returns box-registration status
  over UART so ESP A can display it.
- ESP B performs bass/treble/vocal DSP, balance, volume, mute, and the final
  limiter once, then sends unicast UDP packets to registered boxes.
- Each box selects its LEFT or RIGHT source channel, synchronizes packet
  timing, applies the 2500 Hz LR4 crossover, and outputs high tones on DAC
  left and low tones on DAC right.
- All three ESP-IDF 6.1 projects build. **Network throughput, amplifier
  output, synchronization accuracy, Wi-Fi recovery, and extended playback
  have not been verified on hardware.** Do not treat successful builds as
  proof of end-to-end playback.

## Troubleshooting

| Symptom | Likely cause | What to check |
|---|---|---|
| `SPEAKER-NET` is not visible | ESP B did not start its AP | Check ESP B monitor for `AP up`; verify its power and flash result. |
| A box never registers | Wrong country/credentials, role wiring, or no DHCP | Use the shared settings in `protocol.h`; verify GPIO 27 and common ground. |
| OLED shows `L-` / `R-` although a box says connected | Missing ESP B TX -> ESP A RX status wire | Connect ESP B GPIO 4 to ESP A GPIO 4, with common ground; verify ESP B logs registration. |
| Box registers but reports no packets | ESP A isn't sending I2S, or ESP B has no I2S clock | Check ESP A Bluetooth playback, GPIO 18/19/23, and UART GPIO 17->16. |
| Only one speaker plays | The other box is not registered or has the wrong role | Check both box logs and GPIO 27 jumper. |
| Audio stutters | Wi-Fi loss, weak supply, or queue overruns | Check loss/overrun counters, use short USB cables and stable 5 V supplies, and keep ESP B outside metal enclosures. |
| Sound is distorted or the tweeter is at risk | Wrong crossover/driver limits or clipping | Disconnect power; verify driver datasheets and amplifier wiring before testing again. |

For packet diagnostics, connect a PC to `SPEAKER-NET` and run
`python tools/udp_listener.py --role left` from `speaker_system/`. The utility
registers the PC as a test box and reports packet rate and sequence gaps; it
uses one of ESP B's two Wi-Fi client slots.
