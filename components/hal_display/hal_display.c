#include "hal_display.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "HAL_DISPLAY";

#define LCD_HOST                         SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ               (20 * 1000 * 1000)
#define LCD_CMD_BITS                     8
#define LCD_PARAM_BITS                   8
#define LCD_SPI_QUEUE_DEPTH              10
#define LCD_DRAW_BUFFER_LINES            24
#define LCD_STARTUP_LOGO_DURATION_MS     700
#define LCD_STARTUP_LOGO_BACKLIGHT       80

#define LCD_BACKLIGHT_SPEED_MODE         LEDC_LOW_SPEED_MODE
#define LCD_BACKLIGHT_TIMER              LEDC_TIMER_2
#define LCD_BACKLIGHT_CHANNEL            LEDC_CHANNEL_3
#define LCD_BACKLIGHT_PWM_FREQ_HZ        5000
#define LCD_BACKLIGHT_DUTY_RESOLUTION    LEDC_TIMER_10_BIT
#define LCD_BACKLIGHT_DUTY_MAX           1023
#define LCD_BACKLIGHT_PERCENT_MAX        100

#define LVGL_TICK_PERIOD_MS              2
#define LVGL_TASK_STACK_SIZE             8192
#define LVGL_TASK_PRIORITY               3
#define LVGL_TASK_MIN_DELAY_MS           10
#define LVGL_TASK_MAX_DELAY_MS           500

static bool initialized;
static esp_lcd_panel_io_handle_t panel_io;
static esp_lcd_panel_handle_t panel;
static lv_display_t *lvgl_display;
static void *lvgl_draw_buffer_1;
static void *lvgl_draw_buffer_2;
static uint16_t *startup_logo_buffer;
static SemaphoreHandle_t startup_logo_flush_sem;
static esp_timer_handle_t lvgl_tick_timer;
static SemaphoreHandle_t lvgl_mutex;

static uint32_t backlight_percent_to_duty(uint8_t percent)
{
    return ((uint32_t)percent * LCD_BACKLIGHT_DUTY_MAX +
            (LCD_BACKLIGHT_PERCENT_MAX / 2)) / LCD_BACKLIGHT_PERCENT_MAX;
}

static esp_err_t update_backlight_duty(uint8_t percent)
{
    esp_err_t err = ledc_set_duty(LCD_BACKLIGHT_SPEED_MODE,
                                  LCD_BACKLIGHT_CHANNEL,
                                  backlight_percent_to_duty(percent));
    if (err != ESP_OK) {
        return err;
    }

    return ledc_update_duty(LCD_BACKLIGHT_SPEED_MODE, LCD_BACKLIGHT_CHANNEL);
}

static esp_err_t configure_backlight_pwm(void)
{
    const gpio_config_t safe_gpio_config = {
        .pin_bit_mask = 1ULL << BOARD_GPIO_LCD_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&safe_gpio_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure safe backlight GPIO: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = gpio_set_level(BOARD_GPIO_LCD_BACKLIGHT,
                         BOARD_LCD_BACKLIGHT_OFF_LEVEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to turn off LCD backlight GPIO: %s",
                 esp_err_to_name(err));
        return err;
    }

    const ledc_timer_config_t timer_config = {
        .speed_mode = LCD_BACKLIGHT_SPEED_MODE,
        .duty_resolution = LCD_BACKLIGHT_DUTY_RESOLUTION,
        .timer_num = LCD_BACKLIGHT_TIMER,
        .freq_hz = LCD_BACKLIGHT_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };

    err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LCD backlight PWM timer: %s",
                 esp_err_to_name(err));
        return err;
    }

    const ledc_channel_config_t channel_config = {
        .gpio_num = BOARD_GPIO_LCD_BACKLIGHT,
        .speed_mode = LCD_BACKLIGHT_SPEED_MODE,
        .channel = LCD_BACKLIGHT_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_BACKLIGHT_TIMER,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags.output_invert = BOARD_LCD_BACKLIGHT_OUTPUT_INVERTED,
    };

    err = ledc_channel_config(&channel_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LCD backlight PWM channel: %s",
                 esp_err_to_name(err));
        (void)gpio_set_level(BOARD_GPIO_LCD_BACKLIGHT,
                             BOARD_LCD_BACKLIGHT_OFF_LEVEL);
        return err;
    }

    err = update_backlight_duty(0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set initial backlight duty: %s",
                 esp_err_to_name(err));
    }

    return err;
}

static esp_err_t initialize_spi_panel(void)
{
    const spi_bus_config_t bus_config = {
        .sclk_io_num = BOARD_GPIO_LCD_SCK,
        .mosi_io_num = BOARD_GPIO_LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = BOARD_LCD_H_RES * LCD_DRAW_BUFFER_LINES *
                           sizeof(uint16_t),
    };

    esp_err_t err = spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LCD SPI bus: %s",
                 esp_err_to_name(err));
        return err;
    }
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BOARD_GPIO_LCD_DC,
        .cs_gpio_num = BOARD_GPIO_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = LCD_SPI_QUEUE_DEPTH,
    };

    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                   &io_config, &panel_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attach ST7789 panel IO: %s",
                 esp_err_to_name(err));
        return err;
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_GPIO_LCD_RST,
        .rgb_ele_order = BOARD_LCD_BGR_ORDER ?
                         LCD_RGB_ELEMENT_ORDER_BGR :
                         LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };

    err = esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ST7789 panel: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_reset(panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset ST7789 panel: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_init(panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ST7789 panel: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_set_gap(panel, BOARD_LCD_X_GAP, BOARD_LCD_Y_GAP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set LCD offset: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_invert_color(panel, BOARD_LCD_COLOR_INVERTED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LCD inversion: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_swap_xy(panel, BOARD_LCD_SWAP_XY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LCD axis swap: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_mirror(panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LCD mirroring: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_disp_on_off(panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable ST7789 display output: %s",
                 esp_err_to_name(err));
    }

    return err;
}

static uint16_t swap_rgb565_bytes(uint16_t color)
{
    return (uint16_t)((color << 8) | (color >> 8));
}

static bool notify_startup_logo_flush_ready(esp_lcd_panel_io_handle_t io,
                                            esp_lcd_panel_io_event_data_t *event_data,
                                            void *user_ctx)
{
    (void)io;
    (void)event_data;

    SemaphoreHandle_t done_sem = (SemaphoreHandle_t)user_ctx;
    BaseType_t high_task_woken = pdFALSE;
    if (done_sem != NULL) {
        xSemaphoreGiveFromISR(done_sem, &high_task_woken);
    }
    return high_task_woken == pdTRUE;
}

static esp_err_t wait_startup_logo_flush(void)
{
    if (startup_logo_flush_sem == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(startup_logo_flush_sem, pdMS_TO_TICKS(1000))
        != pdTRUE) {
        ESP_LOGE(TAG, "Timed out waiting for startup logo flush");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    return (uint16_t)(((uint16_t)(red & 0xF8) << 8) |
                      ((uint16_t)(green & 0xFC) << 3) |
                      ((uint16_t)blue >> 3));
}

static uint16_t blend_rgb565(uint16_t color_a, uint16_t color_b,
                             uint8_t blend_255)
{
    const uint8_t inv = (uint8_t)(255U - blend_255);
    const uint8_t a_red = (uint8_t)(((color_a >> 11) & 0x1F) << 3);
    const uint8_t a_green = (uint8_t)(((color_a >> 5) & 0x3F) << 2);
    const uint8_t a_blue = (uint8_t)((color_a & 0x1F) << 3);
    const uint8_t b_red = (uint8_t)(((color_b >> 11) & 0x1F) << 3);
    const uint8_t b_green = (uint8_t)(((color_b >> 5) & 0x3F) << 2);
    const uint8_t b_blue = (uint8_t)((color_b & 0x1F) << 3);

    return rgb565((uint8_t)(((uint16_t)a_red * inv +
                             (uint16_t)b_red * blend_255) / 255U),
                  (uint8_t)(((uint16_t)a_green * inv +
                             (uint16_t)b_green * blend_255) / 255U),
                  (uint8_t)(((uint16_t)a_blue * inv +
                             (uint16_t)b_blue * blend_255) / 255U));
}

static bool in_rect(uint16_t x, uint16_t y, uint16_t left, uint16_t top,
                    uint16_t right, uint16_t bottom)
{
    return x >= left && x < right && y >= top && y < bottom;
}

static bool in_circle(uint16_t x, uint16_t y, int16_t center_x,
                      int16_t center_y, int16_t radius)
{
    const int16_t dx = (int16_t)x - center_x;
    const int16_t dy = (int16_t)y - center_y;
    return (int32_t)dx * dx + (int32_t)dy * dy <= (int32_t)radius * radius;
}

static int32_t ellipse_metric(uint16_t x, uint16_t y, int16_t center_x,
                              int16_t center_y, int16_t radius_x,
                              int16_t radius_y)
{
    const int32_t dx = (int32_t)x - center_x;
    const int32_t dy = (int32_t)y - center_y;
    return (dx * dx * 10000L) / ((int32_t)radius_x * radius_x)
           + (dy * dy * 10000L) / ((int32_t)radius_y * radius_y);
}

static bool in_upper_ellipse_ring(uint16_t x, uint16_t y, int16_t center_x,
                                  int16_t center_y, int16_t radius_x,
                                  int16_t radius_y, int16_t thickness)
{
    if ((int16_t)y > center_y || radius_x <= thickness
        || radius_y <= thickness) {
        return false;
    }

    const int32_t outer = ellipse_metric(x, y, center_x, center_y,
                                         radius_x, radius_y);
    const int32_t inner = ellipse_metric(x, y, center_x, center_y,
                                         (int16_t)(radius_x - thickness),
                                         (int16_t)(radius_y - thickness));
    return outer <= 10000L && inner >= 10000L;
}

static bool in_startup_arches(uint16_t x, uint16_t y)
{
    return in_upper_ellipse_ring(x, y, 91, 178, 45, 115, 15)
           || in_upper_ellipse_ring(x, y, 149, 178, 45, 115, 15)
           || in_rect(x, y, 46, 164, 62, 200)
           || in_rect(x, y, 102, 164, 118, 200)
           || in_rect(x, y, 122, 164, 138, 200)
           || in_rect(x, y, 178, 164, 194, 200);
}

static void fill_default_logo_line(uint16_t y, uint16_t *line,
                                   uint16_t width)
{
    const uint16_t red_top = rgb565(218, 41, 28);
    const uint16_t red_bottom = rgb565(166, 18, 26);
    const uint16_t red_dark = rgb565(116, 15, 22);
    const uint16_t gold = rgb565(255, 199, 44);
    const uint16_t gold_light = rgb565(255, 224, 96);
    const uint16_t gold_shadow = rgb565(194, 113, 0);

    for (uint16_t x = 0; x < width; ++x) {
        const uint8_t vertical_blend =
            (uint8_t)(((uint32_t)y * 255U) / (BOARD_LCD_V_RES - 1U));
        uint16_t color = blend_rgb565(red_top, red_bottom, vertical_blend);

        if (!in_circle(x, y, 120, 120, 116)) {
            color = blend_rgb565(color, red_dark, 58);
        }
        if (in_circle(x, y, 120, 105, 82)) {
            color = blend_rgb565(color, red_top, 54);
        }

        const bool has_shadow = x >= 4U && y >= 5U
            && in_startup_arches((uint16_t)(x - 4U),
                                 (uint16_t)(y - 5U));
        if (has_shadow) {
            color = blend_rgb565(color, red_dark, 150);
        }
        if (in_startup_arches(x, y)) {
            color = gold;
            if (x < 74U || (x >= 122U && x < 136U)) {
                color = blend_rgb565(color, gold_light, 68);
            }
            if (y > 172U) {
                color = blend_rgb565(color, gold_shadow, 48);
            }
        }

        line[x] = swap_rgb565_bytes(color);
    }
}

static esp_err_t draw_default_startup_logo(void)
{
    for (int y = 0; y < BOARD_LCD_V_RES; y += LCD_DRAW_BUFFER_LINES) {
        const int y_end = y + LCD_DRAW_BUFFER_LINES > BOARD_LCD_V_RES ?
                          BOARD_LCD_V_RES : y + LCD_DRAW_BUFFER_LINES;
        const size_t line_pixels = BOARD_LCD_H_RES;

        for (int row = y; row < y_end; ++row) {
            fill_default_logo_line((uint16_t)row,
                                   &startup_logo_buffer[(row - y) *
                                                        line_pixels],
                                   BOARD_LCD_H_RES);
        }

        esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, y,
                                                  BOARD_LCD_H_RES, y_end,
                                                  startup_logo_buffer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to draw default startup logo: %s",
                     esp_err_to_name(err));
            return err;
        }
        err = wait_startup_logo_flush();
        if (err != ESP_OK) {
            return err;
        }
    }

    ESP_LOGI(TAG, "Showing default startup logo");
    return ESP_OK;
}

static esp_err_t show_startup_logo(void)
{
    const size_t buffer_pixels = BOARD_LCD_H_RES * LCD_DRAW_BUFFER_LINES;
    const size_t buffer_size = buffer_pixels * sizeof(uint16_t);

    startup_logo_buffer = spi_bus_dma_memory_alloc(LCD_HOST, buffer_size, 0);
    if (startup_logo_buffer == NULL) {
        ESP_LOGW(TAG, "Skipping startup logo: DMA allocation failed");
        return ESP_OK;
    }

    startup_logo_flush_sem = xSemaphoreCreateBinary();
    if (startup_logo_flush_sem == NULL) {
        ESP_LOGW(TAG, "Skipping startup logo: flush semaphore allocation failed");
        heap_caps_free(startup_logo_buffer);
        startup_logo_buffer = NULL;
        return ESP_OK;
    }

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = notify_startup_logo_flush_ready,
    };
    esp_err_t err = esp_lcd_panel_io_register_event_callbacks(
        panel_io, &callbacks, startup_logo_flush_sem);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register startup logo flush callback: %s",
                 esp_err_to_name(err));
        vSemaphoreDelete(startup_logo_flush_sem);
        startup_logo_flush_sem = NULL;
        heap_caps_free(startup_logo_buffer);
        startup_logo_buffer = NULL;
        return err;
    }

    err = draw_default_startup_logo();
    if (err != ESP_OK) {
        vSemaphoreDelete(startup_logo_flush_sem);
        startup_logo_flush_sem = NULL;
        heap_caps_free(startup_logo_buffer);
        startup_logo_buffer = NULL;
        return err;
    }

    err = update_backlight_duty(LCD_STARTUP_LOGO_BACKLIGHT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to light startup logo: %s",
                 esp_err_to_name(err));
        vSemaphoreDelete(startup_logo_flush_sem);
        startup_logo_flush_sem = NULL;
        heap_caps_free(startup_logo_buffer);
        startup_logo_buffer = NULL;
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(LCD_STARTUP_LOGO_DURATION_MS));

    err = update_backlight_duty(0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to turn off backlight after startup logo: %s",
                 esp_err_to_name(err));
    }

    vSemaphoreDelete(startup_logo_flush_sem);
    startup_logo_flush_sem = NULL;
    heap_caps_free(startup_logo_buffer);
    startup_logo_buffer = NULL;
    return err;
}

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *event_data,
                                    void *user_ctx)
{
    (void)io;
    (void)event_data;

    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

static void lvgl_flush(lv_display_t *display, const lv_area_t *area,
                       uint8_t *pixel_map)
{
    const int x_start = area->x1;
    const int x_end = area->x2 + 1;
    const int y_start = area->y1;
    const int y_end = area->y2 + 1;
    const size_t pixel_count = (size_t)(x_end - x_start) *
                               (size_t)(y_end - y_start);

    lv_draw_sw_rgb565_swap(pixel_map, pixel_count);

    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, x_start, y_start,
                                               x_end, y_end, pixel_map);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to flush LVGL area: %s", esp_err_to_name(err));
        lv_display_flush_ready(display);
    }
}

static void increase_lvgl_tick(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "LVGL handler task started");

    while (true) {
        uint32_t delay_ms = LVGL_TASK_MAX_DELAY_MS;

        if (xSemaphoreTakeRecursive(lvgl_mutex, portMAX_DELAY) == pdTRUE) {
            delay_ms = lv_timer_handler();
            xSemaphoreGiveRecursive(lvgl_mutex);
        }

        if (delay_ms < LVGL_TASK_MIN_DELAY_MS) {
            delay_ms = LVGL_TASK_MIN_DELAY_MS;
        } else if (delay_ms > LVGL_TASK_MAX_DELAY_MS) {
            delay_ms = LVGL_TASK_MAX_DELAY_MS;
        }

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static esp_err_t initialize_lvgl_port(void)
{
    lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    if (lvgl_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
        return ESP_ERR_NO_MEM;
    }

    lv_init();

    lvgl_display = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    if (lvgl_display == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        return ESP_ERR_NO_MEM;
    }

    const size_t draw_buffer_size = BOARD_LCD_H_RES * LCD_DRAW_BUFFER_LINES *
                                    sizeof(lv_color16_t);
    lvgl_draw_buffer_1 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_size, 0);
    lvgl_draw_buffer_2 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_size, 0);
    if (lvgl_draw_buffer_1 == NULL || lvgl_draw_buffer_2 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LVGL DMA draw buffers");
        return ESP_ERR_NO_MEM;
    }

    lv_display_set_buffers(lvgl_display, lvgl_draw_buffer_1,
                           lvgl_draw_buffer_2, draw_buffer_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(lvgl_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(lvgl_display, lvgl_flush);

    const esp_lcd_panel_io_callbacks_t panel_io_callbacks = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };

    esp_err_t err = esp_lcd_panel_io_register_event_callbacks(
        panel_io, &panel_io_callbacks, lvgl_display);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register LVGL flush callback: %s",
                 esp_err_to_name(err));
        return err;
    }

    const esp_timer_create_args_t tick_timer_args = {
        .callback = increase_lvgl_tick,
        .name = "lvgl_tick",
    };

    err = esp_timer_create(&tick_timer_args, &lvgl_tick_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LVGL tick timer: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = esp_timer_start_periodic(lvgl_tick_timer,
                                   LVGL_TICK_PERIOD_MS * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start LVGL tick timer: %s",
                 esp_err_to_name(err));
        return err;
    }

    BaseType_t task_created = xTaskCreate(lvgl_task, "lvgl",
                                          LVGL_TASK_STACK_SIZE, NULL,
                                          LVGL_TASK_PRIORITY, NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LVGL handler task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t hal_display_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    esp_err_t err = configure_backlight_pwm();
    if (err != ESP_OK) {
        return err;
    }

    err = initialize_spi_panel();
    if (err != ESP_OK) {
        (void)update_backlight_duty(0);
        return err;
    }

    err = show_startup_logo();
    if (err != ESP_OK) {
        (void)update_backlight_duty(0);
        return err;
    }

    err = initialize_lvgl_port();
    if (err != ESP_OK) {
        (void)update_backlight_duty(0);
        return err;
    }

    initialized = true;

    ESP_LOGI(TAG, "ST7789 LCD initialized: %ux%u, SPI=%u Hz, backlight is off",
             (unsigned int)BOARD_LCD_H_RES, (unsigned int)BOARD_LCD_V_RES,
             (unsigned int)LCD_PIXEL_CLOCK_HZ);
    ESP_LOGW(TAG, "LCD offset, color order, inversion, orientation, and backlight polarity are provisional");
    return ESP_OK;
}

esp_err_t hal_display_set_backlight(uint8_t percent)
{
    if (!initialized) {
        ESP_LOGE(TAG, "Display HAL is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (percent > LCD_BACKLIGHT_PERCENT_MAX) {
        ESP_LOGE(TAG, "LCD backlight duty out of range: %u", percent);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = update_backlight_duty(percent);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update LCD backlight duty: %s",
                 esp_err_to_name(err));
        (void)update_backlight_duty(0);
        return err;
    }

    ESP_LOGI(TAG, "LCD backlight updated: %u%%", percent);
    return ESP_OK;
}

lv_display_t *hal_display_get_lvgl_display(void)
{
    return initialized ? lvgl_display : NULL;
}

bool hal_display_lock(uint32_t timeout_ms)
{
    if (!initialized || lvgl_mutex == NULL) {
        return false;
    }

    const TickType_t timeout_ticks =
        timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(lvgl_mutex, timeout_ticks) == pdTRUE;
}

void hal_display_unlock(void)
{
    if (initialized && lvgl_mutex != NULL) {
        xSemaphoreGiveRecursive(lvgl_mutex);
    }
}
