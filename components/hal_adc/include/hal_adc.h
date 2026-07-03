#pragma once

#include <stdint.h>

#include "esp_err.h"

#define HAL_ADC_SAMPLE_COUNT           16
#define HAL_ADC_FALLBACK_FULL_SCALE_MV 3300

/**
 * @brief Initialize ADC1 oneshot reads for the battery and NTC inputs.
 *
 * The HAL validates the board GPIO-to-channel map, configures both channels
 * for 12 dB attenuation, and attempts to create curve-fitting calibration
 * handles. Calling this function more than once is safe.
 *
 * If eFuse-backed calibration is unavailable, initialization succeeds with an
 * explicit warning and later reads use an approximate linear conversion.
 *
 * @return ESP_OK on success, or an ESP-IDF error code on failure.
 */
esp_err_t hal_adc_init(void);

/**
 * @brief Read the battery-divider ADC pin voltage.
 *
 * A read averages HAL_ADC_SAMPLE_COUNT raw samples and returns the ADC pin
 * voltage in millivolts. Battery-pack conversion belongs to the application
 * sensor layer.
 *
 * @param mv Output ADC pin voltage in millivolts.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before initialization,
 *         ESP_ERR_INVALID_ARG for a null output pointer, or an ESP-IDF error
 *         code if ADC sampling fails.
 */
esp_err_t hal_adc_read_battery_mv(uint32_t *mv);

/**
 * @brief Read the NTC-divider ADC pin voltage.
 *
 * A read averages HAL_ADC_SAMPLE_COUNT raw samples and returns the ADC pin
 * voltage in millivolts. Temperature conversion belongs to the application
 * sensor layer.
 *
 * @param mv Output ADC pin voltage in millivolts.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before initialization,
 *         ESP_ERR_INVALID_ARG for a null output pointer, or an ESP-IDF error
 *         code if ADC sampling fails.
 */
esp_err_t hal_adc_read_ntc_mv(uint32_t *mv);
