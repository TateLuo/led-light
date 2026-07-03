#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Initialize the WiFi OTA service state.
 *
 * This does not start WiFi. The SoftAP and upload server are started only when
 * the user enters the OTA page from settings.
 */
esp_err_t app_ota_init(void);

/**
 * @brief Confirm a newly booted OTA image after application initialization.
 *
 * With bootloader rollback enabled, an OTA image remains pending until the
 * application has initialized all required subsystems and calls this API.
 */
esp_err_t app_ota_confirm_running_firmware(void);

/**
 * @brief Start SoftAP mode and the browser-based firmware upload server.
 */
esp_err_t app_ota_start(void);

/**
 * @brief Stop the OTA server and SoftAP when no upload is in progress.
 */
esp_err_t app_ota_stop(void);

/**
 * @brief Return true when the OTA page/server is active.
 */
bool app_ota_active(void);

/**
 * @brief Return true while firmware or startup-image bytes are being written.
 */
bool app_ota_uploading(void);
