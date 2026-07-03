#include "app_battery_test.h"
#include "app_input.h"
#include "app_light.h"
#include "app_ota.h"
#include "app_sensor.h"
#include "app_settings.h"
#include "app_state.h"
#include "app_safety.h"
#include "app_ui.h"
#include "board.h"
#include "hal_adc.h"
#include "hal_display.h"
#include "hal_fan.h"
#include "hal_input.h"
#include "hal_led.h"
#include "hal_power.h"

#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APP_MAIN";

#define APP_MAIN_STARTUP_MESSAGE_MS 1500
#define APP_MAIN_PROMPT_BACKLIGHT_MIN 30

static uint8_t visible_startup_backlight_percent(uint8_t saved_percent)
{
    return saved_percent < APP_MAIN_PROMPT_BACKLIGHT_MIN ?
           APP_MAIN_PROMPT_BACKLIGHT_MIN : saved_percent;
}

void app_main(void)
{
    esp_err_t err = board_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Board init failed: %s", esp_err_to_name(err));
        return;
    }

    err = hal_power_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Power HAL init failed: %s", esp_err_to_name(err));
        return;
    }

    err = hal_power_hold(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to assert power hold: %s", esp_err_to_name(err));
        return;
    }

    err = app_settings_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Settings init failed: %s", esp_err_to_name(err));
        return;
    }

    err = app_state_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "State init failed: %s", esp_err_to_name(err));
        return;
    }

    err = app_settings_restore_state();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Restoring settings failed: %s", esp_err_to_name(err));
        return;
    }

    err = app_ota_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA init failed: %s", esp_err_to_name(err));
        return;
    }

    err = hal_led_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED init failed: %s", esp_err_to_name(err));
        return;
    }

    err = hal_fan_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fan init failed: %s", esp_err_to_name(err));
        return;
    }

    err = hal_adc_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC init failed: %s", esp_err_to_name(err));
        return;
    }

    err = hal_input_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Input HAL init failed: %s", esp_err_to_name(err));
        return;
    }

    err = hal_display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed: %s", esp_err_to_name(err));
        return;
    }

    const uint8_t saved_backlight_percent =
        app_settings_get_backlight_percent();
    err = hal_display_set_backlight(saved_backlight_percent);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set backlight: %s", esp_err_to_name(err));
    }

    err = app_sensor_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Sensor init failed: %s", esp_err_to_name(err));
        return;
    }

    err = app_safety_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Safety init failed: %s", esp_err_to_name(err));
        return;
    }

    err = app_light_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Light init failed: %s", esp_err_to_name(err));
        return;
    }

    err = app_battery_test_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Battery test init failed: %s", esp_err_to_name(err));
        return;
    }

    err = app_ui_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UI init failed: %s", esp_err_to_name(err));
        return;
    }

    const uint8_t prompt_backlight_percent =
        visible_startup_backlight_percent(saved_backlight_percent);
    if (prompt_backlight_percent != saved_backlight_percent) {
        err = hal_display_set_backlight(prompt_backlight_percent);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to light startup prompt: %s",
                     esp_err_to_name(err));
        }
    }

    err = app_ui_show_message(hal_power_key_pressed() ?
                              "POWER ON / RELEASE KEY" : "POWER ON",
                              APP_MAIN_STARTUP_MESSAGE_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to show startup prompt: %s",
                 esp_err_to_name(err));
    }

    if (prompt_backlight_percent != saved_backlight_percent) {
        vTaskDelay(pdMS_TO_TICKS(APP_MAIN_STARTUP_MESSAGE_MS));
        err = hal_display_set_backlight(saved_backlight_percent);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to restore startup backlight: %s",
                     esp_err_to_name(err));
        }
    }

    err = app_input_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Input init failed: %s", esp_err_to_name(err));
        return;
    }

    err = app_ota_confirm_running_firmware();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to confirm running firmware: %s",
                 esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Application started successfully");
}
