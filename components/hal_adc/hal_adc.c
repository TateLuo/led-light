#include "hal_adc.h"

#include <stdbool.h>

#include "board_pins.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "HAL_ADC";

#define HAL_ADC_UNIT          ADC_UNIT_1
#define HAL_ADC_ATTENUATION   ADC_ATTEN_DB_12
#define HAL_ADC_BITWIDTH      ADC_BITWIDTH_12
#define HAL_ADC_RAW_MAX       4095

static adc_oneshot_unit_handle_t adc_handle;
static adc_cali_handle_t battery_cali_handle;
static adc_cali_handle_t ntc_cali_handle;
static bool initialized;

static esp_err_t validate_channel_mapping(const char *name, int gpio_num,
                                          adc_channel_t expected_channel)
{
    adc_unit_t unit;
    adc_channel_t channel;

    esp_err_t err = adc_oneshot_io_to_channel(gpio_num, &unit, &channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s GPIO%d is not a valid ADC input: %s",
                 name, gpio_num, esp_err_to_name(err));
        return err;
    }

    if (unit != HAL_ADC_UNIT || channel != expected_channel) {
        ESP_LOGE(TAG, "%s GPIO%d maps to ADC%d channel %d, expected ADC%d channel %d",
                 name, gpio_num, unit + 1, channel, HAL_ADC_UNIT + 1,
                 expected_channel);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static void try_create_calibration(const char *name, adc_channel_t channel,
                                   adc_cali_handle_t *cali_handle)
{
    const adc_cali_curve_fitting_config_t config = {
        .unit_id = HAL_ADC_UNIT,
        .chan = channel,
        .atten = HAL_ADC_ATTENUATION,
        .bitwidth = HAL_ADC_BITWIDTH,
    };

    esp_err_t err = adc_cali_create_scheme_curve_fitting(&config, cali_handle);
    if (err != ESP_OK) {
        *cali_handle = NULL;
        ESP_LOGW(TAG, "%s ADC calibration unavailable: %s; using approximate conversion",
                 name, esp_err_to_name(err));
    }
}

static void cleanup_adc(void)
{
    if (battery_cali_handle != NULL) {
        (void)adc_cali_delete_scheme_curve_fitting(battery_cali_handle);
        battery_cali_handle = NULL;
    }

    if (ntc_cali_handle != NULL) {
        (void)adc_cali_delete_scheme_curve_fitting(ntc_cali_handle);
        ntc_cali_handle = NULL;
    }

    if (adc_handle != NULL) {
        (void)adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
    }
}

static esp_err_t read_average_raw(const char *name, adc_channel_t channel,
                                  uint32_t *average_raw)
{
    uint32_t sample_sum = 0;

    for (uint32_t index = 0; index < HAL_ADC_SAMPLE_COUNT; ++index) {
        int raw;
        esp_err_t err = adc_oneshot_read(adc_handle, channel, &raw);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read %s ADC sample: %s",
                     name, esp_err_to_name(err));
            return err;
        }

        sample_sum += (uint32_t)raw;
    }

    *average_raw = (sample_sum + (HAL_ADC_SAMPLE_COUNT / 2)) /
                   HAL_ADC_SAMPLE_COUNT;
    return ESP_OK;
}

static uint32_t approximate_raw_to_mv(uint32_t raw)
{
    return (raw * HAL_ADC_FALLBACK_FULL_SCALE_MV + (HAL_ADC_RAW_MAX / 2)) /
           HAL_ADC_RAW_MAX;
}

static esp_err_t read_channel_mv(const char *name, adc_channel_t channel,
                                 adc_cali_handle_t cali_handle, uint32_t *mv)
{
    if (mv == NULL) {
        ESP_LOGE(TAG, "%s ADC output pointer is null", name);
        return ESP_ERR_INVALID_ARG;
    }

    if (!initialized) {
        ESP_LOGE(TAG, "ADC HAL is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t average_raw;
    esp_err_t err = read_average_raw(name, channel, &average_raw);
    if (err != ESP_OK) {
        return err;
    }

    if (cali_handle != NULL) {
        int calibrated_mv;
        err = adc_cali_raw_to_voltage(cali_handle, (int)average_raw,
                                      &calibrated_mv);
        if (err == ESP_OK) {
            *mv = (uint32_t)calibrated_mv;
            return ESP_OK;
        }

        ESP_LOGW(TAG, "%s ADC calibration failed: %s; using approximate conversion",
                 name, esp_err_to_name(err));
    }

    *mv = approximate_raw_to_mv(average_raw);
    return ESP_OK;
}

esp_err_t hal_adc_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    esp_err_t err = validate_channel_mapping("NTC", BOARD_GPIO_NTC_ADC,
                                             BOARD_ADC_NTC_CHANNEL);
    if (err != ESP_OK) {
        return err;
    }

    err = validate_channel_mapping("battery", BOARD_GPIO_BAT_ADC,
                                   BOARD_ADC_BAT_CHANNEL);
    if (err != ESP_OK) {
        return err;
    }

    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = HAL_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    err = adc_oneshot_new_unit(&unit_config, &adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ADC1 oneshot unit: %s",
                 esp_err_to_name(err));
        return err;
    }

    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = HAL_ADC_ATTENUATION,
        .bitwidth = HAL_ADC_BITWIDTH,
    };

    err = adc_oneshot_config_channel(adc_handle, BOARD_ADC_NTC_CHANNEL,
                                     &channel_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure NTC ADC channel: %s",
                 esp_err_to_name(err));
        cleanup_adc();
        return err;
    }

    err = adc_oneshot_config_channel(adc_handle, BOARD_ADC_BAT_CHANNEL,
                                     &channel_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure battery ADC channel: %s",
                 esp_err_to_name(err));
        cleanup_adc();
        return err;
    }

    try_create_calibration("NTC", BOARD_ADC_NTC_CHANNEL, &ntc_cali_handle);
    try_create_calibration("battery", BOARD_ADC_BAT_CHANNEL,
                           &battery_cali_handle);

    initialized = true;
    ESP_LOGI(TAG, "ADC1 oneshot initialized with %u-sample averaging",
             HAL_ADC_SAMPLE_COUNT);

    if (ntc_cali_handle != NULL && battery_cali_handle != NULL) {
        ESP_LOGI(TAG, "ADC curve-fitting calibration is active");
    } else {
        ESP_LOGW(TAG, "One or more ADC channels use approximate millivolt conversion");
    }

    return ESP_OK;
}

esp_err_t hal_adc_read_battery_mv(uint32_t *mv)
{
    return read_channel_mv("battery", BOARD_ADC_BAT_CHANNEL,
                           battery_cali_handle, mv);
}

esp_err_t hal_adc_read_ntc_mv(uint32_t *mv)
{
    return read_channel_mv("NTC", BOARD_ADC_NTC_CHANNEL, ntc_cali_handle, mv);
}
