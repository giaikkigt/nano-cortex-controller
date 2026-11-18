// Guition JC3248W535C (TF1 V1.1): ESP32-S3, 16 MB flash, 8 MB octal PSRAM, 3.5" IPS 320x480 (portrait) with the
// AXS15231B controller on QSPI, and the AXS15231B's capacitive touch on I2C (GPIO4 SDA, GPIO8 SCL, address 0x3B).
// The external SX1509 footswitch expander shares that I2C bus (it was on the Waveshare board's touch bus before).
#pragma once

#include "driver/i2c_master.h"
#include "lvgl.h"

// Screen size in the UI's orientation (portrait).
#define BOARD_SCREEN_W 320
#define BOARD_SCREEN_H 480

// Starts display, touch and LVGL (esp_lvgl_port). Use lvgl_port_lock()/unlock() around LVGL calls.
lv_display_t *board_display_init(void);

// The board's I2C bus (GPIO4 SDA / GPIO8 SCL): touch and the external SX1509 footswitch expander.
i2c_master_bus_handle_t board_i2c_bus(void);
