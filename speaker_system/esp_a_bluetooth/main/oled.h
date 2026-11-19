#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define OLED_WIDTH  128
#define OLED_HEIGHT 64

esp_err_t oled_init(void);
void oled_clear(void);
void oled_text(int x, int y, const char *text, unsigned scale);
void oled_text_window(int x, int y, const char *text, unsigned scale,
                      unsigned max_chars, unsigned offset);
void oled_fill_rect(int x, int y, int width, int height, bool set);
void oled_rect(int x, int y, int width, int height, bool set);
esp_err_t oled_flush(void);
esp_err_t oled_flush_slide(bool forward);
