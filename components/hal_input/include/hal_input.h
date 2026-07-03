#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    INPUT_EVENT_NONE = 0,
    INPUT_EVENT_SW1_CW,
    INPUT_EVENT_SW1_CCW,
    INPUT_EVENT_SW2_CW,
    INPUT_EVENT_SW2_CCW,
    INPUT_EVENT_SW2_SHORT_PRESS,
    INPUT_EVENT_SW2_LONG_PRESS,
    INPUT_EVENT_POWER_SHORT_PRESS,
    INPUT_EVENT_POWER_LONG_PRESS,
} input_event_type_t;

typedef struct {
    input_event_type_t type;
    int32_t delta;
    uint32_t timestamp_ms;
} input_event_t;

/**
 * @brief Initialize encoder GPIO interrupts and the debounced input worker.
 *
 * Encoder inputs and debounced buttons use the polarity and pull settings
 * defined in board_pins.h.
 */
esp_err_t hal_input_init(void);

/**
 * @brief Read the next decoded input event.
 *
 * @param event Destination for the decoded event.
 * @param timeout_ms Maximum time to wait in milliseconds.
 *
 * @return ESP_OK when an event is returned, ESP_ERR_TIMEOUT when no event is
 *         available before the timeout, or ESP_ERR_INVALID_STATE if the HAL
 *         has not been initialized.
 */
esp_err_t hal_input_read(input_event_t *event, uint32_t timeout_ms);
