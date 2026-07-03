#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define HAL_LED_PWM_FREQ_HZ          19531
#define HAL_LED_DUTY_RESOLUTION_BITS 12
#define HAL_LED_DUTY_MAX             4095

typedef struct {
    uint32_t pwm_freq_hz;
    uint8_t duty_resolution_bits;
} hal_led_config_t;

typedef struct {
    uint16_t cold_duty;
    uint16_t warm_duty;
} hal_led_duty_snapshot_t;

/**
 * @brief Initialize cold and warm LED PWM outputs in the off state.
 *
 * Uses the configured 19531 Hz, 12-bit LEDC configuration. Calling this
 * function more than once is safe.
 *
 * @return ESP_OK on success, or an ESP-IDF error code on failure.
 */
esp_err_t hal_led_init(void);

/**
 * @brief Set the raw PWM duty for the cold and warm LED channels.
 *
 * This low-level HAL API does not apply brightness, CCT, or safety policies.
 * Both values must be in the range 0..HAL_LED_DUTY_MAX.
 *
 * @param cold_duty Cold LED PWM duty.
 * @param warm_duty Warm LED PWM duty.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before initialization,
 *         ESP_ERR_INVALID_ARG for an out-of-range duty, or an ESP-IDF error
 *         code if the LEDC update fails.
 */
esp_err_t hal_led_set_cw(uint16_t cold_duty, uint16_t warm_duty);

/**
 * @brief Read the last successfully applied raw PWM duties.
 *
 * The returned values are the final LEDC duty counts after application-layer
 * brightness, CCT, safety limits, and gamma correction have been applied.
 *
 * @param snapshot Output duty snapshot.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before initialization,
 *         ESP_ERR_INVALID_ARG for a null output pointer, or ESP_ERR_TIMEOUT if
 *         the LED mutex cannot be locked.
 */
esp_err_t hal_led_get_duty_snapshot(hal_led_duty_snapshot_t *snapshot);

/**
 * @brief Turn both LED channels off.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before initialization, or
 *         an ESP-IDF error code if the LEDC update fails.
 */
esp_err_t hal_led_off(void);

/**
 * @brief Force both LED control pins to a hardware-safe off level.
 *
 * This stops the LEDC channels if they are configured, drives both PWM pins as
 * plain GPIO outputs at BOARD_LED_PWM_OFF_LEVEL, and can optionally hold the
 * GPIO level across a reset/brownout window while the chip remains powered.
 * Use this only for shutdown and fault handling; normal dimming should use
 * hal_led_set_cw() / hal_led_off().
 *
 * @param hold_level True to enable GPIO hold after forcing the off level.
 * @return ESP_OK on success, or the first GPIO/LEDC error encountered.
 */
esp_err_t hal_led_force_safe_off(bool hold_level);
