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

#define LCD_H_RES BOARD_SCREEN_W
#define LCD_V_RES BOARD_SCREEN_H

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

// Touch orientation relative to the portrait screen. The AXS15231B reports in the panel's own (portrait) coordinates;
// if a board reports mirrored or swapped touches, these three switch it without touching the rest.
#define TOUCH_SWAP_XY 0
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

    const esp_lcd_panel_io_spi_config_t io_config = AXS15231B_PANEL_IO_QSPI_CONFIG(LCD_PIN_CS, on_strip_sent, NULL);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &s_lcd_io), TAG,
                        "panel IO");

    const axs15231b_vendor_config_t vendor = {
        .flags.use_qspi_interface = 1,   // the driver's own initialization sequence
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
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel on");
    return ESP_OK;
}

static void set_backlight(bool on)
{
    const gpio_config_t io = { .pin_bit_mask = 1ULL << LCD_PIN_BACKLIGHT, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    gpio_set_level(LCD_PIN_BACKLIGHT, on);
}

static esp_lcd_touch_handle_t init_touch(void)
{
    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG_EX(TOUCH_I2C_HZ);
    if (esp_lcd_new_panel_io_i2c(s_i2c, &io_config, &io) != ESP_OK) return NULL;

    const esp_lcd_touch_config_t config = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,   // read by polling (esp_lvgl_port reads the touch at every LVGL input tick)
        .flags = { .swap_xy = TOUCH_SWAP_XY, .mirror_x = TOUCH_MIRROR_X, .mirror_y = TOUCH_MIRROR_Y },
    };
    for (int attempt = 0; attempt < 5; attempt++) {
        esp_lcd_touch_handle_t touch = NULL;
        if (esp_lcd_touch_new_i2c_axs15231b(io, &config, &touch) == ESP_OK) return touch;
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    return NULL;
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
            memcpy(strip, frame + (size_t)y * LCD_H_RES * 2, (size_t)lines * LCD_H_RES * 2);
#if LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR < 5
            lv_draw_sw_rgb565_swap(strip, (uint32_t)lines * LCD_H_RES);
#else
            lv_draw_rgb565_swap(strip, (uint32_t)lines * LCD_H_RES);
#endif
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + lines, strip);
        }
        // Both strips back before LVGL draws the next frame (the buffers are reused at once).
        xSemaphoreTake(s_strip_free, portMAX_DELAY);
        xSemaphoreTake(s_strip_free, portMAX_DELAY);
        xSemaphoreGive(s_strip_free);
        xSemaphoreGive(s_strip_free);
    }
    lv_display_flush_ready(display);
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_i2c;
}

lv_display_t *board_display_init(void)
{
    ESP_ERROR_CHECK(init_i2c());
    set_backlight(false);
    ESP_ERROR_CHECK(init_panel());
    s_touch = init_touch();

    // LVGL draws on core 1; core 0 runs Bluetooth.
    lvgl_port_cfg_t port_config = ESP_LVGL_PORT_INIT_CONFIG();
    port_config.task_stack = 8192;
    port_config.task_affinity = 1;
    ESP_ERROR_CHECK(lvgl_port_init(&port_config));

    // One full frame in PSRAM (300 KB): LVGL redraws only the changed areas into it.
    const size_t frame_bytes = (size_t)LCD_H_RES * LCD_V_RES * 2;
    void *frame = heap_caps_aligned_alloc(64, frame_bytes, MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(frame ? ESP_OK : ESP_ERR_NO_MEM);
    memset(frame, 0, frame_bytes);

    lvgl_port_lock(0);
    lv_display_t *display = lv_display_create(LCD_H_RES, LCD_V_RES);
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
    ESP_LOGI(TAG, "Display ready (JC3248W535, AXS15231B QSPI %dx%d)", LCD_H_RES, LCD_V_RES);
    return display;
}
