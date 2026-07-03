#include "app_battery_test.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_light.h"
#include "app_safety.h"
#include "app_sensor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal_led.h"

#define BATTERY_TEST_TASK_STACK_SIZE 4096
#define BATTERY_TEST_TASK_PRIORITY   4
#define BATTERY_TEST_CONTROL_PERIOD_MS 100

static const char *TAG = "BATTERY_TEST";

static SemaphoreHandle_t recorder_mutex;
static app_battery_test_sample_t *samples;
static size_t sample_count;
static size_t write_index;
static uint32_t total_samples;
static uint32_t dropped_samples;
static int64_t started_us;
static uint32_t stopped_elapsed_ms;
static bool initialized;
static bool recording;
static app_battery_test_run_state_t run_state = APP_BATTERY_TEST_STOPPED;
static size_t current_step = SIZE_MAX;
static uint8_t restore_brightness;
static uint16_t restore_cct;
static char current_phase[APP_BATTERY_TEST_PHASE_MAX_LEN] = "stopped";

typedef struct {
    uint32_t duration_ms;
    bool light_enabled;
    uint8_t brightness_percent;
    uint16_t cct_kelvin;
    bool manual_fan;
    const char *phase;
} battery_test_step_t;

static const battery_test_step_t test_steps[] = {
    {60000, false, 50, 5100, false, "off_baseline"},
    {30000, false, 100, 2700, false, "warm_pre"},
    {90000, true, 100, 2700, false, "warm_full"},
    {60000, false, 100, 2700, false, "warm_recovery"},
    {30000, false, 100, 6500, false, "cold_pre"},
    {90000, true, 100, 6500, false, "cold_full"},
    {60000, false, 100, 6500, false, "cold_recovery"},
    {30000, false, 100, 5100, false, "mixed_pre"},
    {90000, true, 100, 5100, false, "mixed_full"},
    {60000, false, 100, 5100, false, "mixed_recovery"},
    {30000, false, 50, 5100, false, "medium_pre"},
    {90000, true, 50, 5100, false, "medium_load"},
    {60000, false, 50, 5100, false, "medium_recovery"},
    {30000, false, 100, 5100, false, "step_pre"},
    {10000, true, 100, 5100, false, "step_on_1"},
    {10000, false, 100, 5100, false, "step_off_1"},
    {10000, true, 100, 5100, false, "step_on_2"},
    {10000, false, 100, 5100, false, "step_off_2"},
    {10000, true, 100, 5100, false, "step_on_3"},
    {60000, false, 100, 5100, false, "step_recovery"},
    {60000, false, 50, 5100, false, "fan_off"},
    {60000, false, 50, 5100, true, "fan_on"},
};

#define BATTERY_TEST_STEP_COUNT \
    (sizeof(test_steps) / sizeof(test_steps[0]))

static esp_err_t apply_outputs(bool light_enabled, uint8_t brightness,
                               uint16_t cct, bool manual_fan)
{
    app_state_t state;
    esp_err_t first_error = app_state_get(&state);
    if (first_error != ESP_OK) {
        return first_error;
    }

    esp_err_t err = app_state_adjust_brightness(
        (int32_t)brightness - state.brightness_percent);
    if (err != ESP_OK && first_error == ESP_OK) {
        first_error = err;
    }
    err = app_state_adjust_cct((int32_t)cct - state.cct_kelvin);
    if (err != ESP_OK && first_error == ESP_OK) {
        first_error = err;
    }
    err = app_state_set_light_enabled(light_enabled);
    if (err != ESP_OK && first_error == ESP_OK) {
        first_error = err;
    }
    err = app_state_set_manual_fan_enabled(manual_fan);
    if (err != ESP_OK && first_error == ESP_OK) {
        first_error = err;
    }
    err = app_safety_evaluate_now();
    if (err != ESP_OK && first_error == ESP_OK) {
        first_error = err;
    }
    err = app_light_sync_now();
    if (err != ESP_OK && first_error == ESP_OK) {
        first_error = err;
    }
    return first_error;
}

static esp_err_t apply_safe_pause_outputs(void)
{
    return apply_outputs(false, restore_brightness, restore_cct, false);
}

static size_t step_for_elapsed(uint32_t elapsed_ms)
{
    uint32_t end_ms = 0;
    for (size_t index = 0; index < BATTERY_TEST_STEP_COUNT; ++index) {
        end_ms += test_steps[index].duration_ms;
        if (elapsed_ms < end_ms) {
            return index;
        }
    }
    return BATTERY_TEST_STEP_COUNT;
}

static bool severe_fault_active(void)
{
    app_state_t state;
    if (app_state_get(&state) != ESP_OK) {
        return true;
    }
    return state.fault == SYSTEM_FAULT_CRITICAL_BATTERY
           || state.fault == SYSTEM_FAULT_OVER_TEMP
           || state.fault == SYSTEM_FAULT_NTC_ERROR
           || state.fault == SYSTEM_FAULT_ADC_ERROR;
}

static uint32_t elapsed_ms_now(void)
{
    if (!recording) {
        return stopped_elapsed_ms;
    }
    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    return elapsed_us > 0 ? (uint32_t)(elapsed_us / 1000) : 0;
}

static void capture_sample(void)
{
    app_sensor_snapshot_t sensor;
    app_state_t state;
    hal_led_duty_snapshot_t led = {0};

    if (app_sensor_get_snapshot(&sensor) != ESP_OK
        || app_state_get(&state) != ESP_OK) {
        ESP_LOGW(TAG, "Skipped sample because state is unavailable");
        return;
    }
    (void)hal_led_get_duty_snapshot(&led);

    if (xSemaphoreTake(recorder_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (!recording) {
        xSemaphoreGive(recorder_mutex);
        return;
    }

    app_battery_test_sample_t *sample = &samples[write_index];
    memset(sample, 0, sizeof(*sample));
    sample->sequence = total_samples;
    sample->elapsed_ms = elapsed_ms_now();
    sample->battery_adc_mv = sensor.battery_adc_mv;
    sample->battery_voltage_v = sensor.battery_voltage_v;
    sample->battery_percent = sensor.battery_percent;
    sample->battery_valid = sensor.battery_valid;
    sample->ntc_adc_mv = sensor.ntc_adc_mv;
    sample->ntc_temp_c = sensor.ntc_temp_c;
    sample->ntc_valid = sensor.ntc_valid;
    sample->light_enabled = state.light_enabled;
    sample->brightness_percent = state.brightness_percent;
    sample->cct_kelvin = state.cct_kelvin;
    sample->cold_duty = led.cold_duty;
    sample->warm_duty = led.warm_duty;
    sample->fan_percent = state.fan_percent;
    sample->fault = state.fault;
    (void)snprintf(sample->phase, sizeof(sample->phase), "%s", current_phase);

    write_index = (write_index + 1U) % APP_BATTERY_TEST_MAX_SAMPLES;
    if (sample_count < APP_BATTERY_TEST_MAX_SAMPLES) {
        ++sample_count;
    } else {
        ++dropped_samples;
    }
    ++total_samples;

    ESP_LOGI(TAG,
             "CSV,%" PRIu32 ",%s,%" PRIu32 ",%.3f,%u,%.1f,%u,%u,%u,%u,%u,%u",
             sample->elapsed_ms, sample->phase, sample->battery_adc_mv,
             sample->battery_voltage_v, (unsigned int)sample->battery_percent,
             sample->ntc_temp_c, sample->light_enabled ? 1U : 0U,
             (unsigned int)sample->brightness_percent,
             (unsigned int)sample->cct_kelvin,
             (unsigned int)sample->fan_percent,
             (unsigned int)sample->cold_duty,
             (unsigned int)sample->warm_duty);
    xSemaphoreGive(recorder_mutex);
}

static void recorder_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(APP_BATTERY_TEST_SAMPLE_PERIOD_MS));
        capture_sample();
    }
}

static void automation_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(BATTERY_TEST_CONTROL_PERIOD_MS));

        xSemaphoreTake(recorder_mutex, portMAX_DELAY);
        if (run_state != APP_BATTERY_TEST_RUNNING) {
            xSemaphoreGive(recorder_mutex);
            continue;
        }

        if (severe_fault_active()) {
            stopped_elapsed_ms = elapsed_ms_now();
            recording = false;
            run_state = APP_BATTERY_TEST_STOPPED;
            current_step = SIZE_MAX;
            (void)snprintf(current_phase, sizeof(current_phase),
                           "safety_abort");
            (void)apply_safe_pause_outputs();
            ESP_LOGE(TAG, "Automatic test stopped by safety fault");
            xSemaphoreGive(recorder_mutex);
            continue;
        }

        const uint32_t elapsed_ms = elapsed_ms_now();
        const size_t next_step = step_for_elapsed(elapsed_ms);
        if (next_step >= BATTERY_TEST_STEP_COUNT) {
            stopped_elapsed_ms = elapsed_ms;
            recording = false;
            run_state = APP_BATTERY_TEST_COMPLETE;
            current_step = SIZE_MAX;
            (void)snprintf(current_phase, sizeof(current_phase), "complete");
            (void)apply_safe_pause_outputs();
            ESP_LOGI(TAG, "Automatic battery test completed");
            xSemaphoreGive(recorder_mutex);
            continue;
        }

        if (next_step != current_step) {
            const battery_test_step_t *step = &test_steps[next_step];
            current_step = next_step;
            (void)snprintf(current_phase, sizeof(current_phase), "%s",
                           step->phase);
            const esp_err_t err =
                apply_outputs(step->light_enabled, step->brightness_percent,
                              step->cct_kelvin, step->manual_fan);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to apply test step %s: %s",
                         step->phase, esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "Automatic test step: %s", step->phase);
            }
        }
        xSemaphoreGive(recorder_mutex);
    }
}

esp_err_t app_battery_test_init(void)
{
    if (initialized) {
        return ESP_OK;
    }
    samples = calloc(APP_BATTERY_TEST_MAX_SAMPLES, sizeof(*samples));
    if (samples == NULL) {
        return ESP_ERR_NO_MEM;
    }
    recorder_mutex = xSemaphoreCreateMutex();
    if (recorder_mutex == NULL) {
        free(samples);
        samples = NULL;
        return ESP_ERR_NO_MEM;
    }
    initialized = true;
    TaskHandle_t recorder_task_handle = NULL;
    if (xTaskCreate(recorder_task, "battery_test", BATTERY_TEST_TASK_STACK_SIZE,
                    NULL, BATTERY_TEST_TASK_PRIORITY,
                    &recorder_task_handle) != pdPASS) {
        initialized = false;
        vSemaphoreDelete(recorder_mutex);
        recorder_mutex = NULL;
        free(samples);
        samples = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(automation_task, "battery_auto",
                    BATTERY_TEST_TASK_STACK_SIZE, NULL,
                    BATTERY_TEST_TASK_PRIORITY, NULL) != pdPASS) {
        vTaskDelete(recorder_task_handle);
        initialized = false;
        vSemaphoreDelete(recorder_mutex);
        recorder_mutex = NULL;
        free(samples);
        samples = NULL;
        ESP_LOGE(TAG, "Failed to create battery automation task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Recorder ready for %u one-second samples",
             (unsigned int)APP_BATTERY_TEST_MAX_SAMPLES);
    return ESP_OK;
}

esp_err_t app_battery_test_start(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    app_state_t state;
    esp_err_t err = app_state_get(&state);
    if (err != ESP_OK) {
        return err;
    }
    if (state.ota_active) {
        ESP_LOGW(TAG, "Automatic test start rejected while OTA is active");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(recorder_mutex, portMAX_DELAY);
    if (run_state == APP_BATTERY_TEST_RUNNING) {
        xSemaphoreGive(recorder_mutex);
        return ESP_OK;
    }
    if (run_state == APP_BATTERY_TEST_PAUSED) {
        started_us = esp_timer_get_time()
                     - (int64_t)stopped_elapsed_ms * 1000;
        recording = true;
        run_state = APP_BATTERY_TEST_RUNNING;
        current_step = SIZE_MAX;
    } else {
        sample_count = 0;
        write_index = 0;
        total_samples = 0;
        dropped_samples = 0;
        stopped_elapsed_ms = 0;
        started_us = esp_timer_get_time();
        restore_brightness = state.brightness_percent;
        restore_cct = state.cct_kelvin;
        recording = true;
        run_state = APP_BATTERY_TEST_RUNNING;
        current_step = SIZE_MAX;
    }
    xSemaphoreGive(recorder_mutex);
    return ESP_OK;
}

esp_err_t app_battery_test_pause(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(recorder_mutex, portMAX_DELAY);
    if (run_state != APP_BATTERY_TEST_RUNNING) {
        xSemaphoreGive(recorder_mutex);
        return ESP_OK;
    }
    stopped_elapsed_ms = elapsed_ms_now();
    recording = false;
    run_state = APP_BATTERY_TEST_PAUSED;
    current_step = SIZE_MAX;
    esp_err_t err = apply_safe_pause_outputs();
    xSemaphoreGive(recorder_mutex);
    return err;
}

esp_err_t app_battery_test_stop(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(recorder_mutex, portMAX_DELAY);
    if (run_state == APP_BATTERY_TEST_STOPPED
        || run_state == APP_BATTERY_TEST_COMPLETE) {
        xSemaphoreGive(recorder_mutex);
        return ESP_OK;
    }
    if (run_state == APP_BATTERY_TEST_RUNNING) {
        stopped_elapsed_ms = elapsed_ms_now();
    }
    recording = false;
    run_state = APP_BATTERY_TEST_STOPPED;
    current_step = SIZE_MAX;
    (void)snprintf(current_phase, sizeof(current_phase), "stopped");
    esp_err_t err = apply_safe_pause_outputs();
    xSemaphoreGive(recorder_mutex);
    return err;
}

esp_err_t app_battery_test_clear(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(recorder_mutex, portMAX_DELAY);
    if (run_state == APP_BATTERY_TEST_RUNNING
        || run_state == APP_BATTERY_TEST_PAUSED) {
        xSemaphoreGive(recorder_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    sample_count = 0;
    write_index = 0;
    total_samples = 0;
    dropped_samples = 0;
    started_us = 0;
    stopped_elapsed_ms = 0;
    current_step = SIZE_MAX;
    run_state = APP_BATTERY_TEST_STOPPED;
    (void)snprintf(current_phase, sizeof(current_phase), "stopped");
    xSemaphoreGive(recorder_mutex);
    return ESP_OK;
}

esp_err_t app_battery_test_get_status(app_battery_test_status_t *status)
{
    if (!initialized || status == NULL) {
        return status == NULL ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(recorder_mutex, portMAX_DELAY);
    status->recording = recording;
    status->run_state = run_state;
    status->sample_count = sample_count;
    status->total_samples = total_samples;
    status->dropped_samples = dropped_samples;
    status->elapsed_ms = elapsed_ms_now();
    (void)snprintf(status->phase, sizeof(status->phase), "%s", current_phase);
    xSemaphoreGive(recorder_mutex);
    return ESP_OK;
}

esp_err_t app_battery_test_get_sample(size_t index,
                                      app_battery_test_sample_t *sample)
{
    if (!initialized || sample == NULL) {
        return sample == NULL ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(recorder_mutex, portMAX_DELAY);
    if (index >= sample_count) {
        xSemaphoreGive(recorder_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    const size_t oldest = sample_count == APP_BATTERY_TEST_MAX_SAMPLES ?
                          write_index : 0;
    *sample = samples[(oldest + index) % APP_BATTERY_TEST_MAX_SAMPLES];
    xSemaphoreGive(recorder_mutex);
    return ESP_OK;
}
