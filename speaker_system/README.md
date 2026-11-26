# Multiroom ESP32 speaker system

This firmware targets four ESP32 boards:

1. ESP A receives Bluetooth audio and handles the OLED, encoders, playback
   controls, and saved settings.
2. ESP B receives the raw I2S stream and controls from ESP A, applies the
   shared DSP once, and sends audio over its Wi-Fi access point.
3. The LEFT and RIGHT boxes receive the stream, select their stereo channel,
   apply the same LR4 crossover, and drive their DACs.

The version name is `MR-M0.2`. Wi-Fi settings are in
`common/protocol/include/protocol.h`: SSID `SPEAKER-NET`, placeholder password
`change-me-1234`, channel 1, and country code `AT`. Keep the credentials
non-personal in this public repository. Changing them requires rebuilding and
flashing ESP B and both boxes.

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
| GPIO 17 | GPIO 16 | UART2 TX to RX, 115200 baud |
| GND | GND | Common ground |

Keep I2S connections short (about 20 cm or less). If needed, add 100-ohm
series resistors to the I2S lines. ESP A is the I2S clock master; ESP B is the
I2S receiver. ESP A GPIO 17/ESP B GPIO 16 are unsuitable on modules using
those pins for PSRAM.

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

The crossover is Linkwitz-Riley fourth order at 2500 Hz, with a 1500 Hz
firmware lower bound. Driver models and datasheets were not provided; verify
the tweeter's permitted crossover frequency and amplifier gain before
connecting the amplifiers. Never lower the crossover below the tweeter's
specified limit.

## Flash order

Build, flash, and monitor each project independently. The same box binary is
used for both speakers.

1. Open `esp_a_bluetooth` in VS Code, select the board's COM port, and use
   **Build, Flash and Monitor**.
2. Open `esp_b_wifi_hub`, select ESP B's COM port, and flash/monitor.
3. Open `esp_box`, select the LEFT box's COM port, and flash/monitor.
4. Repeat step 3 for the RIGHT box, with GPIO 27 connected to GND.
5. Power all four devices. ESP B should advertise `SPEAKER-NET`; both boxes
   should connect and register without manual IP configuration.
6. Pair the phone with ESP A over Bluetooth and play a stereo test track.

Expected log fragments:

- ESP A: `firmware: MR-M0.2` and `UART2 rate link ready`.
- ESP B: `AP up: SSID=SPEAKER-NET ... IP=192.168.4.1`; `I2S slave RX ready`;
  registered LEFT/RIGHT boxes and per-second packet/DSP status.
- Boxes: `Speaker box ready, role=LEFT/RIGHT`, `Connected to hub Wi-Fi`,
  `LEFT/RIGHT box registered with hub`, and per-second jitter/loss status.

## Responsibilities and current validation

- ESP A does Bluetooth/A2DP decoding, controls, display, settings, and
  pre-DSP visualizer analysis. It sends raw stereo I2S plus sample rate and
  DSP parameters to ESP B over UART.
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
| Box registers but reports no packets | ESP A isn't sending I2S, or ESP B has no I2S clock | Check ESP A Bluetooth playback, GPIO 18/19/23, and UART GPIO 17->16. |
| Only one speaker plays | The other box is not registered or has the wrong role | Check both box logs and GPIO 27 jumper. |
| Audio stutters | Wi-Fi loss, weak supply, or queue overruns | Check loss/overrun counters, use short USB cables and stable 5 V supplies, and keep ESP B outside metal enclosures. |
| Sound is distorted or the tweeter is at risk | Wrong crossover/driver limits or clipping | Disconnect power; verify driver datasheets and amplifier wiring before testing again. |

For packet diagnostics, connect a PC to `SPEAKER-NET` and run
`python tools/udp_listener.py --role left` from `speaker_system/`. The utility
registers the PC as a test box and reports packet rate and sequence gaps; it
uses one of ESP B's two Wi-Fi client slots.
