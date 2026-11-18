// Guition JC3248W535C (TF1 V1.1): AXS15231B display on QSPI and AXS15231B touch on I2C.
//
// Pins (Guition's JC3248W535 documentation and schematic; the schematic swaps the touch SDA / SCL labels):
//   display QSPI  CS 45, CLK 47, D0 21, D1 48, D2 40, D3 39, no reset line (software reset)
//   backlight     GPIO1 (high = on)
//   touch I2C     SDA 4, SCL 8, address 0x3B (shared with the external SX1509 footswitch expander)
//
// The AXS15231B on QSPI takes whole lines only: the esp_lcd_axs15231b driver sends no row address (RASET) on QSPI, it
// starts at the top with RAMWR (y = 0) and continues with RAMWRC. So the screen is always sent whole, top to bottom:
// LVGL draws only what changed into one full frame in PSRAM (direct mode), and at the end of each frame the frame goes
// out in strips through two small DMA buffers in internal RAM (byte-swapped to the panel's big-endian RGB565).
#include "board.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "board";

// The panel is portrait (320 x 480, rows sent from the top); the UI is landscape (BOARD_SCREEN_W x H = 480 x 320).
// LCD_ROTATION says how the board is held: 90 = the panel's top edge on the right, 270 = on the left. Picture and
// touches are turned together.
#define LCD_H_RES 320
#define LCD_V_RES 480
#define UI_W BOARD_SCREEN_W
#define UI_H BOARD_SCREEN_H
#define LCD_ROTATION 90

#define LCD_HOST SPI2_HOST
#define LCD_PIN_CS GPIO_NUM_45
#define LCD_PIN_CLK GPIO_NUM_47
#define LCD_PIN_D0 GPIO_NUM_21
#define LCD_PIN_D1 GPIO_NUM_48
#define LCD_PIN_D2 GPIO_NUM_40
#define LCD_PIN_D3 GPIO_NUM_39
#define LCD_PIN_BACKLIGHT GPIO_NUM_1

#define I2C_SDA GPIO_NUM_4
#define I2C_SCL GPIO_NUM_8
#define TOUCH_I2C_HZ 400000

// The AXS15231B reports touches in the panel's own (portrait) coordinates; touch_to_ui() turns them like the picture.
// If a board reports mirrored touches, TOUCH_MIRROR_X / Y mirror the raw values first.
#define TOUCH_MIRROR_X 0
#define TOUCH_MIRROR_Y 0

// Lines per strip sent to the panel: 2 x 320 x 24 x 2 bytes = 30 KB of internal DMA RAM (the Waveshare board's RGB
// bounce buffers took 32 KB).
#define STRIP_LINES 24
#define STRIP_BYTES (LCD_H_RES * STRIP_LINES * 2)

static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_io_handle_t s_lcd_io;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;

static uint16_t *s_strip[2];          // DMA buffers in internal RAM
static SemaphoreHandle_t s_strip_free; // counts the strips not on their way to the panel

static esp_err_t init_i2c(void)
{
    const i2c_master_bus_config_t bus = {
        .i2c_port = -1,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus, &s_i2c);
}

// A strip reached the panel (called from the SPI interrupt).
static bool on_strip_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_strip_free, &woken);
    return woken == pdTRUE;
}

// Initialization for the JC3248W535's 320x480 AXS15231B panel - the "320480 type1" sequence of Arduino_GFX
// (moononournation/Arduino_GFX, Arduino_AXS15231B.h), the one used with this board; it ends with sleep out (0x11),
// display on (0x29) and a memory write. The driver's built-in sequence (ending with 0x22, all pixels off) is not used.
static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xA0, (uint8_t[]){0xC0, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00}, 17, 0},
    {0xA2, (uint8_t[]){0x30, 0x3C, 0x24, 0x14, 0xD0, 0x20, 0xFF, 0xE0, 0x40, 0x19, 0x80, 0x80, 0x80, 0x20, 0xF9, 0x10, 0x02, 0xFF, 0xFF, 0xF0, 0x90, 0x01, 0x32, 0xA0, 0x91, 0xE0, 0x20, 0x7F, 0xFF, 0x00, 0x5A}, 31, 0},
    {0xD0, (uint8_t[]){0xE0, 0x40, 0x51, 0x24, 0x08, 0x05, 0x10, 0x01, 0x20, 0x15, 0xC2, 0x42, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0x60, 0x14, 0x1E, 0x51, 0x15, 0x00, 0x8A, 0x20, 0x00, 0x03, 0x3A, 0x12}, 30, 0},
    {0xA3, (uint8_t[]){0xA0, 0x06, 0xAA, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55}, 22, 0},
    {0xC1, (uint8_t[]){0x31, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x41, 0x00, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0D, 0x00, 0xFF, 0x40}, 30, 0},
    {0xC3, (uint8_t[]){0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01}, 11, 0},
    {0xC4, (uint8_t[]){0x00, 0x24, 0x33, 0x80, 0x00, 0xEA, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50}, 29, 0},
    {0xC5, (uint8_t[]){0x18, 0x00, 0x00, 0x03, 0xFE, 0x3A, 0x4A, 0x20, 0x30, 0x10, 0x88, 0xDE, 0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x3A, 0x4A, 0x20, 0x10, 0x10, 0x00}, 23, 0},
    {0xC6, (uint8_t[]){0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22, 0x12, 0x22, 0x01, 0x03, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22}, 20, 0},
    {0xC7, (uint8_t[]){0x50, 0x32, 0x28, 0x00, 0xA2, 0x80, 0x8F, 0x00, 0x80, 0xFF, 0x07, 0x11, 0x9C, 0x67, 0xFF, 0x24, 0x0C, 0x0D, 0x0E, 0x0F}, 20, 0},
    {0xC9, (uint8_t[]){0x33, 0x44, 0x44, 0x01}, 4, 0},
    {0xCF, (uint8_t[]){0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0x88, 0x00, 0x65, 0x09, 0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x08, 0x08, 0x12, 0xA0, 0x08}, 27, 0},
    {0xD5, (uint8_t[]){0x40, 0x8E, 0x8D, 0x01, 0x35, 0x04, 0x92, 0x74, 0x04, 0x92, 0x74, 0x04, 0x08, 0x6A, 0x04, 0x46, 0x03, 0x03, 0x03, 0x03, 0x82, 0x01, 0x03, 0x00, 0xE0, 0x51, 0xA1, 0x00, 0x00, 0x00}, 30, 0},
    {0xD6, (uint8_t[]){0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x93, 0x00, 0x01, 0x83, 0x07, 0x07, 0x00, 0x07, 0x07, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x84, 0x00, 0x20, 0x01, 0x00}, 30, 0},
    {0xD7, (uint8_t[]){0x03, 0x01, 0x0B, 0x09, 0x0F, 0x0D, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19, 0x40, 0x8E, 0x04, 0x00, 0x20, 0xA0, 0x1F}, 19, 0},
    {0xD8, (uint8_t[]){0x02, 0x00, 0x0A, 0x08, 0x0E, 0x0C, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19}, 12, 0},
    {0xD9, (uint8_t[]){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDD, (uint8_t[]){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDF, (uint8_t[]){0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90}, 8, 0},
    {0xE0, (uint8_t[]){0x3B, 0x28, 0x10, 0x16, 0x0C, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x13, 0x2C, 0x33, 0x28, 0x0D}, 17, 0},
    {0xE1, (uint8_t[]){0x37, 0x28, 0x10, 0x16, 0x0B, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x14, 0x2C, 0x33, 0x28, 0x0F}, 17, 0},
    {0xE2, (uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE3, (uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F}, 17, 0},
    {0xE4, (uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE5, (uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0F}, 17, 0},
    {0xA4, (uint8_t[]){0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30}, 16, 0},
    {0xA4, (uint8_t[]){0x85, 0x85, 0x95, 0x85}, 4, 0},
    {0xBB, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, 0},
    {0x11, (uint8_t[]){0x00}, 1, 200},
    {0x29, (uint8_t[]){0x00}, 1, 100},
    {0x2C, (uint8_t[]){0x00, 0x00, 0x00, 0x00}, 4, 0},
};

// On QSPI a command goes as opcode 0x02 and pixel data as opcode 0x32, the command in bits 8-15.
#define QSPI_CMD(c)   ((0x02 << 24) | ((c) << 8))
#define QSPI_PIXELS(c) ((0x32 << 24) | ((c) << 8))

// Sends lines y .. y+lines-1 (full width) - the panel takes big-endian RGB565. The AXS15231B on QSPI ignores the row
// window (RASET), so a frame must go in order from the top: the first strip with memory write (0x2C), which restarts at
// row 0, the following ones with memory write continue (0x3C). Returns at once; the strip's buffer is free again when
// on_strip_sent runs.
static void send_strip(int y, int lines, const void *pixels)
{
    if (y == 0) {
        const uint8_t caset[4] = { 0, 0, (LCD_H_RES - 1) >> 8, (LCD_H_RES - 1) & 0xFF };
        esp_lcd_panel_io_tx_param(s_lcd_io, QSPI_CMD(0x2A), caset, 4);
    }
    esp_lcd_panel_io_tx_color(s_lcd_io, QSPI_PIXELS(y == 0 ? 0x2C : 0x3C), pixels, (size_t)lines * LCD_H_RES * 2);
}

static esp_err_t init_panel(void)
{
    s_strip_free = xSemaphoreCreateCounting(2, 2);
    ESP_RETURN_ON_FALSE(s_strip_free, ESP_ERR_NO_MEM, TAG, "strip semaphore");
    for (int i = 0; i < 2; i++) {
        s_strip[i] = heap_caps_aligned_alloc(64, STRIP_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        ESP_RETURN_ON_FALSE(s_strip[i], ESP_ERR_NO_MEM, TAG, "strip buffer");
    }

    const spi_bus_config_t bus = AXS15231B_PANEL_BUS_QSPI_CONFIG(LCD_PIN_CLK, LCD_PIN_D0, LCD_PIN_D1, LCD_PIN_D2,
                                                                 LCD_PIN_D3, STRIP_BYTES);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "QSPI bus");

    esp_lcd_panel_io_spi_config_t io_config = AXS15231B_PANEL_IO_QSPI_CONFIG(LCD_PIN_CS, on_strip_sent, NULL);
    io_config.pclk_hz = 32 * 1000 * 1000;   // (the AXS15231B's maximum as Arduino_GFX uses it; the macro sets 40 MHz)
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &s_lcd_io), TAG,
                        "panel IO");

    const axs15231b_vendor_config_t vendor = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_axs15231b(s_lcd_io, &panel_config, &s_panel), TAG, "AXS15231B panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    // (Not esp_lcd_panel_disp_on_off(): in esp_lcd_axs15231b 2.1.1 its argument means "off" - true sends DISPOFF.
    // The sequence above already ends with display on.)
    return ESP_OK;
}

static void set_backlight(bool on)
{
    static bool configured;
    if (!configured) {
        const gpio_config_t io = { .pin_bit_mask = 1ULL << LCD_PIN_BACKLIGHT, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&io);
        configured = true;
    }
    gpio_set_level(LCD_PIN_BACKLIGHT, on);
}

// Panel coordinates (portrait, x 0-319, y 0-479) -> the UI's (landscape, x 0-479, y 0-319), as the picture is turned.
static void touch_to_ui(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num,
                        uint8_t max_point_num)
{
    (void)tp;
    (void)strength;
    for (int i = 0; i < *point_num && i < max_point_num; i++) {
        int px = x[i] < LCD_H_RES ? x[i] : LCD_H_RES - 1, py = y[i] < LCD_V_RES ? y[i] : LCD_V_RES - 1;
        if (TOUCH_MIRROR_X) px = LCD_H_RES - 1 - px;
        if (TOUCH_MIRROR_Y) py = LCD_V_RES - 1 - py;
#if LCD_ROTATION == 90
        x[i] = LCD_V_RES - 1 - py;
        y[i] = px;
#else
        x[i] = py;
        y[i] = LCD_H_RES - 1 - px;
#endif
    }
}

// esp_lvgl_port stops the program (ESP_ERROR_CHECK) when one touch read fails - an I2C hiccup on the shared bus is
// enough. The read is skipped instead: LVGL keeps the last touch state, the next read (a few ms later) goes on.
static esp_err_t (*s_touch_read_orig)(esp_lcd_touch_handle_t tp);
static uint32_t s_touch_read_errors;
static esp_err_t touch_read_tolerant(esp_lcd_touch_handle_t tp)
{
    esp_err_t err = s_touch_read_orig(tp);
    if (err != ESP_OK && (s_touch_read_errors++ % 100) == 0)
        ESP_LOGW(TAG, "Touch read failed (%s), skipped (%u so far)", esp_err_to_name(err), (unsigned)s_touch_read_errors);
    return ESP_OK;
}

static esp_lcd_touch_handle_t init_touch(void)
{
    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG_EX(TOUCH_I2C_HZ);
    if (esp_lcd_new_panel_io_i2c(s_i2c, &io_config, &io) != ESP_OK) return NULL;

    const esp_lcd_touch_config_t config = {
        .x_max = UI_W,
        .y_max = UI_H,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,   // read by polling (esp_lvgl_port reads the touch at every LVGL input tick)
        .process_coordinates = touch_to_ui,
    };
    for (int attempt = 0; attempt < 5; attempt++) {
        esp_lcd_touch_handle_t touch = NULL;
        if (esp_lcd_touch_new_i2c_axs15231b(io, &config, &touch) == ESP_OK) return touch;
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    return NULL;
}

// Panel rows y .. y+lines-1 from the landscape frame (UI_W x UI_H), byte-swapped (the panel takes big-endian RGB565).
// Panel pixel (px, py) shows UI pixel (UI_W-1-py, px) at 90 degrees, (py, UI_H-1-px) at 270. A strip is a band of
// `lines` columns of the frame: read row by row (along the frame's memory), written column by column.
static void rotate_strip(uint16_t *strip, const uint16_t *frame, int y, int lines)
{
    for (int px = 0; px < LCD_H_RES; px++) {
#if LCD_ROTATION == 90
        const uint16_t *src = frame + (size_t)px * UI_W + (UI_W - y - lines);   // columns UI_W-y-lines .. UI_W-1-y
        for (int k = 0; k < lines; k++) {
            uint16_t c = src[k];
            strip[(lines - 1 - k) * LCD_H_RES + px] = (uint16_t)((c << 8) | (c >> 8));
        }
#else
        const uint16_t *src = frame + (size_t)(UI_H - 1 - px) * UI_W + y;      // columns y .. y+lines-1
        for (int k = 0; k < lines; k++) {
            uint16_t c = src[k];
            strip[k * LCD_H_RES + px] = (uint16_t)((c << 8) | (c >> 8));
        }
#endif
    }
}

// LVGL (direct mode): called for every changed area; the frame goes out once, after the last area of the frame.
static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    if (lv_display_flush_is_last(display)) {
        const uint8_t *frame = lv_display_get_buf_active(display)->data;
        for (int y = 0, n = 0; y < LCD_V_RES; y += STRIP_LINES, n++) {
            int lines = LCD_V_RES - y < STRIP_LINES ? LCD_V_RES - y : STRIP_LINES;
            uint16_t *strip = s_strip[n & 1];
            xSemaphoreTake(s_strip_free, portMAX_DELAY);   // this buffer's previous strip has reached the panel
            rotate_strip(strip, (const uint16_t *)frame, y, lines);
            send_strip(y, lines, strip);
        }
        // Both strips back before LVGL draws the next frame (the buffers are reused at once).
        xSemaphoreTake(s_strip_free, portMAX_DELAY);
        xSemaphoreTake(s_strip_free, portMAX_DELAY);
        xSemaphoreGive(s_strip_free);
        xSemaphoreGive(s_strip_free);
    }
    lv_display_flush_ready(display);
}

// Diagnostic (temporary): 6 bands of 80 lines top to bottom - red, green, blue, white, black, yellow - with a white
// 60x60 square in the top-left corner of the panel, shown for 5 s before LVGL starts. Tells colour order, orientation
// and whether every strip reaches the panel.
#define BOARD_TEST_PATTERN 0
#if BOARD_TEST_PATTERN
static void show_test_pattern(void)
{
    static const uint16_t band[6] = { 0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000, 0xFFE0 };
    for (int y = 0, n = 0; y < LCD_V_RES; y += STRIP_LINES, n++) {
        int lines = LCD_V_RES - y < STRIP_LINES ? LCD_V_RES - y : STRIP_LINES;
        uint8_t *b = (uint8_t *)s_strip[n & 1];
        xSemaphoreTake(s_strip_free, portMAX_DELAY);
        for (int l = 0; l < lines; l++) {
            for (int x = 0; x < LCD_H_RES; x++) {
                int yy = y + l;
                uint16_t c = (x < 60 && yy < 60) ? 0xFFFF : band[yy / 80];
                b[(l * LCD_H_RES + x) * 2] = c >> 8;          // the panel takes big-endian RGB565
                b[(l * LCD_H_RES + x) * 2 + 1] = c & 0xFF;
            }
        }
        send_strip(y, lines, b);
    }
    xSemaphoreTake(s_strip_free, portMAX_DELAY);
    xSemaphoreTake(s_strip_free, portMAX_DELAY);
    xSemaphoreGive(s_strip_free);
    xSemaphoreGive(s_strip_free);
    set_backlight(true);
    ESP_LOGI(TAG, "Test pattern on screen (5 s)");
    vTaskDelay(pdMS_TO_TICKS(5000));
}
#endif

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_i2c;
}

lv_display_t *board_display_init(void)
{
    ESP_ERROR_CHECK(init_i2c());
    set_backlight(false);
    ESP_ERROR_CHECK(init_panel());
#if BOARD_TEST_PATTERN
    show_test_pattern();
#endif
    s_touch = init_touch();
    if (s_touch) {   // GILIAR CONTROL: a failed touch read is skipped instead of stopping the controller
        s_touch_read_orig = s_touch->read_data;
        s_touch->read_data = touch_read_tolerant;
    }

    // LVGL draws on core 1; core 0 runs Bluetooth.
    lvgl_port_cfg_t port_config = ESP_LVGL_PORT_INIT_CONFIG();
    port_config.task_stack = 8192;
    port_config.task_affinity = 1;
    ESP_ERROR_CHECK(lvgl_port_init(&port_config));

    // One full frame in PSRAM (300 KB): LVGL redraws only the changed areas into it.
    const size_t frame_bytes = (size_t)UI_W * UI_H * 2;
    void *frame = heap_caps_aligned_alloc(64, frame_bytes, MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(frame ? ESP_OK : ESP_ERR_NO_MEM);
    memset(frame, 0, frame_bytes);

    lvgl_port_lock(0);
    lv_display_t *display = lv_display_create(UI_W, UI_H);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, frame, NULL, frame_bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, flush_cb);
    lvgl_port_unlock();

    if (s_touch) {
        const lvgl_port_touch_cfg_t touch_config = { .disp = display, .handle = s_touch };
        lvgl_port_add_touch(&touch_config);
    } else {
        ESP_LOGW(TAG, "Touch controller not found");
    }

    set_backlight(true);
    ESP_LOGI(TAG, "Display ready (JC3248W535, AXS15231B QSPI, %dx%d landscape)", UI_W, UI_H);
    return display;
}
