#pragma once

#include <stdint.h>

#include "esp_err.h"

#define HAL_FAN_PWM_FREQ_HZ          20000
#define HAL_FAN_DUTY_RESOLUTION_BITS 10
#define HAL_FAN_DUTY_MAX             1023
#define HAL_FAN_PERCENT_MAX          100

/**
 * @brief Initialize the fan PWM output in the off state.
 *
 * Uses the provisional 20 kHz, 10-bit LEDC configuration. Calling this
 * function more than once is safe.
 *
 * @return ESP_OK on success, or an ESP-IDF error code on failure.
 */
esp_err_t hal_fan_init(void);

/**
 * @brief Set fan PWM duty as a percentage.
 *
 * This low-level HAL API does not apply temperature curves, startup pulses, or
 * safety policies.
 *
 * @param percent Fan duty in the range 0..100.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before initialization,
 *         ESP_ERR_INVALID_ARG for a value above 100, or an ESP-IDF error code
 *         if the LEDC update fails.
 */
esp_err_t hal_fan_set_percent(uint8_t percent);

/**
 * @brief Turn the fan PWM output off.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before initialization, or
 *         an ESP-IDF error code if the LEDC update fails.
 */
esp_err_t hal_fan_off(void);
