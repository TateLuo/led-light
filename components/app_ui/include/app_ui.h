#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    APP_UI_SETTINGS_ITEM_BACKLIGHT = 0,
    APP_UI_SETTINGS_ITEM_CCT_STEP,
    APP_UI_SETTINGS_ITEM_FAN_MANUAL,
    APP_UI_SETTINGS_ITEM_WIFI_OTA,
    APP_UI_SETTINGS_ITEM_TEST_RECORD,
    APP_UI_SETTINGS_ITEM_FAN_CURVE,
    APP_UI_SETTINGS_ITEM_THERMAL_GUARD,
    APP_UI_SETTINGS_ITEM_COUNT,
} app_ui_settings_item_t;

/**
 * @brief Create the LVGL status screen and start periodic UI updates.
 *
 * Renders a read-only snapshot from app_state. Hardware output and safety
 * policy remain owned by app_light and app_safety.
 */
esp_err_t app_ui_init(void);

/**
 * @brief Show a temporary status message in the UI status area.
 *
 * The message is shown for duration_ms and then normal status rendering
 * resumes. Hardware output remains owned by the HAL and application logic.
 */
esp_err_t app_ui_show_message(const char *message, uint32_t duration_ms);

/**
 * @brief Request an immediate UI refresh after a user-visible state change.
 *
 * This is intended for input-driven changes such as encoder rotation so the
 * control display can start moving without waiting for the periodic refresh.
 */
esp_err_t app_ui_request_refresh(void);

/**
 * @brief Enter or leave the settings page.
 */
esp_err_t app_ui_set_settings_page(bool enabled);

/**
 * @brief Return true when the settings page is active.
 */
bool app_ui_settings_page_active(void);

/**
 * @brief Move the highlighted settings item by delta steps.
 */
esp_err_t app_ui_settings_move_selection(int32_t delta);

/**
 * @brief Return the currently highlighted settings item.
 */
app_ui_settings_item_t app_ui_settings_selected_item(void);
