#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Initialize the power hold GPIO and power-key input.
 *
 * The hold output is asserted during initialization to avoid a power-loss
 * window before the explicit hal_power_hold(true) call in app_main().
 */
esp_err_t hal_power_init(void);

/**
 * @brief Enable or release the system power hold output.
 *
 * @param enable True to keep the system powered, false to request power-off.
 */
esp_err_t hal_power_hold(bool enable);

/**
 * @brief Read the debounced-independent raw power-key pressed state.
 *
 * This returns false until BOARD_EC_KEY_DET_CONFIRMED is enabled after
 * hardware validation. Debouncing and long-press timing belong above the HAL.
 */
bool hal_power_key_pressed(void);

/**
 * @brief Return true when the external supply is ready for LED output.
 *
 * When BOARD_GPIO_USB_VBUS_DETECT is GPIO_NUM_NC, the supply is assumed
 * ready immediately. If this pin is configured, the LED controller will
 * stay off until the pin reports the ready level.
 */
bool hal_power_external_supply_ready(void);

/**
 * @brief Release the system power hold output.
 */
esp_err_t hal_power_shutdown(void);
