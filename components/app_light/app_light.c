#include "app_light.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "app_safety.h"
#include "app_state.h"
#include "hal_power.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal_led.h"

static const char *TAG = "APP_LIGHT";

#define APP_LIGHT_TASK_STACK_SIZE       3072
#define APP_LIGHT_TASK_PRIORITY         4
#define APP_LIGHT_SYNC_PERIOD_MS        50
#define APP_LIGHT_POLICY_RETRY_COUNT    2

typedef struct {
    float gamma;
    float warm_flux_gain;
    float cold_flux_gain;
    float warm_channel_max;
    float cold_channel_max;
    float warm_max_power_w;
    float cold_max_power_w;
    float total_led_power_max_w;
    float output_slew_rate_per_s;
    float minimum_output_duty;
} app_light_cct_config_t;

typedef struct {
    float warm_target;
    float cold_target;
    float warm_current;
    float cold_current;
} app_light_cct_controller_t;

static const app_light_cct_config_t cct_config = {
    .gamma = 1.8f,
    .warm_flux_gain = 1.0f,      // TODO: 需实测校准暖光相对照度
    .cold_flux_gain = 1.0f,      // TODO: 需实测校准冷光相对照度
    .warm_channel_max = 0.80f,   // 样机实测：纯暖 100% 输出限到 80% 后不再断电
    .cold_channel_max = 0.80f,   // 样机实测：纯冷 100% 输出限到 80% 后不再断电
    .warm_max_power_w = 24.5f,   // TODO: 需实测暖光 LED 电压/电流
    .cold_max_power_w = 24.5f,   // TODO: 需实测冷光 LED 电压/电流
    .total_led_power_max_w = 24.5f,
    .output_slew_rate_per_s = 2.5f,
    .minimum_output_duty = 0.0f, // TODO: 需实测 HI6000B 最小稳定 PWM duty
};

static SemaphoreHandle_t light_mutex;
static bool initialized;
static bool shutdown_prepared;
static bool sync_error_logged;
static int64_t last_update_us;
static app_light_cct_controller_t cct_controller;
static app_light_runtime_inputs_t runtime_inputs;

static float clamp_float(float value, float minimum, float maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static float positive_or_one(float value)
{
    if (isfinite(value) && value > 0.0f) {
        return value;
    }
    return 1.0f;
}

static float app_light_battery_voltage_power_scale(float battery_voltage_v,
                                                   bool valid)
{
    (void)battery_voltage_v;
    (void)valid;
    return 1.0f;
}

static float app_light_thermal_scale(float ntc_temp_c, bool valid)
{
    (void)ntc_temp_c;
    (void)valid;
    return 1.0f;
}

static float app_light_low_battery_brightness_cap(uint8_t battery_percent,
                                                  bool valid)
{
    (void)battery_percent;
    (void)valid;
    return 1.0f;
}

static float app_light_estimated_power_scale(float warm_linear,
                                             float cold_linear)
{
    const float led_power_w =
        cct_config.warm_max_power_w * warm_linear +
        cct_config.cold_max_power_w * cold_linear;

    if (cct_config.total_led_power_max_w <= 0.0f ||
        led_power_w <= cct_config.total_led_power_max_w) {
        return 1.0f;
    }

    return clamp_float(cct_config.total_led_power_max_w / led_power_w,
                       0.0f, 1.0f);
}

static void cct_light_force_off(void)
{
    cct_controller.warm_target = 0.0f;
    cct_controller.cold_target = 0.0f;
    cct_controller.warm_current = 0.0f;
    cct_controller.cold_current = 0.0f;
}

static float cct_light_slew(float current, float target, float max_delta)
{
    const float delta = target - current;
    if (delta > max_delta) {
        return current + max_delta;
    }
    if (delta < -max_delta) {
        return current - max_delta;
    }
    return target;
}

static void cct_light_set_target(float brightness_0_1, float kelvin)
{
    const float brightness =
        clamp_float(brightness_0_1, 0.0f, 1.0f);
    const float clamped_kelvin =
        clamp_float(kelvin, (float)APP_STATE_CCT_MIN_KELVIN,
                    (float)APP_STATE_CCT_MAX_KELVIN);

    const float warm_mired = 1000000.0f /
                             (float)APP_STATE_CCT_MIN_KELVIN;
    const float cold_mired = 1000000.0f /
                             (float)APP_STATE_CCT_MAX_KELVIN;
    const float target_mired = 1000000.0f / clamped_kelvin;
    const float cold_ratio =
        clamp_float((warm_mired - target_mired) /
                    (warm_mired - cold_mired), 0.0f, 1.0f);
    const float warm_ratio = 1.0f - cold_ratio;

    const float brightness_pwm = powf(brightness, cct_config.gamma);
    float warm_linear = brightness_pwm * warm_ratio /
                        positive_or_one(cct_config.warm_flux_gain);
    float cold_linear = brightness_pwm * cold_ratio /
                        positive_or_one(cct_config.cold_flux_gain);

    float scale = 1.0f;
    if (warm_linear > 0.0f) {
        scale = fminf(scale, cct_config.warm_channel_max / warm_linear);
    }
    if (cold_linear > 0.0f) {
        scale = fminf(scale, cct_config.cold_channel_max / cold_linear);
    }
    scale = clamp_float(scale, 0.0f, 1.0f);
    warm_linear *= scale;
    cold_linear *= scale;

    float scale_power =
        app_light_estimated_power_scale(warm_linear, cold_linear);
    scale_power =
        fminf(scale_power,
              app_light_battery_voltage_power_scale(
                  runtime_inputs.battery_voltage_v,
                  runtime_inputs.battery_voltage_valid));
    scale_power =
        fminf(scale_power,
              app_light_thermal_scale(runtime_inputs.ntc_temp_c,
                                      runtime_inputs.ntc_temp_valid));
    scale_power =
        fminf(scale_power,
              app_light_low_battery_brightness_cap(
                  runtime_inputs.battery_percent,
                  runtime_inputs.battery_percent_valid));
    scale_power = clamp_float(scale_power, 0.0f, 1.0f);

    cct_controller.warm_target = warm_linear * scale_power;
    cct_controller.cold_target = cold_linear * scale_power;
}

static void cct_light_update(float dt_s)
{
    const float dt = clamp_float(dt_s, 0.0f, 1.0f);
    const float max_delta = cct_config.output_slew_rate_per_s * dt;

    cct_controller.warm_current =
        cct_light_slew(cct_controller.warm_current,
                       cct_controller.warm_target, max_delta);
    cct_controller.cold_current =
        cct_light_slew(cct_controller.cold_current,
                       cct_controller.cold_target, max_delta);
}

static uint16_t cct_light_linear_to_duty(float linear)
{
    float clamped = clamp_float(linear, 0.0f, 1.0f);
    if (clamped > 0.0f && clamped < cct_config.minimum_output_duty) {
        clamped = cct_config.minimum_output_duty;
    }
    return (uint16_t)(clamped * (float)HAL_LED_DUTY_MAX + 0.5f);
}

static void cct_light_compute_pwm(uint16_t *warm_duty, uint16_t *cold_duty)
{
    *warm_duty = cct_light_linear_to_duty(cct_controller.warm_current);
    *cold_duty = cct_light_linear_to_duty(cct_controller.cold_current);
}

static float app_light_update_dt_s(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (last_update_us == 0) {
        last_update_us = now_us;
        return (float)APP_LIGHT_SYNC_PERIOD_MS / 1000.0f;
    }

    const float dt_s = (float)(now_us - last_update_us) / 1000000.0f;
    last_update_us = now_us;
    return clamp_float(dt_s, 0.001f, 1.0f);
}

static esp_err_t set_mixed_output(uint8_t brightness_percent,
                                  uint16_t cct_kelvin)
{
    cct_light_set_target((float)brightness_percent / 100.0f,
                         (float)cct_kelvin);
    cct_light_update(app_light_update_dt_s());

    uint16_t warm_duty = 0;
    uint16_t cold_duty = 0;
    cct_light_compute_pwm(&warm_duty, &cold_duty);

    return hal_led_set_cw(cold_duty, warm_duty);
}

static esp_err_t apply_request_locked(const light_request_t *request)
{
    if (shutdown_prepared || !request->enabled) {
        cct_light_force_off();
        return hal_led_off();
    }

    if (!hal_power_external_supply_ready()) {
        ESP_LOGW(TAG, "External supply not ready; delaying LED output");
        cct_light_force_off();
        return hal_led_off();
    }

    for (uint8_t attempt = 0; attempt < APP_LIGHT_POLICY_RETRY_COUNT;
         ++attempt) {
        app_safety_light_policy_t policy;
        esp_err_t err = app_safety_get_light_policy(&policy);
        if (err != ESP_OK) {
            (void)hal_led_off();
            return err;
        }
        if (!policy.allowed || policy.limit_percent == 0) {
            cct_light_force_off();
            return hal_led_off();
        }

        const uint8_t effective_brightness =
            request->brightness_percent < policy.limit_percent ?
            request->brightness_percent : policy.limit_percent;

        err = set_mixed_output(effective_brightness, request->cct_kelvin);
        if (err != ESP_OK) {
            return err;
        }

        app_safety_light_policy_t confirmed_policy;
        err = app_safety_get_light_policy(&confirmed_policy);
        if (err != ESP_OK) {
            (void)hal_led_off();
            return err;
        }
        if (!confirmed_policy.allowed || confirmed_policy.limit_percent == 0) {
            cct_light_force_off();
            return hal_led_off();
        }
        if (confirmed_policy.limit_percent >= effective_brightness) {
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Safety limit changed repeatedly; keeping LED output off");
    cct_light_force_off();
    return hal_led_off();
}

esp_err_t app_light_set(const light_request_t *request)
{
    if (request == NULL
        || request->brightness_percent < APP_STATE_BRIGHTNESS_MIN_PERCENT
        || request->brightness_percent > APP_STATE_BRIGHTNESS_MAX_PERCENT
        || request->cct_kelvin < APP_STATE_CCT_MIN_KELVIN
        || request->cct_kelvin > APP_STATE_CCT_MAX_KELVIN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!initialized || light_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(light_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = apply_request_locked(request);
    xSemaphoreGive(light_mutex);
    return err;
}

esp_err_t app_light_off(void)
{
    if (!initialized || light_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(light_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    cct_light_force_off();
    esp_err_t err = hal_led_off();
    xSemaphoreGive(light_mutex);
    return err;
}

esp_err_t app_light_sync_now(void)
{
    app_state_t state;
    esp_err_t err = app_state_get(&state);
    if (err != ESP_OK) {
        return err;
    }

    const light_request_t request = {
        .enabled = state.light_enabled,
        .brightness_percent = state.brightness_percent,
        .cct_kelvin = state.cct_kelvin,
    };
    return app_light_set(&request);
}

esp_err_t app_light_update_runtime_inputs(
    const app_light_runtime_inputs_t *inputs)
{
    if (inputs == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!initialized || light_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(light_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    runtime_inputs = *inputs;
    xSemaphoreGive(light_mutex);
    return ESP_OK;
}

esp_err_t app_light_prepare_shutdown(void)
{
    if (!initialized || light_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(light_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    shutdown_prepared = true;
    cct_light_force_off();
    esp_err_t err = hal_led_force_safe_off(true);
    xSemaphoreGive(light_mutex);
    return err;
}

static void app_light_task(void *context)
{
    (void)context;

    while (true) {
        esp_err_t err = app_light_sync_now();
        if (err != ESP_OK && !sync_error_logged) {
            ESP_LOGE(TAG, "LED synchronization failed: %s",
                     esp_err_to_name(err));
            sync_error_logged = true;
        } else if (err == ESP_OK) {
            sync_error_logged = false;
        }

        vTaskDelay(pdMS_TO_TICKS(APP_LIGHT_SYNC_PERIOD_MS));
    }
}

esp_err_t app_light_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    light_mutex = xSemaphoreCreateMutex();
    if (light_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cct_light_force_off();
    last_update_us = 0;
    runtime_inputs = (app_light_runtime_inputs_t){0};
    initialized = true;

    esp_err_t err = app_light_sync_now();
    if (err != ESP_OK) {
        initialized = false;
        vSemaphoreDelete(light_mutex);
        light_mutex = NULL;
        return err;
    }

    if (xTaskCreate(app_light_task, "app_light", APP_LIGHT_TASK_STACK_SIZE,
                    NULL, APP_LIGHT_TASK_PRIORITY, NULL) != pdPASS) {
        (void)app_light_off();
        initialized = false;
        vSemaphoreDelete(light_mutex);
        light_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Light controller started with %u ms synchronization period",
             (unsigned int)APP_LIGHT_SYNC_PERIOD_MS);
    return ESP_OK;
}
