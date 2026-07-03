#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint8_t limit_percent;
    bool allowed;
} app_safety_light_policy_t;

/**
 * @brief Start the periodic safety policy task.
 */
esp_err_t app_safety_init(void);

/**
 * @brief Evaluate and apply the safety policy immediately.
 *
 * This is primarily useful for startup evaluation and diagnostics. Normal
 * operation is handled by the periodic safety task.
 */
esp_err_t app_safety_evaluate_now(void);

/**
 * @brief Get the maximum LED brightness currently permitted by safety policy.
 */
uint8_t app_safety_get_light_limit(void);

/**
 * @brief Return whether LED output is currently permitted by safety policy.
 */
bool app_safety_light_allowed(void);

/**
 * @brief Atomically read the LED permission and maximum brightness limit.
 */
esp_err_t app_safety_get_light_policy(app_safety_light_policy_t *policy);
