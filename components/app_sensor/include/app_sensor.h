#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define APP_SENSOR_SAMPLE_PERIOD_MS 1000

typedef struct {
    uint32_t battery_adc_mv;
    float battery_voltage_v;
    uint8_t battery_percent;
    bool battery_valid;

    uint32_t ntc_adc_mv;
    float ntc_temp_c;
    bool ntc_valid;
} app_sensor_snapshot_t;

/**
 * @brief Initialize the sensor algorithms and start periodic sampling.
 *
 * The first sample is attempted immediately. Sampling errors leave the
 * corresponding validity flag false but do not prevent later retries.
 * Calling this function more than once is safe.
 *
 * @return ESP_OK on success, or ESP_ERR_NO_MEM if the sensor task or mutex
 *         cannot be created.
 */
esp_err_t app_sensor_init(void);

/**
 * @brief Sample both ADC inputs and update the filtered sensor snapshot.
 *
 * This function is exposed for diagnostics. Normal operation uses the
 * periodic sensor task started by app_sensor_init().
 *
 * @return ESP_OK when both inputs are valid, otherwise the first sampling or
 *         conversion error. The snapshot validity flags are always updated.
 */
esp_err_t app_sensor_sample_now(void);

/**
 * @brief Copy the latest filtered sensor snapshot.
 *
 * @param snapshot Output snapshot.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before initialization, or
 *         ESP_ERR_INVALID_ARG for a null output pointer.
 */
esp_err_t app_sensor_get_snapshot(app_sensor_snapshot_t *snapshot);

/**
 * @brief Convert battery-divider ADC pin millivolts to battery-pack volts.
 *
 * Uses the provisional 300k/100k divider ratio from the hardware document.
 *
 * @param adc_mv Battery-divider ADC pin voltage in millivolts.
 * @return Battery-pack voltage in volts.
 */
float app_sensor_battery_adc_mv_to_pack_voltage_v(uint32_t adc_mv);

/**
 * @brief Estimate 2S battery percentage from compensated open-circuit voltage.
 *
 * Uses the provisional piecewise-linear 2S OCV lookup table. Runtime UI
 * battery percentage is additionally load-compensated by app_sensor.
 *
 * @param voltage_v Battery open-circuit estimate in volts.
 * @return Estimated battery percentage in the range 0..100.
 */
uint8_t app_sensor_battery_voltage_to_percent(float voltage_v);

/**
 * @brief Convert NTC-divider millivolts to degrees Celsius.
 *
 * Uses the provisional 3.3 V pull-up, 10k pull-up resistor, and 10k B3950 NTC
 * topology from the hardware document.
 *
 * @param ntc_mv NTC-divider voltage in millivolts.
 * @param temp_c Output temperature in degrees Celsius.
 * @return ESP_OK on success or ESP_ERR_INVALID_ARG for a null output pointer
 *         or an invalid divider voltage.
 */
esp_err_t app_sensor_ntc_mv_to_temp_c(uint32_t ntc_mv, float *temp_c);
