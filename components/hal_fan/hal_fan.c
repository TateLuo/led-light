#include "hal_fan.h"

#include <stdbool.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "HAL_FAN";

#define HAL_FAN_SPEED_MODE LEDC_LOW_SPEED_MODE
#define HAL_FAN_TIMER      LEDC_TIMER_1
#define HAL_FAN_CHANNEL    LEDC_CHANNEL_2

static bool initialized;

static esp_err_t configure_safe_gpio_output(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOARD_GPIO_FAN_PWM,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure safe fan GPIO output: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = gpio_set_level(BOARD_GPIO_FAN_PWM, BOARD_FAN_PWM_OFF_LEVEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to turn off fan GPIO: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static esp_err_t update_pwm_duty(uint32_t duty)
{
    esp_err_t err = ledc_set_duty(HAL_FAN_SPEED_MODE, HAL_FAN_CHANNEL, duty);
    if (err != ESP_OK) {
        return err;
    }

    return ledc_update_duty(HAL_FAN_SPEED_MODE, HAL_FAN_CHANNEL);
}

static uint32_t percent_to_duty(uint8_t percent)
{
    return ((uint32_t)percent * HAL_FAN_DUTY_MAX +
            (HAL_FAN_PERCENT_MAX / 2)) / HAL_FAN_PERCENT_MAX;
}

esp_err_t hal_fan_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    esp_err_t err = configure_safe_gpio_output();
    if (err != ESP_OK) {
        return err;
    }

    const ledc_timer_config_t timer_config = {
        .speed_mode = HAL_FAN_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = HAL_FAN_TIMER,
        .freq_hz = HAL_FAN_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };

    err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure fan PWM timer: %s",
                 esp_err_to_name(err));
        return err;
    }

    const ledc_channel_config_t channel_config = {
        .gpio_num = BOARD_GPIO_FAN_PWM,
        .speed_mode = HAL_FAN_SPEED_MODE,
        .channel = HAL_FAN_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = HAL_FAN_TIMER,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags.output_invert = BOARD_FAN_PWM_OUTPUT_INVERTED,
    };

    err = ledc_channel_config(&channel_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure fan PWM channel: %s",
                 esp_err_to_name(err));
        (void)configure_safe_gpio_output();
        return err;
    }

    err = update_pwm_duty(0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set initial fan duty: %s",
                 esp_err_to_name(err));
        (void)configure_safe_gpio_output();
        return err;
    }

    initialized = true;
    ESP_LOGI(TAG, "Fan PWM initialized at %u Hz, %u-bit duty; output is off",
             HAL_FAN_PWM_FREQ_HZ, HAL_FAN_DUTY_RESOLUTION_BITS);
    return ESP_OK;
}

esp_err_t hal_fan_set_percent(uint8_t percent)
{
    if (!initialized) {
        ESP_LOGE(TAG, "Fan HAL is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (percent > HAL_FAN_PERCENT_MAX) {
        ESP_LOGE(TAG, "Fan duty percentage out of range: %u", percent);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = update_pwm_duty(percent_to_duty(percent));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update fan duty: %s", esp_err_to_name(err));
        (void)update_pwm_duty(0);
        return err;
    }

    ESP_LOGI(TAG, "Fan duty updated: %u%%", percent);
    return ESP_OK;
}

esp_err_t hal_fan_off(void)
{
    return hal_fan_set_percent(0);
}
