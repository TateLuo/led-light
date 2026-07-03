#include "app_sensor.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "app_state.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal_adc.h"
#include "hal_led.h"

static const char *TAG = "APP_SENSOR";

#define SENSOR_TASK_STACK_SIZE         3072
#define SENSOR_TASK_PRIORITY           4
#define SENSOR_FILTER_ALPHA            0.25f
#define BATTERY_DIVIDER_SCALE          4.0f
#define NTC_PULLUP_OHM                 10000.0f
#define NTC_R0_OHM                     10000.0f
#define NTC_BETA                       3950.0f
#define NTC_T0_K                       298.15f
#define NTC_VREF_MV                    3300.0f
#define NTC_INVALID_LOW_MV             50
#define NTC_INVALID_HIGH_MV            3250

#ifndef APP_SENSOR_BATTERY_CAPACITY_AH
#define APP_SENSOR_BATTERY_CAPACITY_AH 2.0f
#endif

#define BATTERY_LED_W_MAX_POWER_W      24.5f
#define BATTERY_LED_C_MAX_POWER_W      24.5f
#define BATTERY_BOOST_EFFICIENCY       0.88f
#define BATTERY_AUX_FAN_OFF_POWER_W    0.6f
#define BATTERY_AUX_FAN_ON_POWER_W     1.5f
#define BATTERY_R0_OHM                 0.06f
#define BATTERY_R1_OHM                 0.03f
#define BATTERY_SLOW_TAU_S             45.0f
#define BATTERY_LOAD_FREEZE_S          8.0f
#define BATTERY_DUTY_STEP_FREEZE       0.05f
#define BATTERY_CURRENT_STEP_FREEZE_A   0.3f
#define BATTERY_CAL_K_LOW_LOAD         0.02f
#define BATTERY_CAL_K_NORMAL_LOAD      0.002f
#define BATTERY_LOW_LOAD_DUTY_SUM      0.05f
#define BATTERY_DISPLAY_FALL_RATE_PPS  0.10f
#define BATTERY_DISPLAY_RISE_RATE_PPS  0.02f
#define BATTERY_MIN_VALID_VOLTAGE_V    1.0f

typedef struct {
    float voltage_v;
    float percent;
} battery_lut_t;

static const battery_lut_t battery_ocv_lut[] = {
    {8.40f, 100.0f},
    {8.30f, 95.0f},
    {8.20f, 90.0f},
    {8.10f, 82.0f},
    {8.00f, 75.0f},
    {7.90f, 68.0f},
    {7.80f, 60.0f},
    {7.70f, 52.0f},
    {7.60f, 45.0f},
    {7.50f, 38.0f},
    {7.40f, 32.0f},
    {7.30f, 25.0f},
    {7.20f, 18.0f},
    {7.10f, 12.0f},
    {7.00f, 8.0f},
    {6.80f, 4.0f},
    {6.60f, 2.0f},
    {6.40f, 0.0f},
};

#define BATTERY_OCV_LUT_SIZE \
    (sizeof(battery_ocv_lut) / sizeof(battery_ocv_lut[0]))

static SemaphoreHandle_t snapshot_mutex;
static app_sensor_snapshot_t latest_snapshot;
static float filtered_battery_voltage_v;
static float filtered_ntc_temp_c;
static bool battery_filter_initialized;
static bool ntc_filter_initialized;
static bool battery_fault_logged;
static bool ntc_fault_logged;
static bool initialized;
static bool soc_initialized;
static float soc_est_percent;
static float soc_display_percent;
static float battery_slow_polarization_v;
static float battery_freeze_remaining_s;
static float previous_led_duty_sum;
static float previous_battery_current_a;
static bool previous_fan_on;
static bool previous_load_initialized;
static int64_t previous_battery_sample_us;

static float filter_value(float previous, float current, bool *filter_initialized)
{
    if (!*filter_initialized) {
        *filter_initialized = true;
        return current;
    }

    return previous + SENSOR_FILTER_ALPHA * (current - previous);
}

static float clamp_float(float value, float minimum, float maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static uint8_t percent_to_u8(float percent)
{
    const float clamped = clamp_float(percent, 0.0f, 100.0f);
    return (uint8_t)(clamped + 0.5f);
}

static float interpolate_percent(const battery_lut_t *upper,
                                 const battery_lut_t *lower,
                                 float voltage_v)
{
    const float voltage_range = upper->voltage_v - lower->voltage_v;
    const float percent_range = upper->percent - lower->percent;
    const float ratio = (voltage_v - lower->voltage_v) / voltage_range;
    return lower->percent + ratio * percent_range;
}

static float battery_ocv_voltage_to_soc(float voltage_v)
{
    if (!isfinite(voltage_v) ||
        voltage_v <= battery_ocv_lut[BATTERY_OCV_LUT_SIZE - 1].voltage_v) {
        return 0.0f;
    }

    if (voltage_v >= battery_ocv_lut[0].voltage_v) {
        return 100.0f;
    }

    for (size_t index = 1; index < BATTERY_OCV_LUT_SIZE; ++index) {
        if (voltage_v >= battery_ocv_lut[index].voltage_v) {
            return interpolate_percent(&battery_ocv_lut[index - 1],
                                       &battery_ocv_lut[index], voltage_v);
        }
    }

    return 0.0f;
}

float app_sensor_battery_adc_mv_to_pack_voltage_v(uint32_t adc_mv)
{
    return ((float)adc_mv / 1000.0f) * BATTERY_DIVIDER_SCALE;
}

uint8_t app_sensor_battery_voltage_to_percent(float voltage_v)
{
    return percent_to_u8(battery_ocv_voltage_to_soc(voltage_v));
}

esp_err_t app_sensor_ntc_mv_to_temp_c(uint32_t ntc_mv, float *temp_c)
{
    if (temp_c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ntc_mv < NTC_INVALID_LOW_MV || ntc_mv > NTC_INVALID_HIGH_MV) {
        *temp_c = NAN;
        return ESP_ERR_INVALID_ARG;
    }

    const float voltage_mv = (float)ntc_mv;
    const float resistance_ohm = NTC_PULLUP_OHM * voltage_mv /
                                 (NTC_VREF_MV - voltage_mv);
    const float inverse_temp_k = (1.0f / NTC_T0_K) +
                                 (logf(resistance_ohm / NTC_R0_OHM) /
                                  NTC_BETA);
    const float converted_temp_c = (1.0f / inverse_temp_k) - 273.15f;

    if (!isfinite(converted_temp_c)) {
        *temp_c = NAN;
        return ESP_FAIL;
    }

    *temp_c = converted_temp_c;
    return ESP_OK;
}

static float battery_sample_dt_s(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (previous_battery_sample_us == 0) {
        previous_battery_sample_us = now_us;
        return (float)APP_SENSOR_SAMPLE_PERIOD_MS / 1000.0f;
    }

    const float dt_s =
        (float)(now_us - previous_battery_sample_us) / 1000000.0f;
    previous_battery_sample_us = now_us;
    return clamp_float(dt_s, 0.001f, 10.0f);
}

static void read_load_state(hal_led_duty_snapshot_t *led_duty, bool *fan_on)
{
    led_duty->cold_duty = 0;
    led_duty->warm_duty = 0;
    *fan_on = false;

    if (hal_led_get_duty_snapshot(led_duty) != ESP_OK) {
        return;
    }

    app_state_t state;
    if (app_state_get(&state) == ESP_OK) {
        *fan_on = state.fan_percent > 0;
    }
}

static float estimate_battery_current_a(float vbat_filtered,
                                        const hal_led_duty_snapshot_t *led_duty,
                                        bool fan_on,
                                        float *led_duty_sum)
{
    const float warm_duty =
        (float)led_duty->warm_duty / (float)HAL_LED_DUTY_MAX;
    const float cold_duty =
        (float)led_duty->cold_duty / (float)HAL_LED_DUTY_MAX;
    *led_duty_sum = clamp_float(warm_duty + cold_duty, 0.0f, 2.0f);

    const float led_power_w = BATTERY_LED_W_MAX_POWER_W * warm_duty +
                              BATTERY_LED_C_MAX_POWER_W * cold_duty;
    const float aux_power_w = fan_on ? BATTERY_AUX_FAN_ON_POWER_W :
                              BATTERY_AUX_FAN_OFF_POWER_W;
    const float battery_power_w = led_power_w / BATTERY_BOOST_EFFICIENCY +
                                  aux_power_w;

    if (!isfinite(vbat_filtered) || vbat_filtered < BATTERY_MIN_VALID_VOLTAGE_V) {
        return 0.0f;
    }

    return battery_power_w / vbat_filtered;
}

static float battery_capacity_ah(void)
{
    if (APP_SENSOR_BATTERY_CAPACITY_AH > 0.0f) {
        return APP_SENSOR_BATTERY_CAPACITY_AH;
    }
    return 2.0f;
}

static bool load_changed(float led_duty_sum, float battery_current_a,
                         bool fan_on)
{
    if (!previous_load_initialized) {
        previous_load_initialized = true;
        previous_led_duty_sum = led_duty_sum;
        previous_battery_current_a = battery_current_a;
        previous_fan_on = fan_on;
        return false;
    }

    const bool changed =
        fabsf(led_duty_sum - previous_led_duty_sum) >
        BATTERY_DUTY_STEP_FREEZE
        || fabsf(battery_current_a - previous_battery_current_a) >
           BATTERY_CURRENT_STEP_FREEZE_A
        || fan_on != previous_fan_on;

    previous_led_duty_sum = led_duty_sum;
    previous_battery_current_a = battery_current_a;
    previous_fan_on = fan_on;
    return changed;
}

static uint8_t estimate_battery_percent(float vbat_filtered,
                                        const hal_led_duty_snapshot_t *led_duty,
                                        bool fan_on,
                                        float dt_s)
{
    float led_duty_sum = 0.0f;
    const float battery_current_a =
        estimate_battery_current_a(vbat_filtered, led_duty, fan_on,
                                   &led_duty_sum);

    if (load_changed(led_duty_sum, battery_current_a, fan_on)) {
        battery_freeze_remaining_s = BATTERY_LOAD_FREEZE_S;
    } else if (battery_freeze_remaining_s > 0.0f) {
        battery_freeze_remaining_s =
            fmaxf(0.0f, battery_freeze_remaining_s - dt_s);
    }

    const float slow_target_v = battery_current_a * BATTERY_R1_OHM;
    const float slow_alpha = clamp_float(dt_s / BATTERY_SLOW_TAU_S,
                                         0.0f, 1.0f);
    battery_slow_polarization_v +=
        (slow_target_v - battery_slow_polarization_v) * slow_alpha;

    const float vocv_est = vbat_filtered +
                           battery_current_a * BATTERY_R0_OHM +
                           battery_slow_polarization_v;
    const float soc_voltage = battery_ocv_voltage_to_soc(vocv_est);

    if (!soc_initialized) {
        soc_est_percent = soc_voltage;
        soc_display_percent = soc_voltage;
        soc_initialized = true;
        return percent_to_u8(soc_display_percent);
    }

    const float coulomb_delta_percent =
        battery_current_a * dt_s /
        (battery_capacity_ah() * 3600.0f) * 100.0f;
    soc_est_percent =
        clamp_float(soc_est_percent - coulomb_delta_percent, 0.0f, 100.0f);

    if (battery_freeze_remaining_s <= 0.0f) {
        const float k = led_duty_sum <= BATTERY_LOW_LOAD_DUTY_SUM ?
                        BATTERY_CAL_K_LOW_LOAD :
                        BATTERY_CAL_K_NORMAL_LOAD;
        soc_est_percent += (soc_voltage - soc_est_percent) * k;
        soc_est_percent = clamp_float(soc_est_percent, 0.0f, 100.0f);
    }

    float max_rise = BATTERY_DISPLAY_RISE_RATE_PPS * dt_s;
    const float max_fall = BATTERY_DISPLAY_FALL_RATE_PPS * dt_s;
    if (led_duty_sum > BATTERY_LOW_LOAD_DUTY_SUM &&
        soc_est_percent > soc_display_percent) {
        max_rise = 0.0f;
    }

    const float lower = soc_display_percent - max_fall;
    const float upper = soc_display_percent + max_rise;
    soc_display_percent = clamp_float(soc_est_percent, lower, upper);
    soc_display_percent = clamp_float(soc_display_percent, 0.0f, 100.0f);
    return percent_to_u8(soc_display_percent);
}

static void reset_battery_estimator(void)
{
    soc_initialized = false;
    battery_slow_polarization_v = 0.0f;
    battery_freeze_remaining_s = 0.0f;
    previous_load_initialized = false;
    previous_battery_sample_us = 0;
}

static void update_battery_snapshot(esp_err_t read_err, uint32_t battery_adc_mv,
                                    const hal_led_duty_snapshot_t *led_duty,
                                    bool fan_on, float dt_s)
{
    if (read_err == ESP_OK) {
        const float voltage_v =
            app_sensor_battery_adc_mv_to_pack_voltage_v(battery_adc_mv);
        filtered_battery_voltage_v =
            filter_value(filtered_battery_voltage_v, voltage_v,
                         &battery_filter_initialized);

        latest_snapshot.battery_adc_mv = battery_adc_mv;
        latest_snapshot.battery_voltage_v = filtered_battery_voltage_v;
        latest_snapshot.battery_percent =
            estimate_battery_percent(filtered_battery_voltage_v, led_duty,
                                     fan_on, dt_s);
        latest_snapshot.battery_valid = true;
        battery_fault_logged = false;
        return;
    }

    latest_snapshot.battery_adc_mv = 0;
    latest_snapshot.battery_voltage_v = NAN;
    latest_snapshot.battery_percent = 0;
    latest_snapshot.battery_valid = false;
    battery_filter_initialized = false;
    reset_battery_estimator();
}

static void update_ntc_snapshot(esp_err_t read_err, uint32_t ntc_adc_mv,
                                esp_err_t convert_err, float ntc_temp_c)
{
    latest_snapshot.ntc_adc_mv = (read_err == ESP_OK) ? ntc_adc_mv : 0;

    if (read_err == ESP_OK && convert_err == ESP_OK) {
        filtered_ntc_temp_c =
            filter_value(filtered_ntc_temp_c, ntc_temp_c,
                         &ntc_filter_initialized);
        latest_snapshot.ntc_temp_c = filtered_ntc_temp_c;
        latest_snapshot.ntc_valid = true;
        ntc_fault_logged = false;
        return;
    }

    latest_snapshot.ntc_temp_c = NAN;
    latest_snapshot.ntc_valid = false;
    ntc_filter_initialized = false;
}

esp_err_t app_sensor_sample_now(void)
{
    if (!initialized) {
        ESP_LOGE(TAG, "Sensor algorithms are not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t battery_adc_mv = 0;
    uint32_t ntc_adc_mv = 0;
    float ntc_temp_c = NAN;
    hal_led_duty_snapshot_t led_duty;
    bool fan_on = false;
    const float battery_dt_s = battery_sample_dt_s();

    read_load_state(&led_duty, &fan_on);

    const esp_err_t battery_err = hal_adc_read_battery_mv(&battery_adc_mv);
    const esp_err_t ntc_read_err = hal_adc_read_ntc_mv(&ntc_adc_mv);
    const esp_err_t ntc_convert_err =
        (ntc_read_err == ESP_OK) ?
        app_sensor_ntc_mv_to_temp_c(ntc_adc_mv, &ntc_temp_c) :
        ntc_read_err;

    if (xSemaphoreTake(snapshot_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to lock sensor snapshot");
        return ESP_ERR_TIMEOUT;
    }

    update_battery_snapshot(battery_err, battery_adc_mv, &led_duty, fan_on,
                            battery_dt_s);
    update_ntc_snapshot(ntc_read_err, ntc_adc_mv, ntc_convert_err, ntc_temp_c);

    const bool log_battery_fault = (battery_err != ESP_OK) &&
                                   !battery_fault_logged;
    const bool log_ntc_fault = (ntc_convert_err != ESP_OK) &&
                               !ntc_fault_logged;
    battery_fault_logged = battery_fault_logged || log_battery_fault;
    ntc_fault_logged = ntc_fault_logged || log_ntc_fault;

    const app_sensor_snapshot_t snapshot = latest_snapshot;
    xSemaphoreGive(snapshot_mutex);

    const app_state_sensor_update_t state_update = {
        .battery_voltage_v = snapshot.battery_voltage_v,
        .battery_percent = snapshot.battery_percent,
        .battery_valid = snapshot.battery_valid,
        .ntc_temp_c = snapshot.ntc_temp_c,
        .ntc_valid = snapshot.ntc_valid,
    };
    const esp_err_t state_err = app_state_update_sensors(&state_update);
    if (state_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update application sensor state: %s",
                 esp_err_to_name(state_err));
    }

    if (log_battery_fault) {
        ESP_LOGW(TAG, "Battery ADC reading is invalid: %s",
                 esp_err_to_name(battery_err));
    }

    if (log_ntc_fault) {
        ESP_LOGW(TAG, "NTC reading is invalid: adc=%" PRIu32 " mV, error=%s",
                 ntc_adc_mv, esp_err_to_name(ntc_convert_err));
    }

    ESP_LOGD(TAG, "Sensors: battery=%" PRIu32 " mV %.2f V %u%%, LED duty C=%u W=%u, fan=%s, NTC=%" PRIu32 " mV %.1f C",
             snapshot.battery_adc_mv, snapshot.battery_voltage_v,
             (unsigned int)snapshot.battery_percent,
             (unsigned int)led_duty.cold_duty,
             (unsigned int)led_duty.warm_duty, fan_on ? "on" : "off",
             snapshot.ntc_adc_mv,
             snapshot.ntc_temp_c);

    if (battery_err != ESP_OK) {
        return battery_err;
    }

    if (ntc_convert_err != ESP_OK) {
        return ntc_convert_err;
    }

    return state_err;
}

esp_err_t app_sensor_get_snapshot(app_sensor_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(snapshot_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    *snapshot = latest_snapshot;
    xSemaphoreGive(snapshot_mutex);
    return ESP_OK;
}

static void sensor_task(void *arg)
{
    (void)arg;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(APP_SENSOR_SAMPLE_PERIOD_MS));
        (void)app_sensor_sample_now();
    }
}

esp_err_t app_sensor_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    snapshot_mutex = xSemaphoreCreateMutex();
    if (snapshot_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create sensor snapshot mutex");
        return ESP_ERR_NO_MEM;
    }

    memset(&latest_snapshot, 0, sizeof(latest_snapshot));
    latest_snapshot.battery_voltage_v = NAN;
    latest_snapshot.ntc_temp_c = NAN;
    reset_battery_estimator();
    initialized = true;

    esp_err_t err = app_sensor_sample_now();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Initial sensor sample is invalid: %s",
                 esp_err_to_name(err));
    }

    BaseType_t task_created =
        xTaskCreate(sensor_task, "sensor", SENSOR_TASK_STACK_SIZE, NULL,
                    SENSOR_TASK_PRIORITY, NULL);
    if (task_created != pdPASS) {
        initialized = false;
        vSemaphoreDelete(snapshot_mutex);
        snapshot_mutex = NULL;
        ESP_LOGE(TAG, "Failed to create sensor task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Sensor task started with a %u ms period",
             (unsigned int)APP_SENSOR_SAMPLE_PERIOD_MS);
    return ESP_OK;
}
