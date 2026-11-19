#include "oled.h"

#include <ctype.h>
#include <string.h>

#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OLED_SDA_PIN 21
#define OLED_SCL_PIN 22
#define OLED_ADDR_1  0x3C
#define OLED_ADDR_2  0x3D

static const char *TAG = "OLED";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static uint8_t s_framebuffer[OLED_WIDTH * OLED_HEIGHT / 8];
static uint8_t s_previous_framebuffer[OLED_WIDTH * OLED_HEIGHT / 8];
static uint8_t s_slide_target[OLED_WIDTH * OLED_HEIGHT / 8];
static uint8_t s_slide_old[OLED_WIDTH * OLED_HEIGHT / 8];

static const uint8_t s_letters[26][5] = {
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C},
    {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
    {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01},
    {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F},
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06},
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01},
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
    {0x7F, 0x20, 0x18, 0x20, 0x7F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}
};

static const uint8_t s_digits[10][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}
};

static void pixel(int x, int y, bool set)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }
    size_t index = (size_t)(y / 8) * OLED_WIDTH + (size_t)x;
    uint8_t mask = (uint8_t)(1U << (y & 7));
    if (set) {
        s_framebuffer[index] |= mask;
    } else {
        s_framebuffer[index] &= (uint8_t)~mask;
    }
}

static bool framebuffer_pixel(const uint8_t *framebuffer, int x, int y)
{
    size_t index = (size_t)(y / 8) * OLED_WIDTH + (size_t)x;
    return (framebuffer[index] & (uint8_t)(1U << (y & 7))) != 0;
}

static void set_framebuffer_pixel(uint8_t *framebuffer, int x, int y)
{
    size_t index = (size_t)(y / 8) * OLED_WIDTH + (size_t)x;
    framebuffer[index] |= (uint8_t)(1U << (y & 7));
}

static uint8_t glyph_column(char c, unsigned column)
{
    if (column >= 5) {
        return 0;
    }
    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'Z') {
        return s_letters[c - 'A'][column];
    }
    if (c >= '0' && c <= '9') {
        return s_digits[c - '0'][column];
    }
    switch (c) {
    case '-': return column == 2 ? 0x08 : 0;
    case '_': return column == 2 ? 0x40 : 0;
    case '.': return column == 2 ? 0x60 : 0;
    case ',': return column == 2 ? 0x60 : (column == 1 ? 0x40 : 0);
    case ':': return column == 2 ? 0x36 : 0;
    case '/': return (uint8_t)(1U << (column + 1));
    case '?': {
        static const uint8_t glyph[5] = {0x02, 0x01, 0x51, 0x09, 0x06};
        return glyph[column];
    }
    case '!': return column == 2 ? 0x5F : 0;
    case '+': return column == 2 ? 0x1C : ((column == 1 || column == 3) ? 0x08 : 0);
    case '%': {
        static const uint8_t glyph[5] = {0x63, 0x13, 0x08, 0x64, 0x63};
        return glyph[column];
    }
    case '(': return column == 1 ? 0x1C : (column == 2 ? 0x22 : 0x41);
    case ')': return column == 2 ? 0x22 : (column == 3 ? 0x1C : 0x41);
    case '\'': return column == 2 ? 0x07 : 0;
    case '"': return column == 1 || column == 3 ? 0x07 : 0;
    default: return 0;
    }
}

esp_err_t oled_init(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = OLED_SDA_PIN,
        .scl_io_num = OLED_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_bus);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t address = 0;
    for (uint16_t candidate = 0x08; candidate <= 0x77; candidate++) {
        err = i2c_master_probe(s_bus, candidate, 10);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "I2C-Geraet auf Adresse 0x%02X gefunden", candidate);
            if (address == 0 || candidate == OLED_ADDR_1 || candidate == OLED_ADDR_2) {
                address = candidate;
            }
            if (candidate == OLED_ADDR_1 || candidate == OLED_ADDR_2) {
                break;
            }
        } else if (err != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "I2C-Suche auf Adresse 0x%02X fehlgeschlagen: %s",
                     candidate, esp_err_to_name(err));
            return err;
        }
    }
    if (address == 0) {
        ESP_LOGE(TAG, "Kein I2C-Geraet gefunden; OLED-Stromversorgung und SDA/SCL pruefen");
        return ESP_ERR_NOT_FOUND;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(s_bus, &device_config, &s_device);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t init_sequence[] = {
        0x00, 0xAE, 0x20, 0x00, 0x40, 0xA1, 0xC8, 0xA8, 0x3F,
        0xD3, 0x00, 0xDA, 0x12, 0xD5, 0x80, 0xD9, 0xF1,
        0xDB, 0x30, 0x81, 0x7F, 0xA4, 0xA6, 0x8D, 0x14, 0xAF,
    };
    err = i2c_master_transmit(s_device, init_sequence, sizeof(init_sequence), 1000);
    if (err != ESP_OK) {
        return err;
    }
    oled_clear();
    return oled_flush();
}

void oled_clear(void)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
}

void oled_text(int x, int y, const char *text, unsigned scale)
{
    if (text == NULL || scale == 0) {
        return;
    }
    while (*text != '\0') {
        for (unsigned col = 0; col < 5; col++) {
            uint8_t bits = glyph_column(*text, col);
            for (unsigned row = 0; row < 7; row++) {
                if ((bits & (1U << row)) != 0) {
                    oled_fill_rect(x + (int)(col * scale), y + (int)(row * scale),
                                   (int)scale, (int)scale, true);
                }
            }
        }
        x += (int)(6 * scale);
        text++;
    }
}

void oled_text_window(int x, int y, const char *text, unsigned scale,
                      unsigned max_chars, unsigned offset)
{
    if (text == NULL || max_chars == 0) {
        return;
    }
    size_t length = strlen(text);
    if (length <= max_chars) {
        oled_text(x, y, text, scale);
        return;
    }

    char window[32];
    if (max_chars >= sizeof(window)) {
        max_chars = sizeof(window) - 1;
    }
    size_t span = length + 3;
    size_t start = offset % span;
    for (unsigned i = 0; i < max_chars; i++) {
        size_t index = (start + i) % span;
        window[i] = index < length ? text[index] : ' ';
    }
    window[max_chars] = '\0';
    oled_text(x, y, window, scale);
}

void oled_fill_rect(int x, int y, int width, int height, bool set)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    for (int row = y; row < y + height; row++) {
        for (int col = x; col < x + width; col++) {
            pixel(col, row, set);
        }
    }
}

void oled_rect(int x, int y, int width, int height, bool set)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    oled_fill_rect(x, y, width, 1, set);
    oled_fill_rect(x, y + height - 1, width, 1, set);
    oled_fill_rect(x, y, 1, height, set);
    oled_fill_rect(x + width - 1, y, 1, height, set);
}

esp_err_t oled_flush(void)
{
    uint8_t command[4];
    uint8_t data[OLED_WIDTH + 1];
    data[0] = 0x40;
    for (uint8_t page = 0; page < OLED_HEIGHT / 8; page++) {
        command[0] = 0x00;
        command[1] = (uint8_t)(0xB0 | page);
        command[2] = 0x00;
        command[3] = 0x10;
        esp_err_t err = i2c_master_transmit(s_device, command, sizeof(command), 1000);
        if (err != ESP_OK) {
            return err;
        }
        memcpy(&data[1], &s_framebuffer[(size_t)page * OLED_WIDTH], OLED_WIDTH);
        err = i2c_master_transmit(s_device, data, sizeof(data), 1000);
        if (err != ESP_OK) {
            return err;
        }
    }
    memcpy(s_previous_framebuffer, s_framebuffer, sizeof(s_framebuffer));
    return ESP_OK;
}

esp_err_t oled_flush_slide(bool forward)
{
    memcpy(s_slide_target, s_framebuffer, sizeof(s_framebuffer));
    memcpy(s_slide_old, s_previous_framebuffer, sizeof(s_slide_old));

    for (int offset = 8; offset <= OLED_WIDTH; offset += 8) {
        memset(s_framebuffer, 0, sizeof(s_framebuffer));
        for (int y = 0; y < OLED_HEIGHT; y++) {
            for (int x = 0; x < OLED_WIDTH; x++) {
                int old_x = forward ? x + offset : x - offset;
                int new_x = forward ? x - (OLED_WIDTH - offset)
                                    : x + (OLED_WIDTH - offset);
                bool set = (old_x >= 0 && old_x < OLED_WIDTH &&
                            framebuffer_pixel(s_slide_old, old_x, y)) ||
                           (new_x >= 0 && new_x < OLED_WIDTH &&
                            framebuffer_pixel(s_slide_target, new_x, y));
                if (set) {
                    set_framebuffer_pixel(s_framebuffer, x, y);
                }
            }
        }
        esp_err_t err = oled_flush();
        if (err != ESP_OK) {
            memcpy(s_framebuffer, s_slide_target, sizeof(s_framebuffer));
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    memcpy(s_framebuffer, s_slide_target, sizeof(s_framebuffer));
    return ESP_OK;
}
