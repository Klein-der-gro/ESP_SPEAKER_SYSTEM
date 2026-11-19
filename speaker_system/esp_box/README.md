# ESP box - LEFT/RIGHT firmware (M0 skeleton)

One firmware selects its role at boot using GPIO 27. Leaving GPIO 27 open (internal pull-up) selects LEFT; connecting GPIO 27 to GND selects RIGHT. M0 only verifies role detection and boot.

## Wiring

For M0, connect USB power and optionally the role jumper. Later milestones use PCM5102 BCK GPIO 18, LCK GPIO 19, DIN GPIO 23, and the DAC power/ground. Common ground is required. The DAC outputs will feed separate high- and low-frequency amplifier channels after crossover implementation.

## Build and flash

Open this folder as the ESP-IDF project, select the ESP32 target and this box's COM port, then run `idf.py build` and `idf.py -p PORT flash monitor`.

## Expected serial output

With GPIO 27 open: `ESP_BOX: ESP box ready (milestone M0), role: LEFT`.

With GPIO 27 connected to GND: the same line reports `RIGHT`.
