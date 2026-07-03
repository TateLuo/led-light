#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool enabled;
    uint8_t brightness_percent;
    uint16_t cct_kelvin;
} light_request_t;

typedef struct {
    bool battery_voltage_valid;
    float battery_voltage_v;
    bool ntc_temp_valid;
    float ntc_temp_c;
    bool battery_percent_valid;
    uint8_t battery_percent;
} app_light_runtime_inputs_t;

/**
 * @brief Start safety-limited cold/warm LED output synchronization.
 */
esp_err_t app_light_init(void);

/**
 * @brief Apply a brightness and CCT request after enforcing safety policy.
 */
esp_err_t app_light_set(const light_request_t *request);

/**
 * @brief Immediately turn both physical LED channels off.
 */
esp_err_t app_light_off(void);

/**
 * @brief Synchronize physical LED output from the shared application state.
 */
esp_err_t app_light_sync_now(void);

/**
 * @brief Update optional runtime derating inputs for the CCT mixer.
 *
 * This is a hook for later battery-voltage compensation, thermal derating,
 * and low-battery brightness caps. The current implementation keeps these
 * derating scales at 1.0 until the hardware thresholds are calibrated.
 */
esp_err_t app_light_update_runtime_inputs(
    const app_light_runtime_inputs_t *inputs);

/**
 * @brief Latch the LED controller off for orderly shutdown.
 *
 * Subsequent synchronization requests keep both physical channels off if USB
 * power leaves the MCU running after the power hold is released.
 *
 * If the external supply is not ready, this module keeps LED output off until
 * the supply becomes available again.
 */
esp_err_t app_light_prepare_shutdown(void);
