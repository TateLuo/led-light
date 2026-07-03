#pragma once

#include <stdint.h>

#include "esp_err.h"

#define APP_SETTINGS_CHANGED_BRIGHTNESS   (1U << 0)
#define APP_SETTINGS_CHANGED_CCT          (1U << 1)
#define APP_SETTINGS_CHANGED_LIGHT_ON     (1U << 2)
#define APP_SETTINGS_CHANGED_BACKLIGHT    (1U << 3)
#define APP_SETTINGS_CHANGED_MANUAL_FAN   (1U << 4)
#define APP_SETTINGS_CHANGED_ALL          (APP_SETTINGS_CHANGED_BRIGHTNESS \
                                           | APP_SETTINGS_CHANGED_CCT \
                                           | APP_SETTINGS_CHANGED_LIGHT_ON \
                                           | APP_SETTINGS_CHANGED_BACKLIGHT \
                                           | APP_SETTINGS_CHANGED_MANUAL_FAN)

#define APP_SETTINGS_BACKLIGHT_MIN_PERCENT     10
#define APP_SETTINGS_BACKLIGHT_MAX_PERCENT     100
#define APP_SETTINGS_BACKLIGHT_DEFAULT_PERCENT 50

#define APP_SETTINGS_CCT_STEP_MIN_KELVIN       50
#define APP_SETTINGS_CCT_STEP_MAX_KELVIN       500
#define APP_SETTINGS_CCT_STEP_DEFAULT_KELVIN   100

/**
 * @brief Initialize NVS and load validated user settings into a local cache.
 *
 * Call this before app_state_init(). Use app_settings_restore_state() after
 * app_state is initialized.
 */
esp_err_t app_settings_init(void);

/**
 * @brief Restore cached brightness, CCT, light state, and manual fan state.
 */
esp_err_t app_settings_restore_state(void);

/**
 * @brief Capture changed user fields and request a delayed NVS save.
 *
 * Repeated calls defer the write until 3 seconds after the final change.
 */
esp_err_t app_settings_schedule_save(uint32_t changed_fields);

/**
 * @brief Synchronously commit cached settings to NVS.
 *
 * Use this during orderly shutdown before releasing the power hold.
 */
esp_err_t app_settings_save_now(void);

/**
 * @brief Freeze user-setting updates and synchronously save before shutdown.
 *
 * Subsequent delayed-save requests are rejected so the pre-shutdown snapshot
 * remains stable if USB power keeps the MCU running.
 */
esp_err_t app_settings_prepare_shutdown(void);

/**
 * @brief Return the cached LCD backlight percentage.
 */
uint8_t app_settings_get_backlight_percent(void);

/**
 * @brief Update the cached LCD backlight percentage and schedule an NVS save.
 *
 * The value must be APP_SETTINGS_BACKLIGHT_MIN_PERCENT to
 * APP_SETTINGS_BACKLIGHT_MAX_PERCENT. It is stored in the settings cache only.
 * The caller remains responsible for applying the new duty to the display HAL.
 */
esp_err_t app_settings_set_backlight_percent(uint8_t percent);

/**
 * @brief Return the cached CCT adjustment step in Kelvin.
 */
uint16_t app_settings_get_cct_step_kelvin(void);

/**
 * @brief Update the cached CCT adjustment step and schedule an NVS save.
 *
 * Valid values are constrained by APP_SETTINGS_CCT_STEP_MIN_KELVIN and
 * APP_SETTINGS_CCT_STEP_MAX_KELVIN. The input layer uses this value as the
 * base CCT encoder step before applying rotation acceleration.
 */
esp_err_t app_settings_set_cct_step_kelvin(uint16_t step_kelvin);
