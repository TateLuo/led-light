#include "hal_input.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal_power.h"

static const char *TAG = "HAL_INPUT";

#define INPUT_TASK_STACK_SIZE         3072
#define INPUT_TASK_PRIORITY           7
#define INPUT_SCAN_PERIOD_MS          10
#define INPUT_BUTTON_DEBOUNCE_MS      30
#define INPUT_BUTTON_LONG_PRESS_MS    2000
#define INPUT_ENCODER_EDGE_GUARD_US   500
#define INPUT_ENCODER_DETENT_HIGH     0x03
#define INPUT_ENCODER_DETENT_LOW      0x00
#define INPUT_ENCODER_STEPS_PER_DETENT 2
#define INPUT_ENCODER_LOG_DIAGNOSTICS 0
#define INPUT_RAW_QUEUE_LENGTH        64
#define INPUT_EVENT_QUEUE_LENGTH      32

typedef enum {
    ENCODER_SW1 = 0,
    ENCODER_SW2,
} encoder_id_t;

typedef struct {
    encoder_id_t encoder;
    uint8_t state;
    uint64_t timestamp_us;
} encoder_edge_t;

typedef struct {
    uint8_t previous_state;
    int8_t quarter_steps;
    bool reversed;
    uint64_t last_edge_us;
    uint32_t invalid_transitions;
    input_event_type_t clockwise_event;
    input_event_type_t counterclockwise_event;
} encoder_state_t;

typedef struct {
    bool initialized;
    bool release_seen;
    bool stable_pressed;
    bool candidate_pressed;
    uint32_t candidate_since_ms;
    uint32_t pressed_since_ms;
    bool long_press_emitted;
    input_event_type_t short_press_event;
    input_event_type_t long_press_event;
} button_state_t;

static QueueHandle_t encoder_edge_queue;
static QueueHandle_t input_event_queue;
static bool initialized;
static bool isr_service_owned;
static bool sw1_a_handler_added;
static bool sw1_b_handler_added;
static bool sw2_a_handler_added;
static bool sw2_b_handler_added;
static volatile uint32_t dropped_encoder_edges;

static encoder_state_t sw1_encoder = {
    .reversed = BOARD_ENCODER_SW1_REVERSED != 0,
    .clockwise_event = INPUT_EVENT_SW1_CW,
    .counterclockwise_event = INPUT_EVENT_SW1_CCW,
};

static encoder_state_t sw2_encoder = {
    .reversed = BOARD_ENCODER_SW2_REVERSED != 0,
    .clockwise_event = INPUT_EVENT_SW2_CW,
    .counterclockwise_event = INPUT_EVENT_SW2_CCW,
};

static button_state_t sw2_button = {
    .short_press_event = INPUT_EVENT_SW2_SHORT_PRESS,
    .long_press_event = INPUT_EVENT_SW2_LONG_PRESS,
};

static button_state_t power_button = {
    .short_press_event = INPUT_EVENT_POWER_SHORT_PRESS,
    .long_press_event = INPUT_EVENT_POWER_LONG_PRESS,
};

static uint32_t input_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint64_t input_now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

static bool elapsed_at_least(uint32_t now_ms, uint32_t since_ms, uint32_t duration_ms)
{
    return (now_ms - since_ms) >= duration_ms;
}

static uint8_t read_encoder_state(encoder_id_t encoder)
{
    gpio_num_t pin_a = encoder == ENCODER_SW1 ? BOARD_GPIO_SW1_A : BOARD_GPIO_SW2_A;
    gpio_num_t pin_b = encoder == ENCODER_SW1 ? BOARD_GPIO_SW1_B : BOARD_GPIO_SW2_B;

    return (uint8_t)((gpio_get_level(pin_a) << 1) | gpio_get_level(pin_b));
}

static void encoder_gpio_isr(void *arg)
{
    const encoder_id_t encoder = (encoder_id_t)(uintptr_t)arg;
    const encoder_edge_t edge = {
        .encoder = encoder,
        .state = read_encoder_state(encoder),
        .timestamp_us = input_now_us(),
    };
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (xQueueSendFromISR(encoder_edge_queue, &edge, &higher_priority_task_woken) != pdTRUE) {
        dropped_encoder_edges++;
    }

    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void emit_event(input_event_type_t type, int32_t delta, uint32_t timestamp_ms)
{
    const input_event_t event = {
        .type = type,
        .delta = delta,
        .timestamp_ms = timestamp_ms,
    };

    if (xQueueSend(input_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Input event queue full; dropping event type %d", type);
    }
}

static void reset_encoder_to_state(encoder_state_t *encoder, uint8_t state)
{
    encoder->previous_state = state;
    encoder->quarter_steps = 0;
}

static void emit_encoder_detent(const encoder_state_t *encoder,
                                int8_t direction, uint32_t timestamp_ms)
{
    if (direction > 0) {
        emit_event(encoder->reversed ? encoder->counterclockwise_event :
                   encoder->clockwise_event,
                   encoder->reversed ? -1 : 1,
                   timestamp_ms);
    } else if (direction < 0) {
        emit_event(encoder->reversed ? encoder->clockwise_event :
                   encoder->counterclockwise_event,
                   encoder->reversed ? 1 : -1,
                   timestamp_ms);
    }
}

static bool encoder_state_is_detent(uint8_t state)
{
    return state == INPUT_ENCODER_DETENT_HIGH
           || state == INPUT_ENCODER_DETENT_LOW;
}

static void process_encoder_edge(const encoder_edge_t *edge)
{
    static const int8_t transition_steps[16] = {
         0, -1,  1,  0,
         1,  0,  0, -1,
        -1,  0,  0,  1,
         0,  1, -1,  0,
    };

    encoder_state_t *encoder = edge->encoder == ENCODER_SW1 ? &sw1_encoder : &sw2_encoder;
    const uint8_t previous_state = encoder->previous_state;
    const uint8_t current_state = edge->state;
    const uint32_t timestamp_ms = (uint32_t)(edge->timestamp_us / 1000ULL);

    if (current_state == previous_state) {
        return;
    }

    if (encoder->last_edge_us != 0
        && edge->timestamp_us - encoder->last_edge_us
           < INPUT_ENCODER_EDGE_GUARD_US) {
        return;
    }
    encoder->last_edge_us = edge->timestamp_us;

    if ((previous_state ^ current_state) == 0x03) {
        encoder->invalid_transitions++;
        reset_encoder_to_state(encoder, current_state);
        return;
    } else {
        const int8_t step =
            transition_steps[(previous_state << 2) | current_state];
        if (step == 0) {
            encoder->invalid_transitions++;
            reset_encoder_to_state(encoder, current_state);
            return;
        }
        encoder->quarter_steps += step;
    }
    encoder->previous_state = current_state;

    if (!encoder_state_is_detent(current_state)) {
        if (encoder->quarter_steps > INPUT_ENCODER_STEPS_PER_DETENT
            || encoder->quarter_steps < -INPUT_ENCODER_STEPS_PER_DETENT) {
            encoder->invalid_transitions++;
            reset_encoder_to_state(encoder, current_state);
        }
        return;
    }

    if (encoder->quarter_steps >= INPUT_ENCODER_STEPS_PER_DETENT) {
        emit_encoder_detent(encoder, 1, timestamp_ms);
    } else if (encoder->quarter_steps <= -INPUT_ENCODER_STEPS_PER_DETENT) {
        emit_encoder_detent(encoder, -1, timestamp_ms);
    } else if (encoder->quarter_steps != 0) {
        encoder->invalid_transitions++;
    }
    encoder->quarter_steps = 0;
}

static bool read_sw2_button_pressed(void)
{
#if BOARD_SW2_DOWN_CONFIRMED
    return gpio_get_level(BOARD_GPIO_SW2_DOWN) == BOARD_SW2_DOWN_PRESSED_LEVEL;
#else
    return false;
#endif
}

static void process_button(button_state_t *button, bool raw_pressed, uint32_t timestamp_ms)
{
    if (!button->initialized) {
        button->initialized = true;
        button->release_seen = !raw_pressed;
        button->stable_pressed = raw_pressed;
        button->candidate_pressed = raw_pressed;
        button->candidate_since_ms = timestamp_ms;
        button->pressed_since_ms = timestamp_ms;
        button->long_press_emitted = false;
        return;
    }

    if (raw_pressed != button->candidate_pressed) {
        button->candidate_pressed = raw_pressed;
        button->candidate_since_ms = timestamp_ms;
    }

    if (button->candidate_pressed != button->stable_pressed
        && elapsed_at_least(timestamp_ms, button->candidate_since_ms,
                            INPUT_BUTTON_DEBOUNCE_MS)) {
        button->stable_pressed = button->candidate_pressed;

        if (button->stable_pressed) {
            button->pressed_since_ms = timestamp_ms;
            button->long_press_emitted = false;
        } else {
            if (button->release_seen && !button->long_press_emitted) {
                emit_event(button->short_press_event, 0, timestamp_ms);
            }
            button->release_seen = true;
        }
    }

    if (button->stable_pressed && button->release_seen
        && !button->long_press_emitted
        && elapsed_at_least(timestamp_ms, button->pressed_since_ms,
                            INPUT_BUTTON_LONG_PRESS_MS)) {
        emit_event(button->long_press_event, 0, timestamp_ms);
        button->long_press_emitted = true;
    }
}

static void input_task(void *arg)
{
    (void)arg;

    uint32_t reported_dropped_edges = 0;
#if INPUT_ENCODER_LOG_DIAGNOSTICS
    uint32_t reported_sw1_invalid = 0;
    uint32_t reported_sw2_invalid = 0;
#endif

    while (true) {
        encoder_edge_t edge;
        if (xQueueReceive(encoder_edge_queue, &edge,
                          pdMS_TO_TICKS(INPUT_SCAN_PERIOD_MS)) == pdTRUE) {
            process_encoder_edge(&edge);

            for (size_t index = 1; index < INPUT_RAW_QUEUE_LENGTH; index++) {
                if (xQueueReceive(encoder_edge_queue, &edge, 0) != pdTRUE) {
                    break;
                }
                process_encoder_edge(&edge);
            }
        }

        const uint32_t timestamp_ms = input_now_ms();
        process_button(&sw2_button, read_sw2_button_pressed(), timestamp_ms);
        process_button(&power_button, hal_power_key_pressed(), timestamp_ms);

        if (reported_dropped_edges != dropped_encoder_edges) {
            reported_dropped_edges = dropped_encoder_edges;
            ESP_LOGW(TAG, "Dropped encoder edges: %lu",
                     (unsigned long)reported_dropped_edges);
        }

#if INPUT_ENCODER_LOG_DIAGNOSTICS
        if (reported_sw1_invalid != sw1_encoder.invalid_transitions) {
            reported_sw1_invalid = sw1_encoder.invalid_transitions;
            ESP_LOGW(TAG, "SW1 invalid encoder transitions: %lu",
                     (unsigned long)reported_sw1_invalid);
        }
        if (reported_sw2_invalid != sw2_encoder.invalid_transitions) {
            reported_sw2_invalid = sw2_encoder.invalid_transitions;
            ESP_LOGW(TAG, "SW2 invalid encoder transitions: %lu",
                     (unsigned long)reported_sw2_invalid);
        }
#endif
    }
}

static esp_err_t configure_encoder_inputs(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << BOARD_GPIO_SW1_A) | (1ULL << BOARD_GPIO_SW1_B)
                        | (1ULL << BOARD_GPIO_SW2_A) | (1ULL << BOARD_GPIO_SW2_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = BOARD_ENCODER_PULL_UP,
        .pull_down_en = BOARD_ENCODER_PULL_DOWN,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure encoder inputs: %s", esp_err_to_name(err));
    }

    return err;
}

static esp_err_t configure_sw2_button_input(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOARD_GPIO_SW2_DOWN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = BOARD_SW2_DOWN_PULL_UP,
        .pull_down_en = BOARD_SW2_DOWN_PULL_DOWN,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure SW2_DOWN: %s", esp_err_to_name(err));
    }

    return err;
}

static void cleanup_isr_resources(void)
{
    if (sw1_a_handler_added) {
        gpio_isr_handler_remove(BOARD_GPIO_SW1_A);
        sw1_a_handler_added = false;
    }
    if (sw1_b_handler_added) {
        gpio_isr_handler_remove(BOARD_GPIO_SW1_B);
        sw1_b_handler_added = false;
    }
    if (sw2_a_handler_added) {
        gpio_isr_handler_remove(BOARD_GPIO_SW2_A);
        sw2_a_handler_added = false;
    }
    if (sw2_b_handler_added) {
        gpio_isr_handler_remove(BOARD_GPIO_SW2_B);
        sw2_b_handler_added = false;
    }
    if (isr_service_owned) {
        gpio_uninstall_isr_service();
        isr_service_owned = false;
    }
}

static esp_err_t add_encoder_handler(gpio_num_t pin, encoder_id_t encoder, bool *added)
{
    esp_err_t err = gpio_isr_handler_add(pin, encoder_gpio_isr, (void *)(uintptr_t)encoder);
    if (err == ESP_OK) {
        *added = true;
    } else {
        ESP_LOGE(TAG, "Failed to add ISR handler for GPIO%d: %s", pin, esp_err_to_name(err));
    }
    return err;
}

esp_err_t hal_input_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    esp_err_t err = ESP_OK;

    encoder_edge_queue = xQueueCreate(INPUT_RAW_QUEUE_LENGTH, sizeof(encoder_edge_t));
    input_event_queue = xQueueCreate(INPUT_EVENT_QUEUE_LENGTH, sizeof(input_event_t));
    if (encoder_edge_queue == NULL || input_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to allocate input queues");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    err = configure_encoder_inputs();
    if (err != ESP_OK) {
        goto fail;
    }

    err = configure_sw2_button_input();
    if (err != ESP_OK) {
        goto fail;
    }

    err = gpio_install_isr_service(0);
    if (err == ESP_OK) {
        isr_service_owned = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(err));
        goto fail;
    }
    err = ESP_OK;

    err = add_encoder_handler(BOARD_GPIO_SW1_A, ENCODER_SW1, &sw1_a_handler_added);
    if (err != ESP_OK) {
        goto fail;
    }
    err = add_encoder_handler(BOARD_GPIO_SW1_B, ENCODER_SW1, &sw1_b_handler_added);
    if (err != ESP_OK) {
        goto fail;
    }
    err = add_encoder_handler(BOARD_GPIO_SW2_A, ENCODER_SW2, &sw2_a_handler_added);
    if (err != ESP_OK) {
        goto fail;
    }
    err = add_encoder_handler(BOARD_GPIO_SW2_B, ENCODER_SW2, &sw2_b_handler_added);
    if (err != ESP_OK) {
        goto fail;
    }

    sw1_encoder.previous_state = read_encoder_state(ENCODER_SW1);
    sw2_encoder.previous_state = read_encoder_state(ENCODER_SW2);

    BaseType_t task_created = xTaskCreate(input_task, "hal_input",
                                          INPUT_TASK_STACK_SIZE, NULL,
                                          INPUT_TASK_PRIORITY, NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create input task");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    initialized = true;
    ESP_LOGI(TAG, "Input event HAL initialized");

#if !BOARD_SW2_DOWN_CONFIRMED
    ESP_LOGW(TAG, "SW2_DOWN handling is disabled until its pressed level is confirmed");
#endif

    return ESP_OK;

fail:
    cleanup_isr_resources();
    if (encoder_edge_queue != NULL) {
        vQueueDelete(encoder_edge_queue);
        encoder_edge_queue = NULL;
    }
    if (input_event_queue != NULL) {
        vQueueDelete(input_event_queue);
        input_event_queue = NULL;
    }
    return err;
}

esp_err_t hal_input_read(input_event_t *event, uint32_t timeout_ms)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueReceive(input_event_queue, event, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        event->type = INPUT_EVENT_NONE;
        event->delta = 0;
        event->timestamp_ms = input_now_ms();
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
