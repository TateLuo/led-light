#include "hal_power.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "HAL_POWER";

static bool initialized;
static uint64_t external_supply_ready_time_us;

static esp_err_t configure_hold_output(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOARD_GPIO_PWR_EN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure PWR_EN: %s", esp_err_to_name(err));
        return err;
    }

    err = gpio_set_level(BOARD_GPIO_PWR_EN, BOARD_PWR_EN_HOLD_LEVEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to assert PWR_EN: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static esp_err_t configure_power_key_input(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOARD_GPIO_EC_KEY_DET,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = BOARD_EC_KEY_PULL_UP,
        .pull_down_en = BOARD_EC_KEY_PULL_DOWN,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure EC_KEY_DET: %s", esp_err_to_name(err));
    }

    return err;
}

static esp_err_t configure_vbus_detect_input(void)
{
    if (BOARD_GPIO_USB_VBUS_DETECT == GPIO_NUM_NC) {
        return ESP_OK;
    }

    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOARD_GPIO_USB_VBUS_DETECT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = BOARD_USB_VBUS_DETECT_PULL_UP,
        .pull_down_en = BOARD_USB_VBUS_DETECT_PULL_DOWN,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure USB VBUS detect: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t hal_power_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    esp_err_t err = configure_hold_output();
    if (err != ESP_OK) {
        return err;
    }

    err = configure_power_key_input();
    if (err != ESP_OK) {
        return err;
    }

    err = configure_vbus_detect_input();
    if (err != ESP_OK) {
        return err;
    }

    if (BOARD_GPIO_USB_VBUS_DETECT == GPIO_NUM_NC) {
        external_supply_ready_time_us =
            esp_timer_get_time() + (uint64_t)BOARD_EXTERNAL_SUPPLY_READY_DELAY_MS
            * 1000ULL;
        ESP_LOGI(TAG, "No VBUS detect pin; delaying LED output %u ms",
                 (unsigned int)BOARD_EXTERNAL_SUPPLY_READY_DELAY_MS);
    } else {
        external_supply_ready_time_us = 0;
    }

    initialized = true;
    ESP_LOGI(TAG, "Power hold asserted");

#if !BOARD_EC_KEY_DET_CONFIRMED
    ESP_LOGW(TAG, "EC_KEY_DET handling is disabled until its pressed level is confirmed");
#endif

    return ESP_OK;
}

esp_err_t hal_power_hold(bool enable)
{
    if (!initialized) {
        ESP_LOGE(TAG, "Power HAL is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t level = enable ? BOARD_PWR_EN_HOLD_LEVEL : BOARD_PWR_EN_RELEASE_LEVEL;
    esp_err_t err = gpio_set_level(BOARD_GPIO_PWR_EN, level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set PWR_EN: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Power hold %s", enable ? "enabled" : "released");
    return ESP_OK;
}

bool hal_power_key_pressed(void)
{
#if BOARD_EC_KEY_DET_CONFIRMED
    if (!initialized) {
        return false;
    }

    return gpio_get_level(BOARD_GPIO_EC_KEY_DET) == BOARD_EC_KEY_PRESSED_LEVEL;
#else
    return false;
#endif
}

bool hal_power_external_supply_ready(void)
{
    if (BOARD_GPIO_USB_VBUS_DETECT == GPIO_NUM_NC) {
        if (external_supply_ready_time_us == 0) {
            return false;
        }
        return esp_timer_get_time() >= external_supply_ready_time_us;
    }

    if (!initialized) {
        return false;
    }

    return gpio_get_level(BOARD_GPIO_USB_VBUS_DETECT)
           == BOARD_USB_VBUS_DETECT_ACTIVE_LEVEL;
}

esp_err_t hal_power_shutdown(void)
{
    ESP_LOGW(TAG, "Releasing power hold");
    return hal_power_hold(false);
}
