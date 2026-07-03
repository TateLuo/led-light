#pragma once

#include "esp_err.h"

/**
 * @brief Start the application input event consumer task.
 *
 * Input events update the shared application state for brightness, CCT, and
 * light requests and schedule coalesced NVS saves. app_light synchronizes the
 * physical output after applying safety limits. The power-key long-press
 * shutdown path saves settings synchronously before releasing the power hold.
 */
esp_err_t app_input_init(void);
