#include "app_settings.h"

#include <stdbool.h>
#include <stdint.h>

#include "app_state.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "APP_SETTINGS";

#define APP_SETTINGS_NAMESPACE                  "light_cfg"
#define APP_SETTINGS_KEY_BRIGHTNESS             "brightness"
#define APP_SETTINGS_KEY_CCT                    "cct"
#define APP_SETTINGS_KEY_LIGHT_ON               "light_on"
#define APP_SETTINGS_KEY_BACKLIGHT              "backlight"
#define APP_SETTINGS_KEY_MANUAL_FAN             "fan_manual"
#define APP_SETTINGS_KEY_CCT_STEP               "cct_step"
#define APP_SETTINGS_KEY_UI_REV                 "ui_rev"

#define APP_SETTINGS_DEFAULT_BRIGHTNESS_PERCENT APP_STATE_BRIGHTNESS_DEFAULT_PERCENT
#define APP_SETTINGS_DEFAULT_CCT_KELVIN         APP_STATE_CCT_DEFAULT_KELVIN
#define APP_SETTINGS_DEFAULT_LIGHT_ON           false
#define APP_SETTINGS_DEFAULT_BACKLIGHT_PERCENT  APP_SETTINGS_BACKLIGHT_DEFAULT_PERCENT
#define APP_SETTINGS_DEFAULT_MANUAL_FAN         false
#define APP_SETTINGS_DEFAULT_CCT_STEP_KELVIN    APP_SETTINGS_CCT_STEP_DEFAULT_KELVIN
#define APP_SETTINGS_UI_REV                     1

#define APP_SETTINGS_SAVE_DELAY_MS              3000
#define APP_SETTINGS_TASK_STACK_SIZE            3072
#define APP_SETTINGS_TASK_PRIORITY              2

typedef struct {
    uint8_t brightness_percent;
    uint16_t cct_kelvin;
    bool light_on;
    uint8_t backlight_percent;
    bool manual_fan_enabled;
    uint16_t cct_step_kelvin;
} app_settings_config_t;

static SemaphoreHandle_t settings_mutex;
static TaskHandle_t settings_task_handle;
static nvs_handle_t settings_handle;
static app_settings_config_t cached_config;
static bool initialized;
static bool dirty;
static bool shutdown_prepared;
static uint32_t last_change_ms;

static app_settings_config_t default_config(void)
{
    app_settings_config_t config = {
        .brightness_percent = APP_SETTINGS_DEFAULT_BRIGHTNESS_PERCENT,
        .cct_kelvin = APP_SETTINGS_DEFAULT_CCT_KELVIN,
        .light_on = APP_SETTINGS_DEFAULT_LIGHT_ON,
        .backlight_percent = APP_SETTINGS_DEFAULT_BACKLIGHT_PERCENT,
        .manual_fan_enabled = APP_SETTINGS_DEFAULT_MANUAL_FAN,
        .cct_step_kelvin = APP_SETTINGS_DEFAULT_CCT_STEP_KELVIN,
    };
    return config;
}

static bool use_default_for_error(esp_err_t err)
{
    return err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_TYPE_MISMATCH;
}

static bool cct_step_kelvin_supported(uint16_t step_kelvin)
{
    return step_kelvin == 50
           || step_kelvin == 100
           || step_kelvin == 200
           || step_kelvin == 500;
}

static esp_err_t write_cached_config_locked(void)
{
    esp_err_t err = nvs_set_u8(settings_handle, APP_SETTINGS_KEY_BRIGHTNESS,
                               cached_config.brightness_percent);
    if (err == ESP_OK) {
        err = nvs_set_u16(settings_handle, APP_SETTINGS_KEY_CCT,
                          cached_config.cct_kelvin);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(settings_handle, APP_SETTINGS_KEY_LIGHT_ON,
                         cached_config.light_on ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(settings_handle, APP_SETTINGS_KEY_BACKLIGHT,
                         cached_config.backlight_percent);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(settings_handle, APP_SETTINGS_KEY_MANUAL_FAN,
                         cached_config.manual_fan_enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(settings_handle, APP_SETTINGS_KEY_CCT_STEP,
                          cached_config.cct_step_kelvin);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(settings_handle, APP_SETTINGS_KEY_UI_REV,
                          APP_SETTINGS_UI_REV);
    }
    if (err == ESP_OK) {
        err = nvs_commit(settings_handle);
    }
    if (err == ESP_OK) {
        dirty = false;
        ESP_LOGI(TAG, "Saved brightness=%u%% CCT=%uK light=%s backlight=%u%% manual_fan=%s cct_step=%uK",
                 (unsigned int)cached_config.brightness_percent,
                 (unsigned int)cached_config.cct_kelvin,
                 cached_config.light_on ? "on" : "off",
                 (unsigned int)cached_config.backlight_percent,
                 cached_config.manual_fan_enabled ? "on" : "off",
                 (unsigned int)cached_config.cct_step_kelvin);
    }
    return err;
}

static esp_err_t read_u8_or_default(const char *key, uint8_t minimum,
                                    uint8_t maximum, uint8_t *value,
                                    bool *rewrite_required)
{
    uint8_t stored_value;
    esp_err_t err = nvs_get_u8(settings_handle, key, &stored_value);
    if (err == ESP_OK) {
        if (stored_value >= minimum && stored_value <= maximum) {
            *value = stored_value;
        } else {
            *rewrite_required = true;
        }
        return ESP_OK;
    }
    if (use_default_for_error(err)) {
        *rewrite_required = true;
        return ESP_OK;
    }
    return err;
}

static esp_err_t read_u16_or_default(const char *key, uint16_t minimum,
                                     uint16_t maximum, uint16_t *value,
                                     bool *rewrite_required)
{
    uint16_t stored_value;
    esp_err_t err = nvs_get_u16(settings_handle, key, &stored_value);
    if (err == ESP_OK) {
        if (stored_value >= minimum && stored_value <= maximum) {
            *value = stored_value;
        } else {
            *rewrite_required = true;
        }
        return ESP_OK;
    }
    if (use_default_for_error(err)) {
        *rewrite_required = true;
        return ESP_OK;
    }
    return err;
}

static esp_err_t load_cached_config(bool *rewrite_required)
{
    cached_config = default_config();
    *rewrite_required = false;

    uint16_t stored_ui_rev;
    esp_err_t err = nvs_get_u16(settings_handle, APP_SETTINGS_KEY_UI_REV,
                                &stored_ui_rev);
    if (err != ESP_OK) {
        if (use_default_for_error(err)) {
            *rewrite_required = true;
            return ESP_OK;
        }
        return err;
    }
    if (stored_ui_rev != APP_SETTINGS_UI_REV) {
        *rewrite_required = true;
        return ESP_OK;
    }

    err = read_u8_or_default(APP_SETTINGS_KEY_BRIGHTNESS,
                             APP_STATE_BRIGHTNESS_MIN_PERCENT,
                             APP_STATE_BRIGHTNESS_MAX_PERCENT,
                             &cached_config.brightness_percent,
                             rewrite_required);
    if (err == ESP_OK) {
        err = read_u16_or_default(APP_SETTINGS_KEY_CCT,
                                  APP_STATE_CCT_MIN_KELVIN,
                                  APP_STATE_CCT_MAX_KELVIN,
                                  &cached_config.cct_kelvin,
                                  rewrite_required);
    }

    uint8_t light_on = cached_config.light_on ? 1 : 0;
    if (err == ESP_OK) {
        err = read_u8_or_default(APP_SETTINGS_KEY_LIGHT_ON, 0, 1, &light_on,
                                 rewrite_required);
        cached_config.light_on = light_on != 0;
    }
    if (err == ESP_OK) {
        err = read_u8_or_default(APP_SETTINGS_KEY_BACKLIGHT,
                                 APP_SETTINGS_BACKLIGHT_MIN_PERCENT,
                                 APP_SETTINGS_BACKLIGHT_MAX_PERCENT,
                                 &cached_config.backlight_percent,
                                 rewrite_required);
    }
    uint8_t manual_fan_enabled = cached_config.manual_fan_enabled ? 1 : 0;
    if (err == ESP_OK) {
        err = read_u8_or_default(APP_SETTINGS_KEY_MANUAL_FAN, 0, 1,
                                 &manual_fan_enabled,
                                 rewrite_required);
        cached_config.manual_fan_enabled = manual_fan_enabled != 0;
    }
    if (err == ESP_OK) {
        err = read_u16_or_default(APP_SETTINGS_KEY_CCT_STEP,
                                  APP_SETTINGS_CCT_STEP_MIN_KELVIN,
                                  APP_SETTINGS_CCT_STEP_MAX_KELVIN,
                                  &cached_config.cct_step_kelvin,
                                  rewrite_required);
        if (err == ESP_OK
            && !cct_step_kelvin_supported(cached_config.cct_step_kelvin)) {
            cached_config.cct_step_kelvin =
                APP_SETTINGS_DEFAULT_CCT_STEP_KELVIN;
            *rewrite_required = true;
        }
    }
    return err;
}

static esp_err_t lock_settings(void)
{
    if (!initialized || settings_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(settings_mutex, portMAX_DELAY) == pdTRUE ?
           ESP_OK : ESP_ERR_TIMEOUT;
}

static void unlock_settings(void)
{
    xSemaphoreGive(settings_mutex);
}

esp_err_t app_settings_save_now(void)
{
    esp_err_t err = lock_settings();
    if (err != ESP_OK) {
        return err;
    }

    err = write_cached_config_locked();
    unlock_settings();
    return err;
}

esp_err_t app_settings_prepare_shutdown(void)
{
    esp_err_t err = lock_settings();
    if (err != ESP_OK) {
        return err;
    }

    shutdown_prepared = true;
    err = write_cached_config_locked();
    unlock_settings();
    return err;
}

static void app_settings_task(void *context)
{
    (void)context;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (true) {
            esp_err_t err = lock_settings();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to lock delayed-save state: %s",
                         esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(APP_SETTINGS_SAVE_DELAY_MS));
                continue;
            }

            bool save_pending = dirty;
            uint32_t elapsed_ms =
                (uint32_t)((uint32_t)(esp_timer_get_time() / 1000ULL)
                           - last_change_ms);
            unlock_settings();

            if (!save_pending) {
                break;
            }
            if (elapsed_ms < APP_SETTINGS_SAVE_DELAY_MS) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(
                    APP_SETTINGS_SAVE_DELAY_MS - elapsed_ms));
                continue;
            }

            err = app_settings_save_now();
            if (err == ESP_OK) {
                break;
            }

            ESP_LOGE(TAG, "Delayed NVS save failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(APP_SETTINGS_SAVE_DELAY_MS));
        }
    }
}

esp_err_t app_settings_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES
        || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Reinitializing incompatible or full NVS partition");
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_open(APP_SETTINGS_NAMESPACE, NVS_READWRITE, &settings_handle);
    if (err != ESP_OK) {
        return err;
    }

    settings_mutex = xSemaphoreCreateMutex();
    if (settings_mutex == NULL) {
        nvs_close(settings_handle);
        return ESP_ERR_NO_MEM;
    }

    bool rewrite_required;
    err = load_cached_config(&rewrite_required);
    if (err != ESP_OK) {
        vSemaphoreDelete(settings_mutex);
        settings_mutex = NULL;
        nvs_close(settings_handle);
        return err;
    }

    initialized = true;
    if (rewrite_required) {
        ESP_LOGW(TAG, "Writing validated default values for incomplete settings");
        err = app_settings_save_now();
        if (err != ESP_OK) {
            initialized = false;
            vSemaphoreDelete(settings_mutex);
            settings_mutex = NULL;
            nvs_close(settings_handle);
            return err;
        }
    }

    if (xTaskCreate(app_settings_task, "app_settings",
                    APP_SETTINGS_TASK_STACK_SIZE, NULL,
                    APP_SETTINGS_TASK_PRIORITY,
                    &settings_task_handle) != pdPASS) {
        initialized = false;
        vSemaphoreDelete(settings_mutex);
        settings_mutex = NULL;
        nvs_close(settings_handle);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Loaded brightness=%u%% CCT=%uK light=%s backlight=%u%% manual_fan=%s cct_step=%uK",
             (unsigned int)cached_config.brightness_percent,
             (unsigned int)cached_config.cct_kelvin,
             cached_config.light_on ? "on" : "off",
             (unsigned int)cached_config.backlight_percent,
             cached_config.manual_fan_enabled ? "on" : "off",
             (unsigned int)cached_config.cct_step_kelvin);
    return ESP_OK;
}

esp_err_t app_settings_restore_state(void)
{
    esp_err_t err = lock_settings();
    if (err != ESP_OK) {
        return err;
    }

    app_state_user_settings_t settings = {
        .brightness_percent = cached_config.brightness_percent,
        .cct_kelvin = cached_config.cct_kelvin,
        .light_enabled = cached_config.light_on,
        .manual_fan_enabled = cached_config.manual_fan_enabled,
    };
    unlock_settings();
    return app_state_update_user_settings(&settings);
}

esp_err_t app_settings_schedule_save(uint32_t changed_fields)
{
    if ((changed_fields & ~APP_SETTINGS_CHANGED_ALL) != 0
        || changed_fields == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    app_state_t state;
    esp_err_t err = app_state_get(&state);
    if (err != ESP_OK) {
        return err;
    }

    err = lock_settings();
    if (err != ESP_OK) {
        return err;
    }
    if (shutdown_prepared) {
        unlock_settings();
        return ESP_ERR_INVALID_STATE;
    }

    if ((changed_fields & APP_SETTINGS_CHANGED_BRIGHTNESS) != 0) {
        cached_config.brightness_percent = state.brightness_percent;
    }
    if ((changed_fields & APP_SETTINGS_CHANGED_CCT) != 0) {
        cached_config.cct_kelvin = state.cct_kelvin;
    }
    if ((changed_fields & APP_SETTINGS_CHANGED_LIGHT_ON) != 0) {
        cached_config.light_on = state.light_enabled;
    }
    if ((changed_fields & APP_SETTINGS_CHANGED_BACKLIGHT) != 0) {
        /* Backlight is cached by app_settings_set_backlight_percent(). */
    }
    if ((changed_fields & APP_SETTINGS_CHANGED_MANUAL_FAN) != 0) {
        cached_config.manual_fan_enabled = state.manual_fan_enabled;
    }
    dirty = true;
    last_change_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    unlock_settings();
    xTaskNotifyGive(settings_task_handle);
    return ESP_OK;
}

uint8_t app_settings_get_backlight_percent(void)
{
    if (lock_settings() != ESP_OK) {
        return APP_SETTINGS_DEFAULT_BACKLIGHT_PERCENT;
    }

    uint8_t backlight_percent = cached_config.backlight_percent;
    unlock_settings();
    return backlight_percent;
}

esp_err_t app_settings_set_backlight_percent(uint8_t percent)
{
    if (percent < APP_SETTINGS_BACKLIGHT_MIN_PERCENT
        || percent > APP_SETTINGS_BACKLIGHT_MAX_PERCENT) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lock_settings();
    if (err != ESP_OK) {
        return err;
    }
    if (shutdown_prepared) {
        unlock_settings();
        return ESP_ERR_INVALID_STATE;
    }

    cached_config.backlight_percent = percent;
    dirty = true;
    last_change_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    unlock_settings();
    xTaskNotifyGive(settings_task_handle);
    return ESP_OK;
}

uint16_t app_settings_get_cct_step_kelvin(void)
{
    if (lock_settings() != ESP_OK) {
        return APP_SETTINGS_DEFAULT_CCT_STEP_KELVIN;
    }

    uint16_t cct_step_kelvin = cached_config.cct_step_kelvin;
    unlock_settings();
    return cct_step_kelvin;
}

esp_err_t app_settings_set_cct_step_kelvin(uint16_t step_kelvin)
{
    if (!cct_step_kelvin_supported(step_kelvin)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lock_settings();
    if (err != ESP_OK) {
        return err;
    }
    if (shutdown_prepared) {
        unlock_settings();
        return ESP_ERR_INVALID_STATE;
    }

    cached_config.cct_step_kelvin = step_kelvin;
    dirty = true;
    last_change_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    unlock_settings();
    xTaskNotifyGive(settings_task_handle);
    return ESP_OK;
}
