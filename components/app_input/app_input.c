#include "app_input.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_battery_test.h"
#include "app_light.h"
#include "app_ota.h"
#include "app_safety.h"
#include "app_settings.h"
#include "app_state.h"
#include "app_ui.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_display.h"
#include "hal_fan.h"
#include "hal_input.h"
#include "hal_power.h"

static const char *TAG = "APP_INPUT";

#define APP_INPUT_TASK_STACK_SIZE 4096
#define APP_INPUT_TASK_PRIORITY   6
#define APP_INPUT_WAIT_MS         1000
#define APP_INPUT_BRIGHTNESS_STEP_PERCENT 1
#define APP_INPUT_BACKLIGHT_STEP_PERCENT  10
#define APP_INPUT_BACKLIGHT_MIN_PERCENT   APP_SETTINGS_BACKLIGHT_MIN_PERCENT
#define APP_INPUT_BACKLIGHT_MAX_PERCENT   APP_SETTINGS_BACKLIGHT_MAX_PERCENT
#define APP_INPUT_ROTATION_MEDIUM_MS      140
#define APP_INPUT_ROTATION_FAST_MS        70
#define APP_INPUT_BRIGHTNESS_MEDIUM_MULT  2
#define APP_INPUT_BRIGHTNESS_FAST_MULT    5
#define APP_INPUT_CCT_MEDIUM_MULT         2
#define APP_INPUT_CCT_FAST_MULT           5
#define APP_INPUT_SHUTDOWN_SETTLE_MS      100
#define APP_INPUT_SHUTDOWN_PROMPT_MS      5000
#define APP_INPUT_POWER_OFF_PROMPT_MS     400
#define APP_INPUT_RELEASE_POLL_MS         20
#define APP_INPUT_PROMPT_BACKLIGHT_MIN    30
#define APP_INPUT_LOG_EVENTS              0

static bool initialized;
static bool shutdown_requested;
static uint32_t last_sw1_rotation_ms;
static uint32_t last_sw2_rotation_ms;

static const uint16_t cct_step_options[] = {
    50,
    100,
    200,
    500,
};

static void log_state_error(const char *action, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to %s: %s", action, esp_err_to_name(err));
    }
}

static void refresh_user_control_outputs(void)
{
    esp_err_t err = app_light_sync_now();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to sync light after encoder input: %s",
                 esp_err_to_name(err));
    }

    err = app_ui_request_refresh();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to request immediate UI refresh: %s",
                 esp_err_to_name(err));
    } else {
        taskYIELD();
    }
}

static uint8_t clamp_backlight_percent(int32_t percent)
{
    if (percent < APP_INPUT_BACKLIGHT_MIN_PERCENT) {
        return APP_INPUT_BACKLIGHT_MIN_PERCENT;
    }
    if (percent > APP_INPUT_BACKLIGHT_MAX_PERCENT) {
        return APP_INPUT_BACKLIGHT_MAX_PERCENT;
    }
    return (uint8_t)percent;
}

static bool is_rotation_event(input_event_type_t type)
{
    return type == INPUT_EVENT_SW1_CW
           || type == INPUT_EVENT_SW1_CCW
           || type == INPUT_EVENT_SW2_CW
           || type == INPUT_EVENT_SW2_CCW;
}

static uint8_t rotation_multiplier(uint32_t *last_rotation_ms,
                                   uint32_t timestamp_ms,
                                   uint8_t medium_multiplier,
                                   uint8_t fast_multiplier)
{
    if (*last_rotation_ms == 0) {
        *last_rotation_ms = timestamp_ms;
        return 1;
    }

    const uint32_t elapsed_ms = timestamp_ms - *last_rotation_ms;
    *last_rotation_ms = timestamp_ms;

    if (elapsed_ms <= APP_INPUT_ROTATION_FAST_MS) {
        return fast_multiplier;
    }
    if (elapsed_ms <= APP_INPUT_ROTATION_MEDIUM_MS) {
        return medium_multiplier;
    }
    return 1;
}

static void reset_rotation_acceleration(void)
{
    last_sw1_rotation_ms = 0;
    last_sw2_rotation_ms = 0;
}

#if APP_INPUT_LOG_EVENTS
static const char *event_type_name(input_event_type_t type)
{
    switch (type) {
    case INPUT_EVENT_SW1_CW:
        return "SW1_CW";
    case INPUT_EVENT_SW1_CCW:
        return "SW1_CCW";
    case INPUT_EVENT_SW2_CW:
        return "SW2_CW";
    case INPUT_EVENT_SW2_CCW:
        return "SW2_CCW";
    case INPUT_EVENT_SW2_SHORT_PRESS:
        return "SW2_SHORT_PRESS";
    case INPUT_EVENT_SW2_LONG_PRESS:
        return "SW2_LONG_PRESS";
    case INPUT_EVENT_POWER_SHORT_PRESS:
        return "POWER_SHORT_PRESS";
    case INPUT_EVENT_POWER_LONG_PRESS:
        return "POWER_LONG_PRESS";
    case INPUT_EVENT_NONE:
    default:
        return "NONE";
    }
}
#endif

static uint8_t visible_prompt_backlight_percent(void)
{
    const uint8_t saved_percent = app_settings_get_backlight_percent();
    return saved_percent < APP_INPUT_PROMPT_BACKLIGHT_MIN ?
           APP_INPUT_PROMPT_BACKLIGHT_MIN : saved_percent;
}

static esp_err_t wake_display_request(void)
{
    app_state_t state;
    esp_err_t err = app_state_get(&state);
    if (err != ESP_OK) {
        return err;
    }
    if (state.display_on) {
        return ESP_OK;
    }

    err = app_state_set_display_on(true);
    if (err != ESP_OK) {
        return err;
    }

    err = hal_display_set_backlight(app_settings_get_backlight_percent());
    if (err != ESP_OK) {
        (void)app_state_set_display_on(false);
        return err;
    }

    ESP_LOGI(TAG, "Display woke by encoder rotation");
    return ESP_OK;
}

static bool consume_rotation_to_wake_display(const input_event_t *event)
{
    if (!is_rotation_event(event->type)) {
        return false;
    }

    app_state_t state;
    esp_err_t err = app_state_get(&state);
    if (err != ESP_OK || state.display_on) {
        return false;
    }

    log_state_error("wake display from rotation", wake_display_request());
    reset_rotation_acceleration();
    return true;
}

static void wait_for_power_key_release(void)
{
    log_state_error("mark display on for shutdown prompt",
                    app_state_set_display_on(true));

    esp_err_t err = hal_display_set_backlight(
        visible_prompt_backlight_percent());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to light LCD backlight for shutdown prompt: %s",
                 esp_err_to_name(err));
    }

    log_state_error("show shutdown release prompt",
                    app_ui_show_message("RELEASE KEY TO POWER OFF",
                                        APP_INPUT_SHUTDOWN_PROMPT_MS));

    uint32_t waited_ms = 0;
    while (hal_power_key_pressed()
           && waited_ms < APP_INPUT_SHUTDOWN_PROMPT_MS) {
        vTaskDelay(pdMS_TO_TICKS(APP_INPUT_RELEASE_POLL_MS));
        waited_ms += APP_INPUT_RELEASE_POLL_MS;
    }

    if (hal_power_key_pressed()) {
        ESP_LOGW(TAG, "Power key still held; proceeding with shutdown");
    }

    log_state_error("show power-off prompt",
                    app_ui_show_message("POWER OFF",
                                        APP_INPUT_POWER_OFF_PROMPT_MS));
    vTaskDelay(pdMS_TO_TICKS(APP_INPUT_POWER_OFF_PROMPT_MS));
}

static void request_shutdown(void)
{
    if (shutdown_requested) {
        return;
    }
    shutdown_requested = true;

    ESP_LOGW(TAG, "Power-key long press detected; requesting shutdown");

    wait_for_power_key_release();

    log_state_error("clear light request", app_state_set_light_enabled(false));

    esp_err_t err = app_light_prepare_shutdown();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to turn off LEDs during shutdown: %s",
                 esp_err_to_name(err));
    }

    log_state_error("save settings before shutdown",
                    app_settings_prepare_shutdown());
    log_state_error("mark display off", app_state_set_display_on(false));

    err = hal_display_set_backlight(0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to turn off LCD backlight during shutdown: %s",
                 esp_err_to_name(err));
    }

    err = hal_fan_off();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to turn off fan during shutdown: %s",
                 esp_err_to_name(err));
    }

    vTaskDelay(pdMS_TO_TICKS(APP_INPUT_SHUTDOWN_SETTLE_MS));

    err = hal_power_shutdown();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to release power hold: %s", esp_err_to_name(err));
    }
}

static esp_err_t toggle_display_request(void)
{
    app_state_t state;
    esp_err_t err = app_state_get(&state);
    if (err != ESP_OK) {
        return err;
    }

    const bool display_on = !state.display_on;
    err = app_state_set_display_on(display_on);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t backlight_percent = display_on ?
                                      app_settings_get_backlight_percent() : 0;
    err = hal_display_set_backlight(backlight_percent);
    if (err != ESP_OK) {
        (void)app_state_set_display_on(state.display_on);
        return err;
    }

    ESP_LOGI(TAG, "Display request updated: %s",
             display_on ? "on" : "off");
    return ESP_OK;
}

static esp_err_t open_settings_page(void)
{
    esp_err_t first_error = ESP_OK;
    esp_err_t err = app_state_set_display_on(true);
    if (err != ESP_OK) {
        first_error = err;
    }

    err = hal_display_set_backlight(visible_prompt_backlight_percent());
    if (err != ESP_OK && first_error == ESP_OK) {
        first_error = err;
    }

    err = app_ui_set_settings_page(true);
    if (err != ESP_OK && first_error == ESP_OK) {
        first_error = err;
    }

    return first_error;
}

static esp_err_t toggle_settings_page(void)
{
    reset_rotation_acceleration();
    if (app_ui_settings_page_active()) {
        return app_ui_set_settings_page(false);
    }
    return open_settings_page();
}

static esp_err_t adjust_backlight_setting(int32_t delta)
{
    const uint8_t current = app_settings_get_backlight_percent();
    const uint8_t next = clamp_backlight_percent(
        (int32_t)current + delta * APP_INPUT_BACKLIGHT_STEP_PERCENT);

    esp_err_t err = hal_display_set_backlight(next);
    if (err != ESP_OK) {
        return err;
    }

    err = app_state_set_display_on(true);
    if (err != ESP_OK) {
        return err;
    }

    return app_settings_set_backlight_percent(next);
}

static esp_err_t adjust_cct_step_setting(int32_t delta)
{
    const uint16_t current = app_settings_get_cct_step_kelvin();
    size_t selected = 0;

    for (size_t index = 0;
         index < sizeof(cct_step_options) / sizeof(cct_step_options[0]);
         ++index) {
        if (cct_step_options[index] == current) {
            selected = index;
            break;
        }
    }

    if (delta > 0) {
        if (selected + 1U < sizeof(cct_step_options) /
                            sizeof(cct_step_options[0])) {
            ++selected;
        }
    } else if (delta < 0 && selected > 0) {
        --selected;
    }

    return app_settings_set_cct_step_kelvin(cct_step_options[selected]);
}

static esp_err_t adjust_manual_fan_setting(int32_t delta)
{
    const bool enabled = delta > 0;
    esp_err_t err = app_state_set_manual_fan_enabled(enabled);
    if (err != ESP_OK) {
        return err;
    }

    err = app_settings_schedule_save(APP_SETTINGS_CHANGED_MANUAL_FAN);
    if (err != ESP_OK) {
        return err;
    }

    return app_safety_evaluate_now();
}

static esp_err_t start_ota_setting(void)
{
    esp_err_t err = app_settings_save_now();
    if (err != ESP_OK) {
        (void)app_ui_show_message("OTA SAVE FAILED", 1200);
        return err;
    }

    err = app_ota_start();
    if (err != ESP_OK) {
        (void)app_ui_show_message("OTA START FAILED", 1200);
    }
    return err;
}

static esp_err_t adjust_battery_test_recording(int32_t delta)
{
    if (delta > 0) {
        app_battery_test_status_t status;
        esp_err_t err = app_battery_test_get_status(&status);
        if (err != ESP_OK) {
            return err;
        }
        if (status.run_state == APP_BATTERY_TEST_RUNNING) {
            return app_battery_test_pause();
        }
        return app_battery_test_start();
    }
    if (delta < 0) {
        return app_battery_test_stop();
    }
    return ESP_OK;
}

static esp_err_t apply_ota_event(const input_event_t *event)
{
    if (app_ota_uploading()) {
        return app_ui_show_message("OTA BUSY", 700);
    }

    switch (event->type) {
    case INPUT_EVENT_SW2_SHORT_PRESS:
    case INPUT_EVENT_SW2_LONG_PRESS:
        return app_ota_stop();
    case INPUT_EVENT_POWER_SHORT_PRESS:
        return app_ui_show_message("OTA ACTIVE", 700);
    case INPUT_EVENT_SW1_CW:
    case INPUT_EVENT_SW1_CCW:
    case INPUT_EVENT_SW2_CW:
    case INPUT_EVENT_SW2_CCW:
    case INPUT_EVENT_NONE:
    case INPUT_EVENT_POWER_LONG_PRESS:
    default:
        return ESP_OK;
    }
}

static esp_err_t apply_settings_event(const input_event_t *event)
{
    switch (event->type) {
    case INPUT_EVENT_SW1_CW:
    case INPUT_EVENT_SW1_CCW:
        if (app_ui_settings_selected_item()
            == APP_UI_SETTINGS_ITEM_BACKLIGHT) {
            return adjust_backlight_setting(event->delta);
        }
        if (app_ui_settings_selected_item()
            == APP_UI_SETTINGS_ITEM_CCT_STEP) {
            return adjust_cct_step_setting(event->delta);
        }
        if (app_ui_settings_selected_item()
            == APP_UI_SETTINGS_ITEM_FAN_MANUAL) {
            return adjust_manual_fan_setting(event->delta);
        }
        if (app_ui_settings_selected_item()
            == APP_UI_SETTINGS_ITEM_WIFI_OTA
            && event->delta != 0) {
            return start_ota_setting();
        }
        if (app_ui_settings_selected_item()
            == APP_UI_SETTINGS_ITEM_TEST_RECORD) {
            return adjust_battery_test_recording(event->delta);
        }
        return app_ui_show_message("INFO ONLY", 700);
    case INPUT_EVENT_SW2_CW:
    case INPUT_EVENT_SW2_CCW:
        return app_ui_settings_move_selection(event->delta);
    case INPUT_EVENT_SW2_SHORT_PRESS:
        return app_ui_set_settings_page(false);
    case INPUT_EVENT_POWER_SHORT_PRESS:
        return toggle_display_request();
    case INPUT_EVENT_SW2_LONG_PRESS:
        return toggle_settings_page();
    case INPUT_EVENT_NONE:
    case INPUT_EVENT_POWER_LONG_PRESS:
    default:
        return ESP_OK;
    }
}

static void apply_state_event(const input_event_t *event)
{
    esp_err_t err = ESP_OK;
    const char *action = NULL;
    uint32_t changed_settings = 0;

    if (consume_rotation_to_wake_display(event)) {
        return;
    }

    if (app_ota_active()) {
        reset_rotation_acceleration();
        log_state_error("apply OTA event", apply_ota_event(event));
        return;
    }

    if (event->type == INPUT_EVENT_SW2_LONG_PRESS) {
        log_state_error("toggle settings page", toggle_settings_page());
        return;
    }

    if (app_ui_settings_page_active()) {
        reset_rotation_acceleration();
        log_state_error("apply settings event", apply_settings_event(event));
        return;
    }

    app_battery_test_status_t test_status;
    if (app_battery_test_get_status(&test_status) == ESP_OK
        && (test_status.run_state == APP_BATTERY_TEST_RUNNING
            || test_status.run_state == APP_BATTERY_TEST_PAUSED)
        && event->type != INPUT_EVENT_POWER_SHORT_PRESS) {
        (void)app_ui_show_message(
            test_status.run_state == APP_BATTERY_TEST_RUNNING ?
            "TEST RUNNING" : "TEST PAUSED", 700);
        return;
    }

    switch (event->type) {
    case INPUT_EVENT_SW1_CW:
    case INPUT_EVENT_SW1_CCW: {
        action = "adjust brightness request";
        const uint8_t brightness_multiplier =
            rotation_multiplier(&last_sw1_rotation_ms, event->timestamp_ms,
                                APP_INPUT_BRIGHTNESS_MEDIUM_MULT,
                                APP_INPUT_BRIGHTNESS_FAST_MULT);
        err = app_state_adjust_brightness(
            event->delta * APP_INPUT_BRIGHTNESS_STEP_PERCENT
            * brightness_multiplier);
        changed_settings = APP_SETTINGS_CHANGED_BRIGHTNESS;
        break;
    }
    case INPUT_EVENT_SW2_CW:
    case INPUT_EVENT_SW2_CCW: {
        action = "adjust CCT request";
        const uint8_t cct_multiplier =
            rotation_multiplier(&last_sw2_rotation_ms, event->timestamp_ms,
                                APP_INPUT_CCT_MEDIUM_MULT,
                                APP_INPUT_CCT_FAST_MULT);
        err = app_state_adjust_cct(event->delta
                                   * app_settings_get_cct_step_kelvin()
                                   * cct_multiplier);
        changed_settings = APP_SETTINGS_CHANGED_CCT;
        break;
    }
    case INPUT_EVENT_SW2_SHORT_PRESS:
        action = "toggle light request";
        err = app_state_toggle_light();
        changed_settings = APP_SETTINGS_CHANGED_LIGHT_ON;
        break;
    case INPUT_EVENT_POWER_SHORT_PRESS:
        action = "toggle display request";
        err = toggle_display_request();
        break;
    case INPUT_EVENT_SW2_LONG_PRESS:
        break;
    case INPUT_EVENT_NONE:
    case INPUT_EVENT_POWER_LONG_PRESS:
    default:
        break;
    }

    if (action != NULL) {
        log_state_error(action, err);
        if (err == ESP_OK && changed_settings != 0) {
            if (event->type == INPUT_EVENT_SW1_CW
                || event->type == INPUT_EVENT_SW1_CCW
                || event->type == INPUT_EVENT_SW2_CW
                || event->type == INPUT_EVENT_SW2_CCW
                || event->type == INPUT_EVENT_SW2_SHORT_PRESS) {
                refresh_user_control_outputs();
            }
            log_state_error("schedule settings save",
                            app_settings_schedule_save(changed_settings));
        }
    }
}

static void app_input_task(void *arg)
{
    (void)arg;

    while (true) {
        input_event_t event;
        esp_err_t err = hal_input_read(&event, APP_INPUT_WAIT_MS);
        if (err == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read input event: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(APP_INPUT_WAIT_MS));
            continue;
        }

#if APP_INPUT_LOG_EVENTS
        ESP_LOGI(TAG, "Event %s delta=%ld timestamp_ms=%lu",
                 event_type_name(event.type), (long)event.delta,
                 (unsigned long)event.timestamp_ms);
#endif

        if (shutdown_requested) {
            continue;
        }

        if (event.type == INPUT_EVENT_POWER_LONG_PRESS
            && app_ota_uploading()) {
            log_state_error("show OTA busy prompt",
                            app_ui_show_message("OTA BUSY", 700));
        } else if (event.type == INPUT_EVENT_POWER_LONG_PRESS) {
            request_shutdown();
        } else {
            apply_state_event(&event);
        }
    }
}

esp_err_t app_input_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    BaseType_t task_created = xTaskCreate(app_input_task, "app_input",
                                          APP_INPUT_TASK_STACK_SIZE, NULL,
                                          APP_INPUT_TASK_PRIORITY, NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create input consumer task");
        return ESP_ERR_NO_MEM;
    }

    initialized = true;
    ESP_LOGI(TAG, "Input state-machine task started");
    return ESP_OK;
}
