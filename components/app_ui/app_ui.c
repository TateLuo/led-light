#include "app_ui.h"

#include "app_battery_test.h"
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_settings.h"
#include "app_state.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal_display.h"
#include "lvgl.h"

static const char *TAG = "APP_UI";

#define UI_TASK_STACK_SIZE          5120
#define UI_TASK_PRIORITY            6
#define UI_REFRESH_PERIOD_MS        100
#define UI_CONTROL_REFRESH_MS       20
#define UI_LOCK_TIMEOUT_MS          100
#define UI_MESSAGE_TEXT_MAX_LEN     48
#define UI_SENSOR_TEXT_MAX_LEN      32
#define UI_BRIGHTNESS_DISPLAY_STEP  1
#define UI_CCT_DISPLAY_STEP_KELVIN  100
#define UI_CONTROL_CATCH_UP_STEPS   2
#define UI_SCREEN_WIDTH             240
#define UI_SCREEN_HEIGHT            240
#define UI_SETTINGS_PANEL_TOP       50
#define UI_SETTINGS_PANEL_HEIGHT    186
#define UI_SETTINGS_ROW_WIDTH       220
#define UI_SETTINGS_ROW_HEIGHT      44
#define UI_SETTINGS_ROW_GAP         6
#define UI_SETTINGS_ROW_TOP_PAD     2
#define UI_MAIN_CONTROL_LABEL_X     14
#define UI_MAIN_CONTROL_LABEL_WIDTH 212
#define UI_MAIN_BAR_WIDTH           212
#define UI_MAIN_BAR_HEIGHT          18
#define UI_MAIN_BRIGHTNESS_LABEL_Y  66
#define UI_MAIN_BRIGHTNESS_BAR_Y    93
#define UI_MAIN_CCT_LABEL_Y         139
#define UI_MAIN_CCT_BAR_Y           166
#define UI_BATTERY_ICON_X           8
#define UI_BATTERY_ICON_Y           11
#define UI_BATTERY_BODY_WIDTH       25
#define UI_BATTERY_BODY_HEIGHT      13
#define UI_BATTERY_CAP_WIDTH        3
#define UI_BATTERY_CAP_HEIGHT       7
#define UI_BATTERY_FILL_MAX_WIDTH   19
#define UI_BATTERY_FILL_HEIGHT      7
#define UI_BATTERY_LABEL_X          40
#define UI_BATTERY_LABEL_Y          7
#define UI_BATTERY_LABEL_WIDTH      94
#define UI_TEMPERATURE_ICON_X       154
#define UI_TEMPERATURE_ICON_Y       13
#define UI_TEMPERATURE_LABEL_X      174
#define UI_TEMPERATURE_LABEL_Y      7
#define UI_TEMPERATURE_LABEL_WIDTH  58
#define UI_STATUS_CARD_WIDTH        220
#define UI_STATUS_CARD_HEIGHT       25
#define UI_STATUS_CARD_Y            31
#define UI_STATUS_DOT_SIZE          8
#define UI_STATUS_DOT_X             16
#define UI_STATUS_LABEL_X           32
#define UI_STATUS_LABEL_WIDTH       170

#define UI_COLOR_BG                 0xFFFFFF
#define UI_COLOR_PANEL              0xF8FAFC
#define UI_COLOR_ROW                0xFFFFFF
#define UI_COLOR_ROW_SELECTED       0xDBEAFE
#define UI_COLOR_BORDER             0xCBD5E1
#define UI_COLOR_BORDER_SELECTED    0x2563EB
#define UI_COLOR_BAR_TRACK          0xE2E8F0
#define UI_COLOR_TEXT_PRIMARY       0x0F172A
#define UI_COLOR_TEXT_SECONDARY     0x334155
#define UI_COLOR_TEXT_MUTED         0x64748B
#define UI_COLOR_ACCENT_BLUE        0x2563EB
#define UI_COLOR_ACCENT_AMBER       0xD97706
#define UI_COLOR_ACCENT_GREEN       0x16A34A
#define UI_COLOR_LABEL_BLUE         0x1D4ED8
#define UI_COLOR_LABEL_AMBER        0xB45309
#define UI_COLOR_LABEL_GREEN        0x15803D
#define UI_COLOR_LABEL_DANGER       0xB4233A
#define UI_COLOR_STATUS_BLUE_BG     0xDBEAFE
#define UI_COLOR_STATUS_BLUE_BORDER 0x93C5FD
#define UI_COLOR_STATUS_AMBER_BG    0xFEF3C7
#define UI_COLOR_STATUS_AMBER_BORDER 0xFCD34D
#define UI_COLOR_STATUS_GREEN_BG    0xDCFCE7
#define UI_COLOR_STATUS_GREEN_BORDER 0x86EFAC
#define UI_COLOR_STATUS_DANGER_BG   0xFEE2E2
#define UI_COLOR_STATUS_DANGER_BORDER 0xFDA4AF

typedef struct {
    lv_obj_t *row;
    lv_obj_t *title_label;
    lv_obj_t *value_label;
} settings_row_widgets_t;

static bool initialized;
static SemaphoreHandle_t message_mutex;
static TaskHandle_t ui_task_handle;
static bool settings_page_active;
static app_ui_settings_item_t selected_settings_item =
    APP_UI_SETTINGS_ITEM_BACKLIGHT;
static app_ui_settings_item_t last_styled_settings_item =
    APP_UI_SETTINGS_ITEM_COUNT;
static app_ui_settings_item_t last_scrolled_settings_item =
    APP_UI_SETTINGS_ITEM_COUNT;
static bool settings_visible_rendered;
static bool ota_visible_rendered;
static char transient_message[UI_MESSAGE_TEXT_MAX_LEN];
static int64_t transient_message_until_us;
static lv_obj_t *main_panel;
static lv_obj_t *settings_panel;
static lv_obj_t *ota_panel;
static settings_row_widgets_t settings_rows[APP_UI_SETTINGS_ITEM_COUNT];
static lv_obj_t *battery_icon_body;
static lv_obj_t *battery_icon_fill;
static lv_obj_t *battery_icon_cap;
static lv_obj_t *battery_label;
static lv_obj_t *temperature_icon_stem;
static lv_obj_t *temperature_icon_bulb;
static lv_obj_t *temperature_label;
static lv_obj_t *status_card;
static lv_obj_t *status_dot;
static lv_obj_t *status_label;
static lv_obj_t *brightness_label;
static lv_obj_t *brightness_bar;
static lv_obj_t *cct_label;
static lv_obj_t *cct_bar;
static lv_obj_t *ota_line1_label;
static lv_obj_t *ota_line2_label;
static lv_obj_t *ota_line3_label;
static lv_obj_t *ota_line4_label;
static uint8_t displayed_brightness_percent =
    APP_STATE_BRIGHTNESS_DEFAULT_PERCENT;
static uint16_t displayed_cct_kelvin = APP_STATE_CCT_DEFAULT_KELVIN;
static uint8_t last_rendered_brightness_percent;
static uint16_t last_rendered_cct_kelvin;
static bool main_widgets_rendered;

static lv_obj_t *create_value_label(lv_obj_t *parent, const char *text,
                                    lv_align_t align, int32_t x_offset,
                                    int32_t y_offset)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 100);  // Limit width to prevent text overflow
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);  // Clip overflowing text
    lv_obj_align(label, align, x_offset, y_offset);
    return label;
}

static lv_obj_t *create_bar(lv_obj_t *parent, int32_t min_value,
                            int32_t max_value, int32_t value,
                            int32_t y_offset, lv_color_t color)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, UI_MAIN_BAR_WIDTH, UI_MAIN_BAR_HEIGHT);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, y_offset);
    lv_bar_set_range(bar, min_value, max_value);
    lv_bar_set_value(bar, value, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_BAR_TRACK),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, color, LV_PART_INDICATOR);
    return bar;
}

static const char *fault_name(system_fault_t fault)
{
    switch (fault) {
    case SYSTEM_FAULT_LOW_BATTERY:
        return "LOW BAT";
    case SYSTEM_FAULT_CRITICAL_BATTERY:
        return "BAT CRIT";
    case SYSTEM_FAULT_OVER_TEMP:
        return "HOT";
    case SYSTEM_FAULT_NTC_ERROR:
        return "TEMP ERR";
    case SYSTEM_FAULT_ADC_ERROR:
        return "ADC ERR";
    case SYSTEM_FAULT_NONE:
    default:
        return "NONE";
    }
}

static lv_color_t battery_color(uint8_t percent)
{
    if (percent <= 10) {
        return lv_color_hex(UI_COLOR_LABEL_DANGER);
    }
    if (percent <= 25) {
        return lv_color_hex(UI_COLOR_ACCENT_AMBER);
    }
    return lv_color_hex(UI_COLOR_ACCENT_GREEN);
}

static lv_color_t temperature_color(float temp_c)
{
    if (temp_c >= 60.0f) {
        return lv_color_hex(UI_COLOR_LABEL_DANGER);
    }
    if (temp_c >= 45.0f) {
        return lv_color_hex(UI_COLOR_ACCENT_AMBER);
    }
    return lv_color_hex(UI_COLOR_LABEL_AMBER);
}

static void set_battery_icon(uint8_t percent, bool valid)
{
    const uint8_t clamped = percent > 100 ? 100 : percent;
    const uint16_t fill_width =
        (uint16_t)(((uint32_t)UI_BATTERY_FILL_MAX_WIDTH * clamped + 50U) /
                   100U);
    const lv_color_t color = valid ? battery_color(clamped) :
                             lv_color_hex(UI_COLOR_TEXT_MUTED);

    lv_obj_set_style_border_color(battery_icon_body, color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(battery_icon_cap, color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(battery_icon_fill, color, LV_PART_MAIN);

    if (!valid || fill_width == 0) {
        lv_obj_add_flag(battery_icon_fill, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(battery_icon_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(battery_icon_fill, fill_width);
}

static void set_temperature_icon(float temp_c, bool valid)
{
    const lv_color_t color = valid ? temperature_color(temp_c) :
                             lv_color_hex(UI_COLOR_TEXT_MUTED);

    lv_obj_set_style_bg_color(temperature_icon_stem, color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(temperature_icon_bulb, color, LV_PART_MAIN);
}

static void set_battery_label(float voltage_v, uint8_t percent)
{
    char text[UI_SENSOR_TEXT_MAX_LEN];
    const uint32_t centivolts =
        (uint32_t)((voltage_v * 100.0f) + 0.5f);

    (void)snprintf(text, sizeof(text), "%lu.%02luV %u%%",
                   (unsigned long)(centivolts / 100),
                   (unsigned long)(centivolts % 100),
                   (unsigned int)percent);
    lv_label_set_text(battery_label, text);
    lv_obj_set_style_text_color(battery_label, battery_color(percent),
                                LV_PART_MAIN);
    set_battery_icon(percent, true);
}

static void set_temperature_label(float temp_c)
{
    char text[UI_SENSOR_TEXT_MAX_LEN];
    const int32_t tenths =
        (int32_t)(temp_c >= 0.0f ?
                  (temp_c * 10.0f) + 0.5f :
                  (temp_c * 10.0f) - 0.5f);
    const int32_t abs_tenths = tenths < 0 ? -tenths : tenths;

    (void)snprintf(text, sizeof(text), "%s%ld.%ldC",
                   tenths < 0 ? "-" : "",
                   (long)(abs_tenths / 10),
                   (long)(abs_tenths % 10));
    lv_label_set_text(temperature_label, text);
    lv_obj_set_style_text_color(temperature_label, temperature_color(temp_c),
                                LV_PART_MAIN);
    set_temperature_icon(temp_c, true);
}

static bool copy_transient_message(char *message, size_t message_size)
{
    if (message == NULL || message_size == 0 || message_mutex == NULL) {
        return false;
    }

    if (xSemaphoreTake(message_mutex, 0) != pdTRUE) {
        return false;
    }

    const int64_t now_us = esp_timer_get_time();
    const bool active = transient_message_until_us > now_us;
    if (active) {
        (void)snprintf(message, message_size, "%s", transient_message);
    }

    xSemaphoreGive(message_mutex);
    return active;
}

static const char *compact_status_text(const char *text)
{
    if (text == NULL) {
        return "";
    }
    if (strcmp(text, "RELEASE KEY TO POWER OFF") == 0) {
        return "POWER OFF";
    }
    return text;
}

static void set_status_text(const char *text, uint32_t color_hex)
{
    uint32_t bg_hex = UI_COLOR_STATUS_AMBER_BG;
    uint32_t border_hex = UI_COLOR_STATUS_AMBER_BORDER;

    if (color_hex == UI_COLOR_LABEL_DANGER) {
        bg_hex = UI_COLOR_STATUS_DANGER_BG;
        border_hex = UI_COLOR_STATUS_DANGER_BORDER;
    } else if (color_hex == UI_COLOR_LABEL_BLUE) {
        bg_hex = UI_COLOR_STATUS_BLUE_BG;
        border_hex = UI_COLOR_STATUS_BLUE_BORDER;
    } else if (color_hex == UI_COLOR_LABEL_GREEN) {
        bg_hex = UI_COLOR_STATUS_GREEN_BG;
        border_hex = UI_COLOR_STATUS_GREEN_BORDER;
    }

    lv_obj_set_style_bg_color(status_card, lv_color_hex(bg_hex),
                              LV_PART_MAIN);
    lv_obj_set_style_border_color(status_card, lv_color_hex(border_hex),
                                  LV_PART_MAIN);
    lv_obj_set_style_bg_color(status_dot, lv_color_hex(color_hex),
                              LV_PART_MAIN);
    lv_label_set_text(status_label, compact_status_text(text));
    lv_obj_set_style_text_color(status_label,
                                lv_color_hex(color_hex),
                                LV_PART_MAIN);
}

static bool read_settings_page_state(app_ui_settings_item_t *selected_item)
{
    if (message_mutex == NULL) {
        if (selected_item != NULL) {
            *selected_item = selected_settings_item;
        }
        return settings_page_active;
    }

    if (xSemaphoreTake(message_mutex, 0) != pdTRUE) {
        if (selected_item != NULL) {
            *selected_item = selected_settings_item;
        }
        return settings_page_active;
    }

    const bool active = settings_page_active;
    if (selected_item != NULL) {
        *selected_item = selected_settings_item;
    }
    xSemaphoreGive(message_mutex);
    return active;
}

static void configure_plain_panel(lv_obj_t *panel)
{
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
}

static const char *settings_item_title(app_ui_settings_item_t item)
{
    switch (item) {
    case APP_UI_SETTINGS_ITEM_BACKLIGHT:
        return "Backlight";
    case APP_UI_SETTINGS_ITEM_CCT_STEP:
        return "CCT Step";
    case APP_UI_SETTINGS_ITEM_FAN_MANUAL:
        return "Fan Manual";
    case APP_UI_SETTINGS_ITEM_WIFI_OTA:
        return "WiFi Service";
    case APP_UI_SETTINGS_ITEM_TEST_RECORD:
        return "Battery Test";
    case APP_UI_SETTINGS_ITEM_FAN_CURVE:
        return "Fan Curve";
    case APP_UI_SETTINGS_ITEM_THERMAL_GUARD:
        return "Thermal Guard";
    case APP_UI_SETTINGS_ITEM_COUNT:
    default:
        return "Unknown";
    }
}

static void create_settings_row(lv_obj_t *parent, app_ui_settings_item_t item)
{
    settings_row_widgets_t *widgets = &settings_rows[item];
    const int32_t y_offset = UI_SETTINGS_ROW_TOP_PAD
        + (int32_t)item * (UI_SETTINGS_ROW_HEIGHT + UI_SETTINGS_ROW_GAP);

    widgets->row = lv_obj_create(parent);
    lv_obj_set_size(widgets->row, UI_SETTINGS_ROW_WIDTH,
                    UI_SETTINGS_ROW_HEIGHT);
    lv_obj_align(widgets->row, LV_ALIGN_TOP_MID, 0, y_offset);
    lv_obj_clear_flag(widgets->row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(widgets->row, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(widgets->row, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(widgets->row, 0, LV_PART_MAIN);

    widgets->title_label = lv_label_create(widgets->row);
    lv_label_set_text(widgets->title_label, settings_item_title(item));
    lv_obj_set_width(widgets->title_label, 118);
    lv_label_set_long_mode(widgets->title_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(widgets->title_label, LV_ALIGN_LEFT_MID, 10, 0);

    widgets->value_label = lv_label_create(widgets->row);
    lv_label_set_text(widgets->value_label, "--");
    lv_obj_set_width(widgets->value_label, 82);
    lv_label_set_long_mode(widgets->value_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(widgets->value_label, LV_TEXT_ALIGN_RIGHT,
                                LV_PART_MAIN);
    lv_obj_align(widgets->value_label, LV_ALIGN_RIGHT_MID, -10, 0);
}

static void create_settings_panel(lv_obj_t *screen)
{
    settings_panel = lv_obj_create(screen);
    lv_obj_set_pos(settings_panel, 0, UI_SETTINGS_PANEL_TOP);
    lv_obj_set_size(settings_panel, UI_SCREEN_WIDTH,
                    UI_SETTINGS_PANEL_HEIGHT);
    configure_plain_panel(settings_panel);
    lv_obj_add_flag(settings_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(settings_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(settings_panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(settings_panel, lv_color_hex(UI_COLOR_PANEL),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(settings_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(settings_panel, lv_color_hex(UI_COLOR_ACCENT_BLUE),
                              LV_PART_SCROLLBAR);

    for (app_ui_settings_item_t item = APP_UI_SETTINGS_ITEM_BACKLIGHT;
         item < APP_UI_SETTINGS_ITEM_COUNT;
         item = (app_ui_settings_item_t)(item + 1)) {
        create_settings_row(settings_panel, item);
    }

    lv_obj_add_flag(settings_panel, LV_OBJ_FLAG_HIDDEN);
}

static void create_ota_panel(lv_obj_t *screen)
{
    ota_panel = lv_obj_create(screen);
    lv_obj_set_pos(ota_panel, 0, UI_SETTINGS_PANEL_TOP);
    lv_obj_set_size(ota_panel, UI_SCREEN_WIDTH, UI_SETTINGS_PANEL_HEIGHT);
    configure_plain_panel(ota_panel);
    lv_obj_set_style_bg_color(ota_panel, lv_color_hex(UI_COLOR_PANEL),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ota_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(ota_panel, LV_OBJ_FLAG_SCROLLABLE);

    ota_line1_label = create_value_label(ota_panel, "OTA IDLE",
                                         LV_ALIGN_TOP_LEFT, 10, 8);
    lv_obj_set_width(ota_line1_label, 220);
    lv_obj_set_style_text_color(ota_line1_label,
                                lv_color_hex(UI_COLOR_LABEL_BLUE),
                                LV_PART_MAIN);
    ota_line2_label = create_value_label(ota_panel, "SSID --",
                                         LV_ALIGN_TOP_LEFT, 10, 48);
    lv_obj_set_width(ota_line2_label, 220);
    lv_obj_set_style_text_color(ota_line2_label,
                                lv_color_hex(UI_COLOR_LABEL_BLUE),
                                LV_PART_MAIN);
    ota_line3_label = create_value_label(ota_panel, "PASS --",
                                         LV_ALIGN_TOP_LEFT, 10, 88);
    lv_obj_set_width(ota_line3_label, 220);
    lv_obj_set_style_text_color(ota_line3_label,
                                lv_color_hex(UI_COLOR_LABEL_AMBER),
                                LV_PART_MAIN);
    ota_line4_label = create_value_label(ota_panel, "IP --",
                                         LV_ALIGN_TOP_LEFT, 10, 128);
    lv_obj_set_width(ota_line4_label, 220);
    lv_obj_set_style_text_color(ota_line4_label,
                                lv_color_hex(UI_COLOR_LABEL_GREEN),
                                LV_PART_MAIN);

    lv_obj_add_flag(ota_panel, LV_OBJ_FLAG_HIDDEN);
}

static void set_settings_visible(bool enabled)
{
    if (main_panel == NULL || settings_panel == NULL) {
        return;
    }
    if (enabled == settings_visible_rendered) {
        return;
    }

    if (enabled) {
        lv_obj_add_flag(main_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(settings_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(main_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(settings_panel, LV_OBJ_FLAG_HIDDEN);
    }
    settings_visible_rendered = enabled;
}

static void set_ota_visible(bool enabled)
{
    if (main_panel == NULL || settings_panel == NULL || ota_panel == NULL) {
        return;
    }
    if (enabled == ota_visible_rendered) {
        return;
    }

    if (enabled) {
        lv_obj_add_flag(main_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(settings_panel, LV_OBJ_FLAG_HIDDEN);
        settings_visible_rendered = false;
        lv_obj_clear_flag(ota_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ota_panel, LV_OBJ_FLAG_HIDDEN);
        if (!settings_visible_rendered) {
            lv_obj_clear_flag(main_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    ota_visible_rendered = enabled;
}

static void update_settings_row_style(app_ui_settings_item_t item,
                                      app_ui_settings_item_t selected)
{
    settings_row_widgets_t *widgets = &settings_rows[item];
    const bool active = item == selected;

    lv_obj_set_style_bg_color(widgets->row,
                              lv_color_hex(active ? UI_COLOR_ROW_SELECTED :
                                           UI_COLOR_ROW),
                              LV_PART_MAIN);
    lv_obj_set_style_border_color(widgets->row,
                                  lv_color_hex(active ?
                                               UI_COLOR_BORDER_SELECTED :
                                               UI_COLOR_BORDER),
                                  LV_PART_MAIN);
    lv_obj_set_style_text_color(widgets->title_label,
                                lv_color_hex(active ? UI_COLOR_TEXT_PRIMARY :
                                             UI_COLOR_TEXT_SECONDARY),
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(widgets->value_label,
                                lv_color_hex(active ? UI_COLOR_ACCENT_AMBER :
                                             UI_COLOR_TEXT_MUTED),
                                LV_PART_MAIN);
}

static void scroll_settings_row_into_view(app_ui_settings_item_t selected)
{
    if (selected >= APP_UI_SETTINGS_ITEM_COUNT
        || settings_panel == NULL
        || selected == last_scrolled_settings_item) {
        return;
    }

    const int32_t row_top = UI_SETTINGS_ROW_TOP_PAD
        + (int32_t)selected * (UI_SETTINGS_ROW_HEIGHT + UI_SETTINGS_ROW_GAP);
    const int32_t row_bottom = row_top + UI_SETTINGS_ROW_HEIGHT;
    const int32_t visible_top = lv_obj_get_scroll_y(settings_panel);
    const int32_t visible_bottom = visible_top + UI_SETTINGS_PANEL_HEIGHT;
    int32_t target_scroll_y = visible_top;

    if (row_top < visible_top) {
        target_scroll_y = row_top;
    } else if (row_bottom > visible_bottom) {
        target_scroll_y = row_bottom - UI_SETTINGS_PANEL_HEIGHT;
    }

    if (target_scroll_y < 0) {
        target_scroll_y = 0;
    }
    if (target_scroll_y != visible_top) {
        lv_obj_scroll_to_y(settings_panel, target_scroll_y, LV_ANIM_OFF);
    }

    last_scrolled_settings_item = selected;
}

static uint8_t move_u8_toward(uint8_t current, uint8_t target,
                              uint8_t step)
{
    if (current < target) {
        const uint8_t remaining = (uint8_t)(target - current);
        return current + (remaining < step ? remaining : step);
    }
    if (current > target) {
        const uint8_t remaining = (uint8_t)(current - target);
        return current - (remaining < step ? remaining : step);
    }
    return current;
}

static uint16_t move_u16_toward(uint16_t current, uint16_t target,
                                uint16_t step)
{
    if (current < target) {
        const uint16_t remaining = (uint16_t)(target - current);
        return current + (remaining < step ? remaining : step);
    }
    if (current > target) {
        const uint16_t remaining = (uint16_t)(current - target);
        return current - (remaining < step ? remaining : step);
    }
    return current;
}

static bool control_display_caught_up(const app_state_t *state)
{
    return displayed_brightness_percent == state->brightness_percent
           && displayed_cct_kelvin == state->cct_kelvin;
}

static void notify_ui_task(void)
{
    if (ui_task_handle != NULL) {
        xTaskNotifyGive(ui_task_handle);
    }
}

static bool advance_control_display(const app_state_t *state)
{
    const uint8_t previous_brightness = displayed_brightness_percent;
    const uint16_t previous_cct = displayed_cct_kelvin;

    const uint8_t brightness_step =
        UI_BRIGHTNESS_DISPLAY_STEP * UI_CONTROL_CATCH_UP_STEPS;
    const uint16_t cct_step =
        UI_CCT_DISPLAY_STEP_KELVIN * UI_CONTROL_CATCH_UP_STEPS;

    displayed_brightness_percent =
        move_u8_toward(displayed_brightness_percent,
                       state->brightness_percent, brightness_step);
    displayed_cct_kelvin =
        move_u16_toward(displayed_cct_kelvin, state->cct_kelvin, cct_step);

    return displayed_brightness_percent != previous_brightness
           || displayed_cct_kelvin != previous_cct;
}

static void create_status_screen(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(screen, lv_color_hex(UI_COLOR_TEXT_PRIMARY),
                                LV_PART_MAIN);

    battery_icon_body = lv_obj_create(screen);
    lv_obj_set_pos(battery_icon_body, UI_BATTERY_ICON_X, UI_BATTERY_ICON_Y);
    lv_obj_set_size(battery_icon_body, UI_BATTERY_BODY_WIDTH,
                    UI_BATTERY_BODY_HEIGHT);
    lv_obj_clear_flag(battery_icon_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(battery_icon_body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(battery_icon_body, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(battery_icon_body,
                                  lv_color_hex(UI_COLOR_TEXT_MUTED),
                                  LV_PART_MAIN);
    lv_obj_set_style_radius(battery_icon_body, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(battery_icon_body, 0, LV_PART_MAIN);

    battery_icon_fill = lv_obj_create(screen);
    lv_obj_set_pos(battery_icon_fill, UI_BATTERY_ICON_X + 3,
                   UI_BATTERY_ICON_Y + 3);
    lv_obj_set_size(battery_icon_fill, UI_BATTERY_FILL_MAX_WIDTH,
                    UI_BATTERY_FILL_HEIGHT);
    lv_obj_clear_flag(battery_icon_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(battery_icon_fill,
                              lv_color_hex(UI_COLOR_ACCENT_GREEN),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(battery_icon_fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(battery_icon_fill, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(battery_icon_fill, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(battery_icon_fill, 0, LV_PART_MAIN);
    lv_obj_add_flag(battery_icon_fill, LV_OBJ_FLAG_HIDDEN);

    battery_icon_cap = lv_obj_create(screen);
    lv_obj_set_pos(battery_icon_cap,
                   UI_BATTERY_ICON_X + UI_BATTERY_BODY_WIDTH + 1,
                   UI_BATTERY_ICON_Y +
                       ((UI_BATTERY_BODY_HEIGHT - UI_BATTERY_CAP_HEIGHT) / 2));
    lv_obj_set_size(battery_icon_cap, UI_BATTERY_CAP_WIDTH,
                    UI_BATTERY_CAP_HEIGHT);
    lv_obj_clear_flag(battery_icon_cap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(battery_icon_cap,
                              lv_color_hex(UI_COLOR_TEXT_MUTED),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(battery_icon_cap, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(battery_icon_cap, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(battery_icon_cap, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(battery_icon_cap, 0, LV_PART_MAIN);

    battery_label = create_value_label(screen, "--V --%", LV_ALIGN_TOP_LEFT,
                                       UI_BATTERY_LABEL_X,
                                       UI_BATTERY_LABEL_Y);
    lv_obj_set_width(battery_label, UI_BATTERY_LABEL_WIDTH);
    lv_obj_set_style_text_color(battery_label,
                                lv_color_hex(UI_COLOR_TEXT_MUTED),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(battery_label, LV_TEXT_ALIGN_LEFT,
                                LV_PART_MAIN);

    temperature_icon_stem = lv_obj_create(screen);
    lv_obj_set_pos(temperature_icon_stem, UI_TEMPERATURE_ICON_X + 8,
                   UI_TEMPERATURE_ICON_Y + 3);
    lv_obj_set_size(temperature_icon_stem, 11, 5);
    lv_obj_clear_flag(temperature_icon_stem, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(temperature_icon_stem,
                              lv_color_hex(UI_COLOR_TEXT_MUTED),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(temperature_icon_stem, LV_OPA_COVER,
                            LV_PART_MAIN);
    lv_obj_set_style_border_width(temperature_icon_stem, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(temperature_icon_stem, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(temperature_icon_stem, 0, LV_PART_MAIN);

    temperature_icon_bulb = lv_obj_create(screen);
    lv_obj_set_pos(temperature_icon_bulb, UI_TEMPERATURE_ICON_X,
                   UI_TEMPERATURE_ICON_Y);
    lv_obj_set_size(temperature_icon_bulb, 11, 11);
    lv_obj_clear_flag(temperature_icon_bulb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(temperature_icon_bulb,
                              lv_color_hex(UI_COLOR_TEXT_MUTED),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(temperature_icon_bulb, LV_OPA_COVER,
                            LV_PART_MAIN);
    lv_obj_set_style_border_width(temperature_icon_bulb, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(temperature_icon_bulb, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(temperature_icon_bulb, 0, LV_PART_MAIN);

    temperature_label = create_value_label(screen, "--.-C",
                                           LV_ALIGN_TOP_LEFT,
                                           UI_TEMPERATURE_LABEL_X,
                                           UI_TEMPERATURE_LABEL_Y);
    lv_obj_set_width(temperature_label, UI_TEMPERATURE_LABEL_WIDTH);
    lv_obj_set_style_text_align(temperature_label, LV_TEXT_ALIGN_RIGHT,
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(temperature_label,
                                lv_color_hex(UI_COLOR_TEXT_MUTED),
                                LV_PART_MAIN);

    status_card = lv_obj_create(screen);
    lv_obj_set_size(status_card, UI_STATUS_CARD_WIDTH,
                    UI_STATUS_CARD_HEIGHT);
    lv_obj_align(status_card, LV_ALIGN_TOP_MID, 0, UI_STATUS_CARD_Y);
    lv_obj_clear_flag(status_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(status_card,
                              lv_color_hex(UI_COLOR_STATUS_AMBER_BG),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(status_card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(status_card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(status_card,
                                  lv_color_hex(UI_COLOR_STATUS_AMBER_BORDER),
                                  LV_PART_MAIN);
    lv_obj_set_style_radius(status_card, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_all(status_card, 0, LV_PART_MAIN);

    status_dot = lv_obj_create(status_card);
    lv_obj_set_size(status_dot, UI_STATUS_DOT_SIZE, UI_STATUS_DOT_SIZE);
    lv_obj_align(status_dot, LV_ALIGN_LEFT_MID, UI_STATUS_DOT_X, 0);
    lv_obj_clear_flag(status_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(status_dot,
                              lv_color_hex(UI_COLOR_LABEL_AMBER),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(status_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(status_dot, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(status_dot, UI_STATUS_DOT_SIZE / 2,
                            LV_PART_MAIN);
    lv_obj_set_style_pad_all(status_dot, 0, LV_PART_MAIN);

    status_label = create_value_label(status_card, "CHECK",
                                      LV_ALIGN_LEFT_MID,
                                      UI_STATUS_LABEL_X, 0);
    lv_obj_set_width(status_label, UI_STATUS_LABEL_WIDTH);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_LEFT,
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label,
                                lv_color_hex(UI_COLOR_LABEL_AMBER),
                                LV_PART_MAIN);

    main_panel = lv_obj_create(screen);
    lv_obj_set_size(main_panel, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    configure_plain_panel(main_panel);
    lv_obj_clear_flag(main_panel, LV_OBJ_FLAG_SCROLLABLE);

    brightness_label = create_value_label(main_panel, "Brightness 50%",
                                          LV_ALIGN_TOP_LEFT,
                                          UI_MAIN_CONTROL_LABEL_X,
                                          UI_MAIN_BRIGHTNESS_LABEL_Y);
    lv_obj_set_width(brightness_label, UI_MAIN_CONTROL_LABEL_WIDTH);
    lv_obj_set_style_text_color(brightness_label,
                                lv_color_hex(UI_COLOR_LABEL_BLUE),
                                LV_PART_MAIN);
    brightness_bar = create_bar(main_panel, APP_STATE_BRIGHTNESS_MIN_PERCENT,
                                APP_STATE_BRIGHTNESS_MAX_PERCENT,
                                APP_STATE_BRIGHTNESS_DEFAULT_PERCENT,
                                UI_MAIN_BRIGHTNESS_BAR_Y,
                                lv_color_hex(UI_COLOR_ACCENT_BLUE));

    cct_label = create_value_label(main_panel, "CCT       5100K",
                                   LV_ALIGN_TOP_LEFT,
                                   UI_MAIN_CONTROL_LABEL_X,
                                   UI_MAIN_CCT_LABEL_Y);
    lv_obj_set_width(cct_label, UI_MAIN_CONTROL_LABEL_WIDTH);
    lv_obj_set_style_text_color(cct_label,
                                lv_color_hex(UI_COLOR_LABEL_AMBER),
                                LV_PART_MAIN);
    cct_bar = create_bar(main_panel, APP_STATE_CCT_MIN_KELVIN,
                         APP_STATE_CCT_MAX_KELVIN,
                         APP_STATE_CCT_DEFAULT_KELVIN, UI_MAIN_CCT_BAR_Y,
                         lv_color_hex(UI_COLOR_ACCENT_AMBER));

    create_settings_panel(screen);
    create_ota_panel(screen);
}

static const char *ota_status_text(app_ota_status_t status)
{
    switch (status) {
    case APP_OTA_STATUS_STARTING:
        return "STARTING";
    case APP_OTA_STATUS_READY:
        return "READY";
    case APP_OTA_STATUS_UPLOADING:
        return "UPLOADING";
    case APP_OTA_STATUS_VERIFYING:
        return "VERIFYING";
    case APP_OTA_STATUS_SUCCESS:
        return "REBOOTING";
    case APP_OTA_STATUS_FAILED:
        return "FAILED";
    case APP_OTA_STATUS_IDLE:
    default:
        return "IDLE";
    }
}

static void update_status_widget(const app_state_t *state)
{
    char message[UI_MESSAGE_TEXT_MAX_LEN];
    if (copy_transient_message(message, sizeof(message))) {
        set_status_text(message, UI_COLOR_LABEL_AMBER);
        return;
    }

    if (state->fault != SYSTEM_FAULT_NONE) {
        set_status_text(fault_name(state->fault), UI_COLOR_LABEL_DANGER);
    } else if (!isfinite(state->battery_voltage_v)
               || !isfinite(state->ntc_temp_c)) {
        set_status_text("CHECKING SENSORS", UI_COLOR_LABEL_AMBER);
    } else if (state->light_enabled) {
        set_status_text("LIGHT ON", UI_COLOR_LABEL_BLUE);
    } else {
        set_status_text("READY", UI_COLOR_LABEL_GREEN);
    }
}

static void update_settings_widgets(const app_state_t *state,
                                    app_ui_settings_item_t selected)
{
    set_ota_visible(false);
    set_settings_visible(true);

    char message[UI_MESSAGE_TEXT_MAX_LEN];
    if (copy_transient_message(message, sizeof(message))) {
        set_status_text(message, UI_COLOR_LABEL_AMBER);
    } else {
        set_status_text("SETTINGS", UI_COLOR_LABEL_AMBER);
    }

    const uint8_t backlight_percent = app_settings_get_backlight_percent();
    const uint16_t cct_step_kelvin = app_settings_get_cct_step_kelvin();
    lv_obj_t *backlight_value =
        settings_rows[APP_UI_SETTINGS_ITEM_BACKLIGHT].value_label;
    lv_obj_t *cct_step_value =
        settings_rows[APP_UI_SETTINGS_ITEM_CCT_STEP].value_label;
    lv_obj_t *fan_manual_value =
        settings_rows[APP_UI_SETTINGS_ITEM_FAN_MANUAL].value_label;
    lv_obj_t *wifi_ota_value =
        settings_rows[APP_UI_SETTINGS_ITEM_WIFI_OTA].value_label;
    lv_obj_t *test_record_value =
        settings_rows[APP_UI_SETTINGS_ITEM_TEST_RECORD].value_label;
    lv_obj_t *fan_curve_value =
        settings_rows[APP_UI_SETTINGS_ITEM_FAN_CURVE].value_label;
    lv_obj_t *thermal_guard_value =
        settings_rows[APP_UI_SETTINGS_ITEM_THERMAL_GUARD].value_label;

    lv_label_set_text_fmt(backlight_value, "%u%%",
                          (unsigned int)backlight_percent);
    lv_label_set_text_fmt(cct_step_value, "%uK",
                          (unsigned int)cct_step_kelvin);
    lv_label_set_text(fan_manual_value,
                      state->manual_fan_enabled ? "ON" : "OFF");
    lv_label_set_text(wifi_ota_value, state->ota_active ? "OPEN" : "START");
    app_battery_test_status_t test_status;
    if (app_battery_test_get_status(&test_status) == ESP_OK) {
        const char *test_state = "STOP";
        if (test_status.run_state == APP_BATTERY_TEST_RUNNING) {
            test_state = "RUN";
        } else if (test_status.run_state == APP_BATTERY_TEST_PAUSED) {
            test_state = "PAUSE";
        } else if (test_status.run_state == APP_BATTERY_TEST_COMPLETE) {
            test_state = "DONE";
        }
        lv_label_set_text_fmt(test_record_value, "%s %u", test_state,
                              (unsigned int)test_status.sample_count);
    } else {
        lv_label_set_text(test_record_value, "ERROR");
    }
    lv_label_set_text_fmt(fan_curve_value, "AUTO %u%%",
                          (unsigned int)state->fan_percent);
    lv_label_set_text(thermal_guard_value,
                      state->over_temperature ? "ACTIVE" : "OFF >65C");

    if (selected != last_styled_settings_item) {
        for (app_ui_settings_item_t item = APP_UI_SETTINGS_ITEM_BACKLIGHT;
             item < APP_UI_SETTINGS_ITEM_COUNT;
             item = (app_ui_settings_item_t)(item + 1)) {
            update_settings_row_style(item, selected);
        }
        last_styled_settings_item = selected;
    }
    scroll_settings_row_into_view(selected);
}

static void update_ota_widgets(const app_state_t *state)
{
    set_settings_visible(false);
    set_ota_visible(true);

    set_status_text("WIFI OTA", UI_COLOR_LABEL_AMBER);

    if (state->ota_status == APP_OTA_STATUS_UPLOADING
        || state->ota_status == APP_OTA_STATUS_VERIFYING
        || state->ota_status == APP_OTA_STATUS_SUCCESS) {
        lv_label_set_text_fmt(ota_line1_label, "%s %u%%",
                              ota_status_text(state->ota_status),
                              (unsigned int)state->ota_progress_percent);
    } else {
        lv_label_set_text_fmt(ota_line1_label, "OTA %s",
                              ota_status_text(state->ota_status));
    }

    if (state->ota_status == APP_OTA_STATUS_FAILED) {
        lv_obj_set_style_text_color(ota_line2_label,
                                    lv_color_hex(UI_COLOR_LABEL_DANGER),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_color(ota_line3_label,
                                    lv_color_hex(UI_COLOR_LABEL_AMBER),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_color(ota_line4_label,
                                    lv_color_hex(UI_COLOR_LABEL_GREEN),
                                    LV_PART_MAIN);
        lv_label_set_text_fmt(ota_line2_label, "ERR %s", state->ota_error);
        lv_label_set_text(ota_line3_label, "SW2 EXIT");
        lv_label_set_text(ota_line4_label, "");
        return;
    }

    lv_obj_set_style_text_color(ota_line2_label,
                                lv_color_hex(UI_COLOR_LABEL_BLUE),
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(ota_line3_label,
                                lv_color_hex(UI_COLOR_LABEL_AMBER),
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(ota_line4_label,
                                lv_color_hex(UI_COLOR_LABEL_GREEN),
                                LV_PART_MAIN);
    lv_label_set_text_fmt(ota_line2_label, "SSID %s", state->ota_ssid);
    lv_label_set_text_fmt(ota_line3_label, "PASS %s", state->ota_password);
    lv_label_set_text_fmt(ota_line4_label, "OPEN %s", state->ota_ip);
}

static bool update_main_widgets(const app_state_t *state)
{
    set_ota_visible(false);
    set_settings_visible(false);

    if (!main_widgets_rendered) {
        displayed_brightness_percent = state->brightness_percent;
        displayed_cct_kelvin = state->cct_kelvin;
    } else {
        (void)advance_control_display(state);
    }

    if (!main_widgets_rendered
        || last_rendered_brightness_percent != displayed_brightness_percent) {
        lv_label_set_text_fmt(brightness_label, "Brightness %u%%",
                              (unsigned int)displayed_brightness_percent);
        lv_bar_set_range(brightness_bar, APP_STATE_BRIGHTNESS_MIN_PERCENT,
                         APP_STATE_BRIGHTNESS_MAX_PERCENT);
        lv_bar_set_value(brightness_bar, displayed_brightness_percent,
                         LV_ANIM_OFF);
        last_rendered_brightness_percent = displayed_brightness_percent;
    }

    if (!main_widgets_rendered
        || last_rendered_cct_kelvin != displayed_cct_kelvin) {
        lv_label_set_text_fmt(cct_label, "CCT       %uK",
                              (unsigned int)displayed_cct_kelvin);
        lv_bar_set_range(cct_bar, APP_STATE_CCT_MIN_KELVIN,
                         APP_STATE_CCT_MAX_KELVIN);
        lv_bar_set_value(cct_bar, displayed_cct_kelvin, LV_ANIM_OFF);
        last_rendered_cct_kelvin = displayed_cct_kelvin;
    }

    update_status_widget(state);
    main_widgets_rendered = true;
    return !control_display_caught_up(state);
}

static bool refresh_state_widgets(void)
{
    app_state_t state;
    esp_err_t err = app_state_get(&state);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read application state: %s",
                 esp_err_to_name(err));
        return false;
    }

    if (!hal_display_lock(UI_LOCK_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "Timed out waiting for LVGL lock");
        return false;
    }

    bool control_refresh_needed = false;

    if (isfinite(state.battery_voltage_v)) {
        set_battery_label(state.battery_voltage_v, state.battery_percent);
    } else {
        lv_label_set_text(battery_label, "--V --%");
        lv_obj_set_style_text_color(battery_label,
                                    lv_color_hex(UI_COLOR_TEXT_MUTED),
                                    LV_PART_MAIN);
        set_battery_icon(0, false);
    }

    if (isfinite(state.ntc_temp_c)) {
        set_temperature_label(state.ntc_temp_c);
    } else {
        lv_label_set_text(temperature_label, "--.-C");
        lv_obj_set_style_text_color(temperature_label,
                                    lv_color_hex(UI_COLOR_TEXT_MUTED),
                                    LV_PART_MAIN);
        set_temperature_icon(0.0f, false);
    }

    app_ui_settings_item_t selected;
    if (state.ota_active) {
        update_ota_widgets(&state);
    } else if (read_settings_page_state(&selected)) {
        update_settings_widgets(&state, selected);
    } else {
        control_refresh_needed = update_main_widgets(&state);
    }

    lv_display_t *display = hal_display_get_lvgl_display();
    if (display != NULL) {
        lv_refr_now(display);
    }

    hal_display_unlock();
    return control_refresh_needed;
}

static void ui_task(void *arg)
{
    (void)arg;

    while (true) {
        const bool control_refresh_needed = refresh_state_widgets();
        const uint32_t wait_ms = control_refresh_needed ?
                                 UI_CONTROL_REFRESH_MS :
                                 UI_REFRESH_PERIOD_MS;
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

esp_err_t app_ui_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    if (hal_display_get_lvgl_display() == NULL) {
        ESP_LOGE(TAG, "Display HAL is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    message_mutex = xSemaphoreCreateMutex();
    if (message_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create UI message mutex");
        return ESP_ERR_NO_MEM;
    }

    if (!hal_display_lock(UINT32_MAX)) {
        ESP_LOGE(TAG, "Failed to lock LVGL while creating status screen");
        return ESP_ERR_TIMEOUT;
    }
    create_status_screen();
    hal_display_unlock();

    BaseType_t task_created = xTaskCreate(ui_task, "app_ui",
                                          UI_TASK_STACK_SIZE, NULL,
                                          UI_TASK_PRIORITY, &ui_task_handle);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UI update task");
        ui_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    initialized = true;
    ESP_LOGI(TAG, "LVGL final-integration status screen started");
    return ESP_OK;
}

esp_err_t app_ui_request_refresh(void)
{
    if (!initialized || ui_task_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    notify_ui_task();
    return ESP_OK;
}

esp_err_t app_ui_show_message(const char *message, uint32_t duration_ms)
{
    if (message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!initialized || message_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(message_mutex,
                       pdMS_TO_TICKS(UI_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    (void)snprintf(transient_message, sizeof(transient_message), "%s",
                   message);
    transient_message_until_us = duration_ms == 0 ?
                                 0 :
                                 esp_timer_get_time()
                                 + (int64_t)duration_ms * 1000;

    xSemaphoreGive(message_mutex);
    notify_ui_task();
    return ESP_OK;
}

esp_err_t app_ui_set_settings_page(bool enabled)
{
    if (!initialized || message_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(message_mutex,
                       pdMS_TO_TICKS(UI_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    settings_page_active = enabled;
    if (enabled) {
        selected_settings_item = APP_UI_SETTINGS_ITEM_BACKLIGHT;
        last_styled_settings_item = APP_UI_SETTINGS_ITEM_COUNT;
        last_scrolled_settings_item = APP_UI_SETTINGS_ITEM_COUNT;
    }
    transient_message_until_us = 0;

    xSemaphoreGive(message_mutex);
    notify_ui_task();
    return ESP_OK;
}

bool app_ui_settings_page_active(void)
{
    return read_settings_page_state(NULL);
}

esp_err_t app_ui_settings_move_selection(int32_t delta)
{
    if (!initialized || message_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(message_mutex,
                       pdMS_TO_TICKS(UI_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    int32_t next = (int32_t)selected_settings_item + delta;
    const int32_t count = (int32_t)APP_UI_SETTINGS_ITEM_COUNT;
    while (next < 0) {
        next += count;
    }
    while (next >= count) {
        next -= count;
    }
    selected_settings_item = (app_ui_settings_item_t)next;

    xSemaphoreGive(message_mutex);
    notify_ui_task();
    return ESP_OK;
}

app_ui_settings_item_t app_ui_settings_selected_item(void)
{
    app_ui_settings_item_t selected;
    (void)read_settings_page_state(&selected);
    return selected;
}
