# Multiroom ESP32 speaker system

This tree contains three independent ESP-IDF projects. Open and flash one project folder at a time; the shared protocol component must remain next to all three projects.

## Projects and wiring

- `esp_a_bluetooth`: existing Bluetooth speaker, OLED, controls, and local DAC, plus UART TX on GPIO 17.
- `esp_b_wifi_hub`: Wi-Fi hub and shared DSP component consumer. ESP A I2S BCK GPIO 18, WS GPIO 19, and data GPIO 23 connect in parallel to ESP A's DAC and ESP B inputs. ESP A GPIO 17 TX connects to ESP B GPIO 16 RX. Connect grounds.
- `esp_box`: one firmware for both boxes. GPIO 27 open selects LEFT; GPIO 27 tied to GND selects RIGHT. The DAC uses BCK GPIO 18, LCK GPIO 19, and DIN GPIO 23.

ESP A's existing peripherals remain as wired: PCM5102 BCK/LCK/DIN on GPIO 18/19/23; encoder signals on GPIO 33/27/14 and GPIO 32/25/26; OLED SDA/SCL on GPIO 21/22. Encoder buttons/signals use pull-ups and connect to ground. Power the OLED from 3.3 V. All connected ESPs must share GND. Keep the inter-ESP I2S wiring short; use 100-ohm series resistors if signal integrity requires them.

The hub defaults are SSID `SPEAKER-NET`, password `change-me-1234`, channel 1, and AP address 192.168.4.1. The crossover defaults to 2500 Hz and must not be lowered below 1500 Hz. Check the actual tweeter/driver specifications before using the crossover firmware.

`DSP_ON_HUB` defaults to `1` in the shared protocol header. In this mode, ESP A's DAC outputs raw, unprocessed audio; do not connect its analog output to an amplifier. The shared DSP source is in `common/dsp/` and is built independently by ESP A and ESP B.

## Flash order

1. Build, flash, and monitor `esp_a_bluetooth`.
2. Build, flash, and monitor `esp_b_wifi_hub`.
3. Build, flash, and monitor each box from `esp_box`; use the role jumper to identify LEFT and RIGHT.

At M0 these projects only verify independent builds and startup/role messages. Audio streaming, Wi-Fi operation, crossover, and synchronization are not implemented yet.

## Milestones

Follow the supplied handoff: test each milestone before moving to the next. The UDP PC listener will be implemented with M2.
