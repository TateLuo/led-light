#include "app_safety.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "app_settings.h"
#include "app_state.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal_fan.h"
#include "hal_led.h"

static const char *TAG = "APP_SAFETY";

#define SAFETY_TASK_STACK_SIZE                         4096
#define SAFETY_TASK_PRIORITY                           5
#define SAFETY_TASK_PERIOD_MS                          500

#define SAFETY_LIGHT_LIMIT_FULL_PERCENT                100
#define SAFETY_LIGHT_LIMIT_LOW_BATTERY_PERCENT         50
#define SAFETY_LIGHT_LIMIT_CRITICAL_BATTERY_PERCENT    10
#define SAFETY_LIGHT_LIMIT_ADC_ERROR_PERCENT           20

#define SAFETY_BATTERY_LOW_V                           6.40f
#define SAFETY_BATTERY_CRITICAL_V                      6.20f
#define SAFETY_BATTERY_FORCE_OFF_V                     6.00f
#define SAFETY_BATTERY_RECOVERY_V                      6.60f
#define SAFETY_BATTERY_LOW_DURATION_MS                 3000
#define SAFETY_BATTERY_CRITICAL_DURATION_MS            3000
#define SAFETY_BATTERY_FORCE_OFF_DURATION_MS           1000
#define SAFETY_BATTERY_RECOVERY_DURATION_MS            5000

#define SAFETY_TEMP_DERATE_C                           60.0f
#define SAFETY_TEMP_FORCE_OFF_C                        65.0f
#define SAFETY_TEMP_RECOVERY_C                         55.0f

#define SAFETY_FAN_OFF_PERCENT                         0
#define SAFETY_FAN_40C_PERCENT                         25
#define SAFETY_FAN_45C_PERCENT                         60
#define SAFETY_FAN_MAX_PERCENT                         100

typedef struct {
    bool active;
    uint32_t since_ms;
} duration_tracker_t;

typedef struct {
    uint8_t light_limit_percent;
    uint8_t fan_percent;
    bool light_allowed;
    bool low_battery;
    bool critical_battery;
    bool over_temperature;
    bool severe_battery_force_off;
    system_fault_t fault;
} safety_policy_t;

static SemaphoreHandle_t safety_mutex;
static bool initialized;
static bool low_battery_active;
static bool critical_battery_active;
static bool over_temperature_active;
static bool severe_battery_force_off_active;
static bool outputs_forced_off;
static uint8_t light_limit_percent;
static bool light_allowed;
static uint8_t applied_fan_percent = UINT8_MAX;
static duration_tracker_t low_battery_tracker;
static duration_tracker_t critical_battery_tracker;
static duration_tracker_t force_off_battery_tracker;
static duration_tracker_t recovery_battery_tracker;

static uint8_t minimum_u8(uint8_t left, uint8_t right)
{
    return left < right ? left : right;
}

static bool condition_sustained(bool condition, duration_tracker_t *tracker,
                                uint32_t now_ms, uint32_t duration_ms)
{
    if (!condition) {
        tracker->active = false;
        return false;
    }

    if (!tracker->active) {
        tracker->active = true;
        tracker->since_ms = now_ms;
    }

    return (uint32_t)(now_ms - tracker->since_ms) >= duration_ms;
}

static uint8_t calculate_fan_percent(float temperature_c)
{
    if (!isfinite(temperature_c) || temperature_c >= 50.0f) {
        return SAFETY_FAN_MAX_PERCENT;
    }
    if (temperature_c >= 45.0f) {
        return SAFETY_FAN_45C_PERCENT;
    }
    if (temperature_c >= 40.0f) {
        return SAFETY_FAN_40C_PERCENT;
    }
    return SAFETY_FAN_OFF_PERCENT;
}

static const char *fault_name(system_fault_t fault)
{
    switch (fault) {
    case SYSTEM_FAULT_NONE:
        return "none";
    case SYSTEM_FAULT_LOW_BATTERY:
        return "low-battery";
    case SYSTEM_FAULT_CRITICAL_BATTERY:
        return "critical-battery";
    case SYSTEM_FAULT_OVER_TEMP:
        return "over-temperature";
    case SYSTEM_FAULT_NTC_ERROR:
        return "ntc-error";
    case SYSTEM_FAULT_ADC_ERROR:
        return "adc-error";
    default:
        return "unknown";
    }
}

static void capture_error(const char *action, esp_err_t err,
                          esp_err_t *first_error)
{
    if (err == ESP_OK) {
        return;
    }

    ESP_LOGE(TAG, "%s failed: %s", action, esp_err_to_name(err));
    if (*first_error == ESP_OK) {
        *first_error = err;
    }
}

static void derate_brightness_request_if_needed(const app_state_t *state,
                                                const safety_policy_t *policy,
                                                esp_err_t *first_error)
{
    if (!state->light_enabled
        || !(policy->low_battery || policy->critical_battery)
        || !policy->light_allowed
        || policy->light_limit_percent == 0
        || state->brightness_percent <= policy->light_limit_percent) {
        return;
    }

    const int32_t delta =
        (int32_t)policy->light_limit_percent -
        (int32_t)state->brightness_percent;
    esp_err_t err = app_state_adjust_brightness(delta);
    capture_error("apply low-battery brightness derate", err, first_error);
    if (err != ESP_OK) {
        return;
    }

    err = app_settings_schedule_save(APP_SETTINGS_CHANGED_BRIGHTNESS);
    capture_error("schedule low-battery brightness save", err, first_error);
    ESP_LOGW(TAG, "Low-battery derate wrote brightness request %u%% -> %u%%",
             (unsigned int)state->brightness_percent,
             (unsigned int)policy->light_limit_percent);
}

static safety_policy_t evaluate_policy(const app_state_t *state,
                                       uint32_t now_ms)
{
    safety_policy_t policy = {
        .light_limit_percent = SAFETY_LIGHT_LIMIT_FULL_PERCENT,
        .fan_percent = calculate_fan_percent(state->ntc_temp_c),
        .light_allowed = true,
        .fault = SYSTEM_FAULT_NONE,
    };
    const bool battery_valid = isfinite(state->battery_voltage_v);
    const bool ntc_valid = isfinite(state->ntc_temp_c);

    if (condition_sustained(battery_valid
                            && state->battery_voltage_v < SAFETY_BATTERY_LOW_V,
                            &low_battery_tracker, now_ms,
                            SAFETY_BATTERY_LOW_DURATION_MS)) {
        low_battery_active = true;
    }
    if (condition_sustained(battery_valid
                            && state->battery_voltage_v
                               < SAFETY_BATTERY_CRITICAL_V,
                            &critical_battery_tracker, now_ms,
                            SAFETY_BATTERY_CRITICAL_DURATION_MS)) {
        critical_battery_active = true;
    }
    if (condition_sustained(battery_valid
                            && state->battery_voltage_v
                               > SAFETY_BATTERY_RECOVERY_V,
                            &recovery_battery_tracker, now_ms,
                            SAFETY_BATTERY_RECOVERY_DURATION_MS)) {
        low_battery_active = false;
        critical_battery_active = false;
        severe_battery_force_off_active = false;
    }
    if (condition_sustained(battery_valid
                            && state->battery_voltage_v
                               < SAFETY_BATTERY_FORCE_OFF_V,
                            &force_off_battery_tracker, now_ms,
                            SAFETY_BATTERY_FORCE_OFF_DURATION_MS)) {
        severe_battery_force_off_active = true;
    }

    if (ntc_valid && state->ntc_temp_c >= SAFETY_TEMP_FORCE_OFF_C) {
        over_temperature_active = true;
    } else if (ntc_valid && state->ntc_temp_c < SAFETY_TEMP_RECOVERY_C) {
        over_temperature_active = false;
    }

    policy.low_battery = low_battery_active;
    policy.critical_battery = critical_battery_active;
    policy.over_temperature = over_temperature_active;

    if (policy.low_battery) {
        policy.light_limit_percent =
            minimum_u8(policy.light_limit_percent,
                       SAFETY_LIGHT_LIMIT_LOW_BATTERY_PERCENT);
        policy.fault = SYSTEM_FAULT_LOW_BATTERY;
    }
    if (policy.critical_battery) {
        policy.light_limit_percent =
            minimum_u8(policy.light_limit_percent,
                       SAFETY_LIGHT_LIMIT_CRITICAL_BATTERY_PERCENT);
        policy.fault = SYSTEM_FAULT_CRITICAL_BATTERY;
    }
    if (ntc_valid && state->ntc_temp_c >= SAFETY_TEMP_DERATE_C) {
        policy.light_limit_percent =
            minimum_u8(policy.light_limit_percent, 70);
    }
    if (policy.over_temperature) {
        policy.light_limit_percent = 0;
        policy.light_allowed = false;
        policy.fault = SYSTEM_FAULT_OVER_TEMP;
    }
    if (!battery_valid) {
        policy.light_limit_percent =
            minimum_u8(policy.light_limit_percent,
                       SAFETY_LIGHT_LIMIT_ADC_ERROR_PERCENT);
        policy.fault = SYSTEM_FAULT_ADC_ERROR;
    }
    if (!ntc_valid) {
        policy.light_limit_percent = 0;
        policy.fan_percent = SAFETY_FAN_MAX_PERCENT;
        policy.light_allowed = false;
        policy.fault = SYSTEM_FAULT_NTC_ERROR;
    }
    policy.severe_battery_force_off = severe_battery_force_off_active;
    if (policy.severe_battery_force_off) {
        policy.light_limit_percent = 0;
        policy.light_allowed = false;
        policy.fault = SYSTEM_FAULT_CRITICAL_BATTERY;
    }
    if (state->manual_fan_enabled
        && policy.fan_percent < SAFETY_FAN_MAX_PERCENT) {
        policy.fan_percent = SAFETY_FAN_MAX_PERCENT;
    }

    return policy;
}

static esp_err_t apply_policy(const app_state_t *state,
                              const safety_policy_t *policy)
{
    esp_err_t first_error = ESP_OK;
    const bool policy_changed =
        light_limit_percent != policy->light_limit_percent
        || light_allowed != policy->light_allowed;

    light_limit_percent = policy->light_limit_percent;
    light_allowed = policy->light_allowed;

    app_state_safety_update_t safety_update = {
        .fan_percent = policy->fan_percent,
        .low_battery = policy->low_battery,
        .critical_battery = policy->critical_battery,
        .over_temperature = policy->over_temperature,
        .fault = policy->fault,
    };
    capture_error("publish safety state", app_state_update_safety(&safety_update),
                  &first_error);
    derate_brightness_request_if_needed(state, policy, &first_error);

    if (applied_fan_percent != policy->fan_percent) {
        esp_err_t err = hal_fan_set_percent(policy->fan_percent);
        capture_error("set safety fan output", err, &first_error);
        if (err == ESP_OK) {
            applied_fan_percent = policy->fan_percent;
        }
    }

    if (!policy->light_allowed && !outputs_forced_off) {
        capture_error("clear light request", app_state_set_light_enabled(false),
                      &first_error);
        esp_err_t led_err = hal_led_off();
        capture_error("force LED output off", led_err, &first_error);
        if (led_err == ESP_OK) {
            outputs_forced_off = true;
        }
    } else if (policy->light_allowed) {
        outputs_forced_off = false;
    }

    if (policy_changed) {
        ESP_LOGI(TAG, "Policy allowed=%s limit=%u%% fan=%u%% fault=%s",
                 policy->light_allowed ? "yes" : "no",
                 (unsigned int)policy->light_limit_percent,
                 (unsigned int)policy->fan_percent,
                 fault_name(policy->fault));
    }

    return first_error;
}

esp_err_t app_safety_evaluate_now(void)
{
    if (!initialized || safety_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(safety_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    app_state_t state;
    esp_err_t err = app_state_get(&state);
    if (err == ESP_OK) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        safety_policy_t policy = evaluate_policy(&state, now_ms);
        err = apply_policy(&state, &policy);
    }

    xSemaphoreGive(safety_mutex);
    return err;
}

static void safety_task(void *context)
{
    (void)context;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SAFETY_TASK_PERIOD_MS));
        esp_err_t err = app_safety_evaluate_now();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Periodic evaluation failed: %s",
                     esp_err_to_name(err));
        }
    }
}

esp_err_t app_safety_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    safety_mutex = xSemaphoreCreateMutex();
    if (safety_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    light_limit_percent = 0;
    light_allowed = false;
    initialized = true;

    esp_err_t err = app_safety_evaluate_now();
    if (err != ESP_OK) {
        initialized = false;
        vSemaphoreDelete(safety_mutex);
        safety_mutex = NULL;
        return err;
    }

    if (xTaskCreate(safety_task, "app_safety", SAFETY_TASK_STACK_SIZE, NULL,
                    SAFETY_TASK_PRIORITY, NULL) != pdPASS) {
        initialized = false;
        vSemaphoreDelete(safety_mutex);
        safety_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Safety manager started with %u ms period",
             (unsigned int)SAFETY_TASK_PERIOD_MS);
    return ESP_OK;
}

uint8_t app_safety_get_light_limit(void)
{
    app_safety_light_policy_t policy;
    if (app_safety_get_light_policy(&policy) != ESP_OK) {
        return 0;
    }
    return policy.limit_percent;
}

bool app_safety_light_allowed(void)
{
    app_safety_light_policy_t policy;
    if (app_safety_get_light_policy(&policy) != ESP_OK) {
        return false;
    }
    return policy.allowed;
}

esp_err_t app_safety_get_light_policy(app_safety_light_policy_t *policy)
{
    if (policy == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!initialized || safety_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(safety_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    policy->limit_percent = light_limit_percent;
    policy->allowed = light_allowed;
    xSemaphoreGive(safety_mutex);
    return ESP_OK;
}
