#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define APP_STATE_BRIGHTNESS_MIN_PERCENT      1
#define APP_STATE_BRIGHTNESS_MAX_PERCENT      100
#define APP_STATE_BRIGHTNESS_DEFAULT_PERCENT  50
#define APP_STATE_CCT_MIN_KELVIN              2700
#define APP_STATE_CCT_MAX_KELVIN              6500
#define APP_STATE_CCT_DEFAULT_KELVIN          5100
#define APP_STATE_OTA_TEXT_MAX_LEN            32
#define APP_STATE_OTA_ERROR_MAX_LEN           48

typedef enum {
    CHARGE_STATE_UNKNOWN = 0,
    CHARGE_STATE_NOT_CHARGING,
    CHARGE_STATE_CHARGING,
    CHARGE_STATE_FULL,
} charge_state_t;

typedef enum {
    SYSTEM_FAULT_NONE = 0,
    SYSTEM_FAULT_LOW_BATTERY,
    SYSTEM_FAULT_CRITICAL_BATTERY,
    SYSTEM_FAULT_OVER_TEMP,
    SYSTEM_FAULT_NTC_ERROR,
    SYSTEM_FAULT_ADC_ERROR,
} system_fault_t;

typedef enum {
    APP_OTA_STATUS_IDLE = 0,
    APP_OTA_STATUS_STARTING,
    APP_OTA_STATUS_READY,
    APP_OTA_STATUS_UPLOADING,
    APP_OTA_STATUS_VERIFYING,
    APP_OTA_STATUS_SUCCESS,
    APP_OTA_STATUS_FAILED,
} app_ota_status_t;

typedef struct {
    bool light_enabled;
    uint8_t brightness_percent;
    uint16_t cct_kelvin;

    float battery_voltage_v;
    uint8_t battery_percent;
    charge_state_t charge_state;

    float ntc_temp_c;
    uint8_t fan_percent;
    bool manual_fan_enabled;

    bool low_battery;
    bool critical_battery;
    bool over_temperature;
    bool display_on;

    bool ota_active;
    bool ota_uploading;
    uint8_t ota_progress_percent;
    app_ota_status_t ota_status;
    char ota_ssid[APP_STATE_OTA_TEXT_MAX_LEN];
    char ota_password[APP_STATE_OTA_TEXT_MAX_LEN];
    char ota_ip[APP_STATE_OTA_TEXT_MAX_LEN];
    char ota_error[APP_STATE_OTA_ERROR_MAX_LEN];

    system_fault_t fault;
} app_state_t;

typedef struct {
    float battery_voltage_v;
    uint8_t battery_percent;
    bool battery_valid;

    float ntc_temp_c;
    bool ntc_valid;
} app_state_sensor_update_t;

typedef struct {
    uint8_t fan_percent;
    bool low_battery;
    bool critical_battery;
    bool over_temperature;
    system_fault_t fault;
} app_state_safety_update_t;

typedef struct {
    uint8_t brightness_percent;
    uint16_t cct_kelvin;
    bool light_enabled;
    bool manual_fan_enabled;
} app_state_user_settings_t;

typedef struct {
    bool active;
    bool uploading;
    uint8_t progress_percent;
    app_ota_status_t status;
    const char *ssid;
    const char *password;
    const char *ip;
    const char *error;
} app_state_ota_update_t;

/**
 * @brief Initialize the mutex-protected application state with safe defaults.
 *
 * The default light request is off. Brightness and CCT retain usable values so
 * input changes and settings restoration can update them while outputs
 * remain controlled by app_light and app_safety.
 */
esp_err_t app_state_init(void);

/**
 * @brief Copy the current application state atomically.
 */
esp_err_t app_state_get(app_state_t *state);

/**
 * @brief Replace the full application state after range validation.
 *
 * Prefer the focused update helpers when changing a single ownership domain.
 */
esp_err_t app_state_set(const app_state_t *state);

/**
 * @brief Adjust the requested brightness and clamp it to 1..100 percent.
 */
esp_err_t app_state_adjust_brightness(int32_t delta_percent);

/**
 * @brief Adjust the requested CCT and clamp it to 2700..6500 Kelvin.
 */
esp_err_t app_state_adjust_cct(int32_t delta_kelvin);

/**
 * @brief Set the requested light enabled state.
 *
 * This updates the user request only. Physical output remains owned by
 * app_light, while app_safety can force the hardware output off.
 */
esp_err_t app_state_set_light_enabled(bool enabled);

/**
 * @brief Toggle the requested light enabled state.
 */
esp_err_t app_state_toggle_light(void);

/**
 * @brief Set whether the display is logically enabled.
 */
esp_err_t app_state_set_display_on(bool display_on);

/**
 * @brief Set whether the user requests manual full-speed fan operation.
 *
 * Safety policy remains authoritative: NTC faults and over-temperature can
 * still force the fan on even when manual fan is disabled.
 */
esp_err_t app_state_set_manual_fan_enabled(bool enabled);

/**
 * @brief Atomically update filtered sensor values in the application state.
 *
 * Invalid sensor values are represented as NAN. Fault policy remains owned by
 * app_safety.
 */
esp_err_t app_state_update_sensors(const app_state_sensor_update_t *sensors);

/**
 * @brief Atomically publish safety policy fields.
 */
esp_err_t app_state_update_safety(const app_state_safety_update_t *safety);

/**
 * @brief Atomically restore persisted user settings without replacing runtime fields.
 */
esp_err_t app_state_update_user_settings(
    const app_state_user_settings_t *settings);

/**
 * @brief Atomically publish WiFi OTA status for the UI.
 */
esp_err_t app_state_update_ota(const app_state_ota_update_t *ota);
