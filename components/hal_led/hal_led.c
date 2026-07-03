#include "hal_led.h"

#include <stdbool.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "HAL_LED";

#define HAL_LED_SPEED_MODE   LEDC_LOW_SPEED_MODE
#define HAL_LED_TIMER        LEDC_TIMER_0
#define HAL_LED_COLD_CHANNEL LEDC_CHANNEL_0
#define HAL_LED_WARM_CHANNEL LEDC_CHANNEL_1

static bool initialized;
static SemaphoreHandle_t led_mutex;
static uint16_t current_cold_duty;
static uint16_t current_warm_duty;

static esp_err_t configure_safe_gpio_outputs(void)
{
    (void)gpio_hold_dis(BOARD_GPIO_LED_C_PWM);
    (void)gpio_hold_dis(BOARD_GPIO_LED_W_PWM);

    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << BOARD_GPIO_LED_C_PWM) |
                        (1ULL << BOARD_GPIO_LED_W_PWM),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure safe LED GPIO outputs: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = gpio_set_level(BOARD_GPIO_LED_C_PWM, BOARD_LED_PWM_OFF_LEVEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to turn off cold LED GPIO: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = gpio_set_level(BOARD_GPIO_LED_W_PWM, BOARD_LED_PWM_OFF_LEVEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to turn off warm LED GPIO: %s",
                 esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static void capture_first_error(esp_err_t err, esp_err_t *first_error)
{
    if (err != ESP_OK && *first_error == ESP_OK) {
        *first_error = err;
    }
}

static esp_err_t configure_ledc_channel(int gpio_num, ledc_channel_t channel)
{
    const ledc_channel_config_t config = {
        .gpio_num = gpio_num,
        .speed_mode = HAL_LED_SPEED_MODE,
        .channel = channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = HAL_LED_TIMER,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags.output_invert = BOARD_LED_PWM_OUTPUT_INVERTED,
    };

    return ledc_channel_config(&config);
}

static esp_err_t update_ledc_duty(ledc_channel_t channel, uint16_t duty)
{
    esp_err_t err = ledc_set_duty(HAL_LED_SPEED_MODE, channel, duty);
    if (err != ESP_OK) {
        return err;
    }

    return ledc_update_duty(HAL_LED_SPEED_MODE, channel);
}

static void best_effort_pwm_off(void)
{
    (void)update_ledc_duty(HAL_LED_COLD_CHANNEL, 0);
    (void)update_ledc_duty(HAL_LED_WARM_CHANNEL, 0);
}

static esp_err_t set_pwm_duties(uint16_t cold_duty, uint16_t warm_duty)
{
    esp_err_t err = update_ledc_duty(HAL_LED_COLD_CHANNEL, cold_duty);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update cold LED duty: %s",
                 esp_err_to_name(err));
        best_effort_pwm_off();
        return err;
    }

    err = update_ledc_duty(HAL_LED_WARM_CHANNEL, warm_duty);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update warm LED duty: %s",
                 esp_err_to_name(err));
        best_effort_pwm_off();
        return err;
    }

    return ESP_OK;
}

esp_err_t hal_led_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    esp_err_t err = configure_safe_gpio_outputs();
    if (err != ESP_OK) {
        return err;
    }

    const ledc_timer_config_t timer_config = {
        .speed_mode = HAL_LED_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_12_BIT,
        .timer_num = HAL_LED_TIMER,
        .freq_hz = HAL_LED_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };

    err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LED PWM timer: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = configure_ledc_channel(BOARD_GPIO_LED_C_PWM, HAL_LED_COLD_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure cold LED PWM: %s",
                 esp_err_to_name(err));
        (void)configure_safe_gpio_outputs();
        return err;
    }

    err = configure_ledc_channel(BOARD_GPIO_LED_W_PWM, HAL_LED_WARM_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure warm LED PWM: %s",
                 esp_err_to_name(err));
        (void)configure_safe_gpio_outputs();
        return err;
    }

    err = set_pwm_duties(0, 0);
    if (err != ESP_OK) {
        (void)configure_safe_gpio_outputs();
        return err;
    }

    led_mutex = xSemaphoreCreateMutex();
    if (led_mutex == NULL) {
        (void)configure_safe_gpio_outputs();
        return ESP_ERR_NO_MEM;
    }

    current_cold_duty = 0;
    current_warm_duty = 0;
    initialized = true;
    ESP_LOGI(TAG, "LED PWM initialized at %u Hz, %u-bit duty; outputs are off",
             HAL_LED_PWM_FREQ_HZ, HAL_LED_DUTY_RESOLUTION_BITS);
    return ESP_OK;
}

esp_err_t hal_led_set_cw(uint16_t cold_duty, uint16_t warm_duty)
{
    if (!initialized || led_mutex == NULL) {
        ESP_LOGE(TAG, "LED HAL is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (cold_duty > HAL_LED_DUTY_MAX || warm_duty > HAL_LED_DUTY_MAX) {
        ESP_LOGE(TAG, "LED duty out of range: cold=%u warm=%u",
                 cold_duty, warm_duty);
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(led_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (cold_duty == current_cold_duty && warm_duty == current_warm_duty) {
        xSemaphoreGive(led_mutex);
        return ESP_OK;
    }

    esp_err_t err = set_pwm_duties(cold_duty, warm_duty);
    if (err == ESP_OK) {
        current_cold_duty = cold_duty;
        current_warm_duty = warm_duty;
        ESP_LOGI(TAG, "LED duty updated: cold=%u warm=%u",
                 cold_duty, warm_duty);
    } else {
        current_cold_duty = 0;
        current_warm_duty = 0;
    }

    xSemaphoreGive(led_mutex);
    return err;
}

esp_err_t hal_led_get_duty_snapshot(hal_led_duty_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!initialized || led_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(led_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    snapshot->cold_duty = current_cold_duty;
    snapshot->warm_duty = current_warm_duty;
    xSemaphoreGive(led_mutex);
    return ESP_OK;
}

esp_err_t hal_led_off(void)
{
    return hal_led_set_cw(0, 0);
}

esp_err_t hal_led_force_safe_off(bool hold_level)
{
    esp_err_t first_error = ESP_OK;

    if (led_mutex != NULL) {
        if (xSemaphoreTake(led_mutex, portMAX_DELAY) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
    }

    if (initialized) {
        capture_first_error(update_ledc_duty(HAL_LED_COLD_CHANNEL, 0),
                            &first_error);
        capture_first_error(update_ledc_duty(HAL_LED_WARM_CHANNEL, 0),
                            &first_error);
        capture_first_error(ledc_stop(HAL_LED_SPEED_MODE,
                                      HAL_LED_COLD_CHANNEL,
                                      BOARD_LED_PWM_OFF_LEVEL),
                            &first_error);
        capture_first_error(ledc_stop(HAL_LED_SPEED_MODE,
                                      HAL_LED_WARM_CHANNEL,
                                      BOARD_LED_PWM_OFF_LEVEL),
                            &first_error);
    }

    capture_first_error(configure_safe_gpio_outputs(), &first_error);
    if (hold_level) {
        capture_first_error(gpio_hold_en(BOARD_GPIO_LED_C_PWM), &first_error);
        capture_first_error(gpio_hold_en(BOARD_GPIO_LED_W_PWM), &first_error);
    }

    current_cold_duty = 0;
    current_warm_duty = 0;

    if (led_mutex != NULL) {
        xSemaphoreGive(led_mutex);
    }

    if (first_error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to force LED GPIOs safe off: %s",
                 esp_err_to_name(first_error));
    } else {
        ESP_LOGI(TAG, "LED GPIOs forced safe off%s",
                 hold_level ? " with hold" : "");
    }
    return first_error;
}
