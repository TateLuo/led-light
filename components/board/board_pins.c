#include "board.h"

#include <stdint.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "BOARD";

static esp_err_t configure_safe_power_output(gpio_num_t gpio_num, uint32_t off_level)
{
    (void)gpio_hold_dis(gpio_num);

    esp_err_t err = gpio_set_level(gpio_num, off_level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to preload GPIO%d off level: %s",
                 gpio_num, esp_err_to_name(err));
        return err;
    }

    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO%d safe output: %s",
                 gpio_num, esp_err_to_name(err));
        return err;
    }

    err = gpio_set_level(gpio_num, off_level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to drive GPIO%d off level: %s",
                 gpio_num, esp_err_to_name(err));
    }

    return err;
}

esp_err_t board_init(void)
{
    esp_err_t err = configure_safe_power_output(BOARD_GPIO_LED_C_PWM,
                                                BOARD_LED_PWM_OFF_LEVEL);
    if (err != ESP_OK) {
        return err;
    }

    err = configure_safe_power_output(BOARD_GPIO_LED_W_PWM,
                                      BOARD_LED_PWM_OFF_LEVEL);
    if (err != ESP_OK) {
        return err;
    }

    err = configure_safe_power_output(BOARD_GPIO_FAN_PWM,
                                      BOARD_FAN_PWM_OFF_LEVEL);
    if (err != ESP_OK) {
        return err;
    }

    err = configure_safe_power_output(BOARD_GPIO_LCD_BACKLIGHT,
                                      BOARD_LCD_BACKLIGHT_OFF_LEVEL);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Early power outputs forced off");
    return ESP_OK;
}
