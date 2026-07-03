#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_state.h"
#include "esp_err.h"

#define APP_BATTERY_TEST_SAMPLE_PERIOD_MS 1000
#define APP_BATTERY_TEST_MAX_SAMPLES      1800
#define APP_BATTERY_TEST_PHASE_MAX_LEN    24

typedef enum {
    APP_BATTERY_TEST_STOPPED = 0,
    APP_BATTERY_TEST_RUNNING,
    APP_BATTERY_TEST_PAUSED,
    APP_BATTERY_TEST_COMPLETE,
} app_battery_test_run_state_t;

typedef struct {
    uint32_t sequence;
    uint32_t elapsed_ms;
    uint32_t battery_adc_mv;
    float battery_voltage_v;
    uint8_t battery_percent;
    bool battery_valid;
    uint32_t ntc_adc_mv;
    float ntc_temp_c;
    bool ntc_valid;
    bool light_enabled;
    uint8_t brightness_percent;
    uint16_t cct_kelvin;
    uint16_t cold_duty;
    uint16_t warm_duty;
    uint8_t fan_percent;
    system_fault_t fault;
    char phase[APP_BATTERY_TEST_PHASE_MAX_LEN];
} app_battery_test_sample_t;

typedef struct {
    bool recording;
    app_battery_test_run_state_t run_state;
    size_t sample_count;
    uint32_t total_samples;
    uint32_t dropped_samples;
    uint32_t elapsed_ms;
    char phase[APP_BATTERY_TEST_PHASE_MAX_LEN];
} app_battery_test_status_t;

/** Initialize the in-memory battery-test recorder and its sampling task. */
esp_err_t app_battery_test_init(void);

/**
 * Start the automatic test, or resume it after a pause.
 *
 * Starting from STOPPED or COMPLETE clears old samples and begins the full
 * automatic sequence from the first step.
 */
esp_err_t app_battery_test_start(void);

/** Pause the automatic sequence, preserving its samples and elapsed time. */
esp_err_t app_battery_test_pause(void);

/** Stop the automatic sequence and preserve samples for CSV download. */
esp_err_t app_battery_test_stop(void);

/** Clear all recorded samples. Recording must be stopped first. */
esp_err_t app_battery_test_clear(void);

/** Copy the current recorder status. */
esp_err_t app_battery_test_get_status(app_battery_test_status_t *status);

/** Copy a sample by chronological index, oldest sample at index zero. */
esp_err_t app_battery_test_get_sample(size_t index,
                                      app_battery_test_sample_t *sample);
