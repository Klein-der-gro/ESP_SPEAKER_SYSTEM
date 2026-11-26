# Speaker box - LEFT/RIGHT firmware

The same binary is flashed to both boxes. GPIO 27 open (internal pull-up)
selects LEFT; connect GPIO 27 to GND on the RIGHT box.

## Connections

- PCM5102 BCLK/LCK/DIN: GPIO 18/19/23.
- GPIO 27: leave open for LEFT; connect to GND for RIGHT.
- DAC left output is the high-frequency crossover output; DAC right is the
  low-frequency output. Connect each to its separate amplifier input.
- Use a stable supply and common signal ground. Check PCM5102 board strap
  requirements before powering it.

The firmware uses a 2500 Hz Linkwitz-Riley fourth-order crossover, with a
1500 Hz lower bound. Verify that crossover frequency against the actual
tweeter and driver specifications before connecting an amplifier.

## Build and flash

Open this folder as its own ESP-IDF project, select ESP32 and the box's COM
port, then run **Build, Flash and Monitor**. Expect `Speaker box ready` with
the selected role, then Wi-Fi connection and hub registration messages.
Repeat the flash for the second box with GPIO 27 grounded. Full system steps
and known hardware-validation limits are in [`../README.md`](../README.md).
