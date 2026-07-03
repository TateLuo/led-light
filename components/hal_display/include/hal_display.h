#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

/**
 * @brief Initialize the ST7789-compatible SPI LCD and LVGL port.
 *
 * The panel uses DMA-backed partial refresh buffers. Initialization displays
 * the startup logo, starts the LVGL handler task, and leaves the backlight off
 * until hal_display_set_backlight() is called explicitly.
 */
esp_err_t hal_display_init(void);

/**
 * @brief Set the LCD backlight PWM duty.
 *
 * @param percent Backlight duty in the range 0..100.
 */
esp_err_t hal_display_set_backlight(uint8_t percent);

/**
 * @brief Get the LVGL display owned by the display HAL.
 *
 * @return LVGL display handle after initialization, otherwise NULL.
 */
lv_display_t *hal_display_get_lvgl_display(void);

/**
 * @brief Lock LVGL before calling LVGL APIs outside the HAL handler task.
 *
 * @param timeout_ms Maximum wait in milliseconds. UINT32_MAX waits forever.
 * @return True when the recursive LVGL mutex was acquired.
 */
bool hal_display_lock(uint32_t timeout_ms);

/**
 * @brief Release a lock acquired with hal_display_lock().
 */
void hal_display_unlock(void);
