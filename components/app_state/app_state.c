#include "app_state.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "APP_STATE";

static SemaphoreHandle_t state_mutex;
static app_state_t current_state;
static bool initialized;

static bool state_float_valid(float value)
{
    return isnan(value) || isfinite(value);
}

static bool state_values_valid(const app_state_t *state)
{
    return state->brightness_percent >= APP_STATE_BRIGHTNESS_MIN_PERCENT
           && state->brightness_percent <= APP_STATE_BRIGHTNESS_MAX_PERCENT
           && state->cct_kelvin >= APP_STATE_CCT_MIN_KELVIN
           && state->cct_kelvin <= APP_STATE_CCT_MAX_KELVIN
           && state->battery_percent <= 100
           && state->fan_percent <= 100
           && state->ota_progress_percent <= 100
           && state->charge_state >= CHARGE_STATE_UNKNOWN
           && state->charge_state <= CHARGE_STATE_FULL
           && state->fault >= SYSTEM_FAULT_NONE
           && state->fault <= SYSTEM_FAULT_ADC_ERROR
           && state->ota_status >= APP_OTA_STATUS_IDLE
           && state->ota_status <= APP_OTA_STATUS_FAILED
           && state_float_valid(state->battery_voltage_v)
           && (isnan(state->battery_voltage_v)
               || state->battery_voltage_v >= 0.0f)
           && state_float_valid(state->ntc_temp_c);
}

static esp_err_t lock_state(void)
{
    if (!initialized || state_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE ?
           ESP_OK : ESP_ERR_TIMEOUT;
}

static void unlock_state(void)
{
    xSemaphoreGive(state_mutex);
}

static int32_t clamp_int64_to_range(int64_t value, int32_t minimum,
                                    int32_t maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return (int32_t)value;
}

esp_err_t app_state_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    state_mutex = xSemaphoreCreateMutex();
    if (state_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create application state mutex");
        return ESP_ERR_NO_MEM;
    }

    memset(&current_state, 0, sizeof(current_state));
    current_state.light_enabled = false;
    current_state.brightness_percent = APP_STATE_BRIGHTNESS_DEFAULT_PERCENT;
    current_state.cct_kelvin = APP_STATE_CCT_DEFAULT_KELVIN;
    current_state.battery_voltage_v = NAN;
    current_state.charge_state = CHARGE_STATE_UNKNOWN;
    current_state.ntc_temp_c = NAN;
    current_state.display_on = true;
    current_state.fault = SYSTEM_FAULT_NONE;
    current_state.ota_status = APP_OTA_STATUS_IDLE;

    initialized = true;
    ESP_LOGI(TAG, "Application state initialized: light off, brightness=%u%%, CCT=%uK",
             (unsigned int)current_state.brightness_percent,
             (unsigned int)current_state.cct_kelvin);
    return ESP_OK;
}

esp_err_t app_state_get(app_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lock_state();
    if (err != ESP_OK) {
        return err;
    }

    *state = current_state;
    unlock_state();
    return ESP_OK;
}

esp_err_t app_state_set(const app_state_t *state)
{
    if (state == NULL || !state_values_valid(state)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lock_state();
    if (err != ESP_OK) {
        return err;
    }

    current_state = *state;
    unlock_state();
    return ESP_OK;
}

esp_err_t app_state_adjust_brightness(int32_t delta_percent)
{
    esp_err_t err = lock_state();
    if (err != ESP_OK) {
        return err;
    }

    current_state.brightness_percent = (uint8_t)clamp_int64_to_range(
        (int64_t)current_state.brightness_percent + delta_percent,
        APP_STATE_BRIGHTNESS_MIN_PERCENT, APP_STATE_BRIGHTNESS_MAX_PERCENT);
    const uint8_t brightness_percent = current_state.brightness_percent;

    unlock_state();
    ESP_LOGI(TAG, "Brightness request updated: %u%%",
             (unsigned int)brightness_percent);
    return ESP_OK;
}

esp_err_t app_state_adjust_cct(int32_t delta_kelvin)
{
    esp_err_t err = lock_state();
    if (err != ESP_OK) {
        return err;
    }

    current_state.cct_kelvin = (uint16_t)clamp_int64_to_range(
        (int64_t)current_state.cct_kelvin + delta_kelvin,
        APP_STATE_CCT_MIN_KELVIN, APP_STATE_CCT_MAX_KELVIN);
    const uint16_t cct_kelvin = current_state.cct_kelvin;

    unlock_state();
    ESP_LOGI(TAG, "CCT request updated: %uK", (unsigned int)cct_kelvin);
    return ESP_OK;
}

esp_err_t app_state_set_light_enabled(bool enabled)
{
    esp_err_t err = lock_state();
    if (err != ESP_OK) {
        return err;
    }

    current_state.light_enabled = enabled;
    unlock_state();

    ESP_LOGI(TAG, "Light request updated: %s", enabled ? "on" : "off");
    return ESP_OK;
}

esp_err_t app_state_toggle_light(void)
{
    esp_err_t err = lock_state();
    if (err != ESP_OK) {
        return err;
    }

    current_state.light_enabled = !current_state.light_enabled;
    const bool enabled = current_state.light_enabled;
    unlock_state();

    ESP_LOGI(TAG, "Light request updated: %s", enabled ? "on" : "off");
    return ESP_OK;
}

esp_err_t app_state_set_display_on(bool display_on)
{
    esp_err_t err = lock_state();
    if (err != ESP_OK) {
        return err;
    }

    current_state.display_on = display_on;
    unlock_state();
    return ESP_OK;
}

esp_err_t app_state_set_manual_fan_enabled(bool enabled)
{
    esp_err_t err = lock_state();
    if (err != ESP_OK) {
        return err;
    }

    current_state.manual_fan_enabled = enabled;
    unlock_state();

    ESP_LOGI(TAG, "Manual fan request updated: %s",
             enabled ? "on" : "off");
    return ESP_OK;
}

esp_err_t app_state_update_sensors(const app_state_sensor_update_t *sensors)
{
    if (sensors == NULL || sensors->battery_percent > 100
        || (sensors->battery_valid
            && (!isfinite(sensors->battery_voltage_v)
                || sensors->battery_voltage_v < 0.0f))
        || (sensors->ntc_valid && !isfinite(sensors->ntc_temp_c))) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lock_state();
    if (err != ESP_OK) {
        return err;
    }

    current_state.battery_voltage_v = sensors->battery_valid ?
                                      sensors->battery_voltage_v : NAN;
    current_state.battery_percent = sensors->battery_valid ?
                                    sensors->battery_percent : 0;
    current_state.ntc_temp_c = sensors->ntc_valid ? sensors->ntc_temp_c : NAN;

    unlock_state();
    return ESP_OK;
}

esp_err_t app_state_update_safety(const app_state_safety_update_t *safety)
{
    if (safety == NULL || safety->fan_percent > 100
        || safety->fault < SYSTEM_FAULT_NONE
        || safety->fault > SYSTEM_FAULT_ADC_ERROR) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lock_state();
    if (err != ESP_OK) {
        return err;
    }

    current_state.fan_percent = safety->fan_percent;
    current_state.low_battery = safety->low_battery;
    current_state.critical_battery = safety->critical_battery;
    current_state.over_temperature = safety->over_temperature;
    current_state.fault = safety->fault;

    unlock_state();
    return ESP_OK;
}

esp_err_t app_state_update_user_settings(
    const app_state_user_settings_t *settings)
{
    if (settings == NULL
        || settings->brightness_percent < APP_STATE_BRIGHTNESS_MIN_PERCENT
        || settings->brightness_percent > APP_STATE_BRIGHTNESS_MAX_PERCENT
        || settings->cct_kelvin < APP_STATE_CCT_MIN_KELVIN
        || settings->cct_kelvin > APP_STATE_CCT_MAX_KELVIN) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lock_state();
    if (err != ESP_OK) {
        return err;
    }

    current_state.brightness_percent = settings->brightness_percent;
    current_state.cct_kelvin = settings->cct_kelvin;
    current_state.light_enabled = settings->light_enabled;
    current_state.manual_fan_enabled = settings->manual_fan_enabled;

    unlock_state();
    return ESP_OK;
}

esp_err_t app_state_update_ota(const app_state_ota_update_t *ota)
{
    if (ota == NULL || ota->progress_percent > 100
        || ota->status < APP_OTA_STATUS_IDLE
        || ota->status > APP_OTA_STATUS_FAILED) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lock_state();
    if (err != ESP_OK) {
        return err;
    }

    current_state.ota_active = ota->active;
    current_state.ota_uploading = ota->uploading;
    current_state.ota_progress_percent = ota->progress_percent;
    current_state.ota_status = ota->status;
    (void)snprintf(current_state.ota_ssid, sizeof(current_state.ota_ssid),
                   "%s", ota->ssid != NULL ? ota->ssid : "");
    (void)snprintf(current_state.ota_password,
                   sizeof(current_state.ota_password), "%s",
                   ota->password != NULL ? ota->password : "");
    (void)snprintf(current_state.ota_ip, sizeof(current_state.ota_ip),
                   "%s", ota->ip != NULL ? ota->ip : "");
    (void)snprintf(current_state.ota_error, sizeof(current_state.ota_error),
                   "%s", ota->error != NULL ? ota->error : "");

    unlock_state();
    return ESP_OK;
}
