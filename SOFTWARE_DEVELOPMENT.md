# 25W 补光灯 ESP32-S3 固件开发文档

文件名建议：`SOFTWARE_DEVELOPMENT.md`  
目标读者：Codex / 固件开发者 / 硬件联调人员  
目标平台：ESP32-S3，ESP-IDF，LVGL  
硬件版本依据：2026-06-01 版 25W 补光灯电路

---

## 1. 文档目的

本文档定义 25W 补光灯在 ESP32-S3 平台上的固件架构、模块边界、硬件映射、控制算法、任务划分、安全策略和开发验收标准。

Codex 或开发者应按本文档直接创建或维护固件工程。本文档是软件开发规格，不是硬件说明书；涉及硬件参数时，仅列出固件必须感知的信号、算法和限制。

开发原则：

1. 使用 ESP-IDF，不使用 Arduino API。
2. 使用组件化目录结构，应用层不得直接操作 GPIO、PWM、ADC、SPI。
3. 所有硬件相关代码集中在 HAL 和 board 层。
4. UI、输入、LED、风扇、电池、温度、安全保护之间通过状态对象和事件队列交互。
5. 上电默认所有功率输出关闭，初始化完成并通过安全检查后才允许打开 LED。
6. 任何 ADC、NTC、低压、过温异常都必须进入降额或保护状态。

---

## 2. 固件目标功能

第一版固件必须实现：

| 功能 | 要求 |
|---|---|
| 电源保持 | 开机后立即拉起 `PWR_EN`，避免系统掉电 |
| 安全关机 | 长按电源键或低压/过温保护触发时执行有序关机 |
| LCD 显示 | SPI LCD + LVGL UI，显示亮度、色温、电量、温度、状态 |
| 背光控制 | 支持 LCD 背光开关和亮度 PWM 控制 |
| 冷暖光控制 | 两路 PWM 控制 `LED_C` 与 `LED_W` |
| 亮度控制 | 1%~100%，支持旋钮调节 |
| 色温控制 | 2700K~6500K，支持旋钮调节 |
| 电池检测 | 2S 电池包电压检测、百分比估算、低压保护 |
| 温度检测 | 10k B3950 NTC 温度检测 |
| 风扇控制 | 根据温度自动调速，异常时强制满速 |
| 输入处理 | 双旋钮、按键、长按、短按、消抖 |
| 参数保存 | 使用 NVS 保存亮度、色温、开关状态等设置 |
| 开机 Logo | 默认显示固件内置 Logo |
| 调试日志 | 串口输出关键状态和错误信息 |

第一版默认不实现：

1. RGB 彩色灯效。
2. BLE / Wi-Fi 配网。
3. OTA。
4. 充电状态精确识别，除非后续确认充电状态脚已接入 MCU。
5. 触摸屏，除非后续 LCD 模组带触摸且有对应连线。

---

## 3. 硬件功能摘要

固件相关硬件模块如下：

| 模块 | 器件/网络 | 软件职责 |
|---|---|---|
| 主控 | ESP32-S3 模组 U8 | 运行 ESP-IDF 固件 |
| USB-C 输入 | Type-C + CH224K | 固件通常无需控制，仅可选检测 VBUS |
| 2S 充电 | TP5100 | 固件只处理电池电压；充电状态默认未知 |
| 电源路径 | TPS2121 / MOS 电源切换 | 固件通过 `PWR_EN` 做电源保持 |
| 5V DCDC | TPS54202DDCR | 供 LCD/风扇/外设；固件无需控制 |
| 3.3V LDO | SPX3819M5-L-3-3/TR | 供 MCU/逻辑；固件无需控制 |
| 冷光 LED | Hi6000B + MOS + 电感 + 采样电阻 | `LEDC_PWM` 输出 PWM 调光 |
| 暖光 LED | Hi6000B + MOS + 电感 + 采样电阻 | `LEDW_PWM` 输出 PWM 调光 |
| 风扇 | 5V 风扇 + AO3400A 低边开关 | `FAN_PWM` 输出 PWM 调速 |
| 电池采样 | `BAT_ADC`，300k/100k 分压 | ADC 读取 2S 电池包电压 |
| 温度采样 | `NTC`，10k B3950 NTC | ADC 读取并换算温度 |
| 屏幕 | SPI LCD FPC | `esp_lcd` + LVGL 显示 |
| 输入 | `SW1_A/B`、`SW2_A/B`、`SW2_DOWN`、`EC_KEY_DET` | 编码器与按键事件 |
| 调试/下载 | `TXD`、`RXD`、`BOOT`、`EN` | 烧录和日志 |

---

## 4. GPIO 与信号映射

所有引脚定义必须集中放在：

```text
components/board/include/board_pins.h
```

业务层禁止直接写 `GPIO_NUM_x`，必须使用 `BOARD_GPIO_*` 宏。

### 4.1 当前电路建议映射

以下映射按当前电路图网络与 U8 引脚标注整理。若 PCB 网表、模组封装或后续原理图存在差异，以最终硬件文件为准。

| 功能 | 网络名 | ESP32-S3 GPIO / 信号 | 方向 | 备注 |
|---|---|---:|---|---|
| 旋钮 1 A 相 | `SW1_A` | GPIO4 | 输入 | 建议上拉，编码器输入 |
| 旋钮 1 B 相 | `SW1_B` | GPIO5 | 输入 | 建议上拉，编码器输入 |
| 电源键检测 | `EC_KEY_DET` | GPIO6 | 输入 | 需要消抖，支持长按 |
| 电源保持 | `PWR_EN` | GPIO18 | 输出 | 上电后尽早拉高 |
| USB D- | `D-` | GPIO19 | USB | Native USB D- |
| USB D+ | `D+` | GPIO20 | USB | Native USB D+ |
| NTC ADC | `NTC` | GPIO3 / ADC1_CH2 | 模拟输入 | 10k B3950 NTC |
| 电池 ADC | `BAT_ADC` | GPIO9 / ADC1_CH8 | 模拟输入 | 300k/100k 分压 |
| LCD RST | `LCD_RST` | GPIO10 | 输出 | LCD 复位 |
| LCD SCK | `LCD_SCK` | GPIO11 | 输出 | SPI 时钟 |
| LCD MOSI | `LCD_MOSI` | GPIO12 | 输出 | SPI 数据 |
| LCD DC | `LCD_DC` | GPIO13 | 输出 | LCD 命令/数据 |
| LCD CS | `LCD_CS` | GPIO14 | 输出 | LCD 片选 |
| LCD 背光 | `LCD_BACKLIGHT` | GPIO21 | 输出/PWM | 背光控制 |
| 旋钮 2 B 相 | `SW2_B` | GPIO1 | 输入 | 编码器输入 |
| 旋钮 2 A 相 | `SW2_A` | GPIO2 | 输入 | 编码器输入 |
| UART TX | `TXD` | TXD0 | 输出 | 下载/日志 |
| UART RX | `RXD` | RXD0 | 输入 | 下载/日志 |
| 旋钮 2 按下 | `SW2_DOWN` | GPIO42 | 输入 | 短按/长按 |
| 暖光 PWM | `LEDW_PWM` | GPIO40 | 输出/PWM | LEDC PWM；当前硬件已按固件映射调整 |
| 冷光 PWM | `LEDC_PWM` | GPIO38 | 输出/PWM | LEDC PWM；注意名称不是 ESP-IDF LEDC 外设名 |
| BOOT | `BOOT` | GPIO0 | 输入 | 下载模式，不作为普通按键 |
| 风扇 PWM | `FAN_PWM` | GPIO45 | 输出/PWM | LEDC PWM；注意启动绑带风险 |

### 4.2 board_pins.h 模板

```c
#pragma once

#include "driver/gpio.h"
#include "hal/adc_types.h"

// Power
#define BOARD_GPIO_PWR_EN            GPIO_NUM_18
#define BOARD_GPIO_EC_KEY_DET        GPIO_NUM_6

// Native USB
#define BOARD_GPIO_USB_DM            GPIO_NUM_19
#define BOARD_GPIO_USB_DP            GPIO_NUM_20

// Encoders / buttons
#define BOARD_GPIO_SW1_A             GPIO_NUM_4
#define BOARD_GPIO_SW1_B             GPIO_NUM_5
#define BOARD_GPIO_SW2_A             GPIO_NUM_2
#define BOARD_GPIO_SW2_B             GPIO_NUM_1
#define BOARD_GPIO_SW2_DOWN          GPIO_NUM_42

// LED outputs
#define BOARD_GPIO_LED_C_PWM         GPIO_NUM_38
#define BOARD_GPIO_LED_W_PWM         GPIO_NUM_40

// Fan
#define BOARD_GPIO_FAN_PWM           GPIO_NUM_45

// LCD SPI
#define BOARD_GPIO_LCD_SCK           GPIO_NUM_11
#define BOARD_GPIO_LCD_MOSI          GPIO_NUM_12
#define BOARD_GPIO_LCD_CS            GPIO_NUM_14
#define BOARD_GPIO_LCD_DC            GPIO_NUM_13
#define BOARD_GPIO_LCD_RST           GPIO_NUM_10
#define BOARD_GPIO_LCD_BACKLIGHT     GPIO_NUM_21

// ADC
#define BOARD_GPIO_NTC_ADC           GPIO_NUM_3
#define BOARD_GPIO_BAT_ADC           GPIO_NUM_9
#define BOARD_ADC_NTC_CHANNEL        ADC_CHANNEL_2
#define BOARD_ADC_BAT_CHANNEL        ADC_CHANNEL_8

// Boot / debug
#define BOARD_GPIO_BOOT              GPIO_NUM_0
```

### 4.3 引脚风险提示

1. `GPIO0` 是 BOOT 启动相关引脚，不得作为普通 UI 输入使用。
2. `GPIO45` 属于敏感启动相关引脚，当前用于 `FAN_PWM`。固件必须保证启动阶段不会主动拉到影响启动模式的状态；硬件也应确保外部电路不会改变启动配置。
3. `GPIO19/GPIO20` 用于 USB D-/D+，不要配置为普通 GPIO。
4. `GPIO3`、`GPIO9` 用作 ADC，禁止复用为数字输出。
5. LCD SPI 与 LED/FAN PWM 初始化前，应确保所有功率输出为安全状态。

---

## 5. 推荐工程结构

工程根目录建议如下：

```text
led-light-fw/
├── CMakeLists.txt
├── sdkconfig.defaults
├── idf_component.yml
├── SOFTWARE_DEVELOPMENT.md
├── main/
│   ├── CMakeLists.txt
│   └── app_main.c
└── components/
    ├── board/
    │   ├── CMakeLists.txt
    │   ├── include/board_pins.h
    │   └── board_pins.c
    ├── hal_power/
    │   ├── CMakeLists.txt
    │   ├── include/hal_power.h
    │   └── hal_power.c
    ├── hal_led/
    │   ├── CMakeLists.txt
    │   ├── include/hal_led.h
    │   └── hal_led.c
    ├── hal_fan/
    │   ├── CMakeLists.txt
    │   ├── include/hal_fan.h
    │   └── hal_fan.c
    ├── hal_adc/
    │   ├── CMakeLists.txt
    │   ├── include/hal_adc.h
    │   └── hal_adc.c
    ├── hal_display/
    │   ├── CMakeLists.txt
    │   ├── include/hal_display.h
    │   └── hal_display.c
    ├── hal_input/
    │   ├── CMakeLists.txt
    │   ├── include/hal_input.h
    │   └── hal_input.c
    ├── app_state/
    │   ├── CMakeLists.txt
    │   ├── include/app_state.h
    │   └── app_state.c
    ├── app_light/
    │   ├── CMakeLists.txt
    │   ├── include/app_light.h
    │   └── app_light.c
    ├── app_sensor/
    │   ├── CMakeLists.txt
    │   ├── include/app_sensor.h
    │   └── app_sensor.c
    ├── app_safety/
    │   ├── CMakeLists.txt
    │   ├── include/app_safety.h
    │   └── app_safety.c
    ├── app_input/
    │   ├── CMakeLists.txt
    │   ├── include/app_input.h
    │   └── app_input.c
    ├── app_ui/
    │   ├── CMakeLists.txt
    │   ├── include/app_ui.h
    │   └── app_ui.c
    └── app_settings/
        ├── CMakeLists.txt
        ├── include/app_settings.h
        └── app_settings.c
```

---

## 6. 分层架构

### 6.1 分层说明

```text
Application Layer
  app_ui
  app_light
  app_input
  app_sensor
  app_safety
  app_settings
  app_state

HAL Layer
  hal_display
  hal_led
  hal_fan
  hal_adc
  hal_input
  hal_power

Board Layer
  board_pins
```

### 6.2 依赖方向

允许：

```text
app_* -> hal_*
app_* -> app_state
hal_* -> board
```

禁止：

```text
hal_* -> app_ui
hal_* -> app_light
app_ui -> driver/gpio.h
app_ui -> driver/ledc.h
app_ui -> driver/adc_oneshot.h
app_light -> driver/ledc.h
app_sensor -> driver/adc_oneshot.h
```

业务层不允许直接依赖 ESP-IDF 外设驱动头文件，除非该模块本身就是 HAL。

---

## 7. app_main 启动流程

`main/app_main.c` 必须按以下顺序启动：

```text
1. 初始化日志等级
2. 初始化 board 层
3. 初始化 hal_power
4. 立即设置 PWR_EN = true
5. 初始化 NVS
6. 加载 app_settings
7. 初始化 app_state
8. 初始化 hal_led，并保持 LED 关闭
9. 初始化 hal_fan，并保持风扇默认安全状态
10. 初始化 hal_adc
11. 初始化 hal_input
12. 初始化 hal_display / LVGL / UI
13. 创建 input_task
14. 创建 sensor_task
15. 创建 safety_task
16. 创建 ui_task 或启动 LVGL port
17. 根据保存参数恢复 LED 状态，但必须先通过安全检查
```

伪代码：

```c
void app_main(void)
{
    board_init();

    ESP_ERROR_CHECK(hal_power_init());
    ESP_ERROR_CHECK(hal_power_hold(true));

    ESP_ERROR_CHECK(app_settings_init());
    ESP_ERROR_CHECK(app_state_init());

    ESP_ERROR_CHECK(hal_led_init());
    ESP_ERROR_CHECK(hal_led_off());

    ESP_ERROR_CHECK(hal_fan_init());
    ESP_ERROR_CHECK(hal_fan_set_percent(0));

    ESP_ERROR_CHECK(hal_adc_init());
    ESP_ERROR_CHECK(hal_input_init());
    ESP_ERROR_CHECK(hal_display_init());

    ESP_ERROR_CHECK(app_light_init());
    ESP_ERROR_CHECK(app_sensor_init());
    ESP_ERROR_CHECK(app_safety_init());
    ESP_ERROR_CHECK(app_input_init());
    ESP_ERROR_CHECK(app_ui_init());

    app_tasks_start();
}
```

---

## 8. 全局状态模型

状态统一由 `app_state` 管理，不允许 UI、输入、传感器、LED 控制各自维护互相冲突的全局变量。

```c
typedef enum {
    CHARGE_STATE_UNKNOWN = 0,
    CHARGE_STATE_NOT_CHARGING,
    CHARGE_STATE_CHARGING,
    CHARGE_STATE_FULL,
} charge_state_t;

typedef enum {
    SYSTEM_FAULT_NONE = 0,
    SYSTEM_FAULT_LOW_BATTERY,
    SYSTEM_FAULT_CRITICAL_BATTERY,
    SYSTEM_FAULT_OVER_TEMP,
    SYSTEM_FAULT_NTC_ERROR,
    SYSTEM_FAULT_ADC_ERROR,
} system_fault_t;

typedef struct {
    bool light_enabled;
    uint8_t brightness_percent;     // 1~100
    uint16_t cct_kelvin;            // 2700~6500

    float battery_voltage_v;
    uint8_t battery_percent;
    charge_state_t charge_state;

    float ntc_temp_c;
    uint8_t fan_percent;
    bool manual_fan_enabled;

    bool low_battery;
    bool critical_battery;
    bool over_temperature;
    bool display_on;

    system_fault_t fault;
} app_state_t;
```

状态读写要求：

1. 使用 mutex 保护共享状态，或使用单写者事件模型。
2. UI 只读取状态并渲染，不直接改变硬件。
3. 输入事件修改状态后，通过 app_light / app_safety 决定硬件输出。
4. safety 状态优先级高于用户输入。

---

## 9. 事件模型

### 9.1 输入事件

```c
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
```

### 9.2 系统事件

```c
typedef enum {
    APP_EVENT_INPUT = 0,
    APP_EVENT_SENSOR_UPDATED,
    APP_EVENT_LIGHT_REQUEST,
    APP_EVENT_SAFETY_CHANGED,
    APP_EVENT_SETTINGS_SAVE_REQUEST,
    APP_EVENT_SHUTDOWN_REQUEST,
} app_event_type_t;
```

建议使用 FreeRTOS queue 或 ESP event loop。第一版使用 FreeRTOS queue 更简单。

---

## 10. 输入交互定义

| 输入 | 默认功能 |
|---|---|
| SW1 顺时针 | 亮度 +1% 或 +5%，按 UI 设置决定 |
| SW1 逆时针 | 亮度 -1% 或 -5% |
| SW2 顺时针 | 色温 +100K |
| SW2 逆时针 | 色温 -100K |
| SW2 短按 | 开关 LED |
| SW2 长按 | 进入/退出设置页 |
| EC_KEY 短按 | 亮屏/息屏，不改变 LED 开关状态 |
| EC_KEY 长按 | 显示松手关机提示，松开后执行安全关机 |
| BOOT | 仅用于下载模式，不进入 UI 输入系统 |

设置页第一版交互：

| 输入 | 设置页功能 |
|---|---|
| SW1 顺/逆时针 | 调整当前设置项 |
| SW2 顺/逆时针 | 选择设置项 |
| SW2 短按 | 退出设置页 |
| SW2 长按 | 进入/退出设置页 |

设置页布局要求：

1. 采用传统纵向列表，每个设置项占一行，左侧显示名称，右侧显示当前值或状态。
2. 当前选中项必须有明显高亮，不依赖文字说明才能判断焦点。
3. 设置项数量超过可视区域时，列表必须支持上下滚动，并自动把当前选中项滚动到可见范围内。

设置项规划：

| 设置项 | 第一版行为 |
|---|---|
| Backlight | 可调 10%~100%，延迟保存到 NVS |
| Fan Manual | 可设 OFF/ON；ON 时强制风扇 100%，OFF 时恢复自动温控 |
| Fan Curve | 只读显示自动风扇策略，避免用户误改安全曲线 |
| Thermal Guard | 只读显示过温保护策略，避免用户误关安全保护 |

后续可评估增加：语言/单位、恢复默认、风扇最低启动占空比、
屏幕方向、隐藏硬件测试页入口。

消抖要求：

1. 普通按键消抖时间：20ms~50ms。
2. 长按时间：1500ms~2500ms，推荐 2000ms。
3. 编码器使用 PCNT 或 GPIO ISR + 状态机，禁止在 ISR 内做复杂逻辑。
4. ISR 中只记录边沿或发送轻量事件，不调用 LVGL、不调用 NVS、不做浮点计算。

---

## 11. LED 冷暖光控制

### 11.1 硬件输出

| 通道 | 网络 | GPIO | 外设 |
|---|---|---:|---|
| 冷光 | `LEDC_PWM` | GPIO38 | LEDC PWM |
| 暖光 | `LEDW_PWM` | GPIO40 | LEDC PWM |

### 11.2 HAL 接口

```c
#pragma once

#include <stdint.h>
#include "esp_err.h"

#define HAL_LED_DUTY_MAX 4095

typedef struct {
    uint32_t pwm_freq_hz;
    uint8_t duty_resolution_bits;
} hal_led_config_t;

esp_err_t hal_led_init(void);
esp_err_t hal_led_set_cw(uint16_t cold_duty, uint16_t warm_duty);
esp_err_t hal_led_off(void);
```

### 11.3 PWM 参数

| 参数 | 推荐值 |
|---|---:|
| PWM 外设 | LEDC |
| 频率 | 19531 Hz（约 20 kHz） |
| 分辨率 | 12 bit |
| duty 最大值 | 4095 |
| 上电默认 | 0 |
| 故障默认 | 0 |

如果 LED 驱动芯片对 PWM 频率有特殊要求，以驱动芯片规格为准，并在 `hal_led_config_t` 中调整。

### 11.4 应用层接口

```c
typedef struct {
    bool enabled;
    uint8_t brightness_percent;   // 1~100
    uint16_t cct_kelvin;          // 2700~6500
} light_request_t;

esp_err_t app_light_init(void);
esp_err_t app_light_set(const light_request_t *request);
esp_err_t app_light_off(void);
```

### 11.5 亮度与色温算法

色温范围：

```text
CCT_MIN = 2700K
CCT_MAX = 6500K
```

亮度范围：

```text
BRIGHTNESS_MIN = 1%
BRIGHTNESS_MAX = 100%
```

混光算法第一版使用线性混合：

```c
float mix = (cct_kelvin - 2700.0f) / (6500.0f - 2700.0f);
mix = clamp(mix, 0.0f, 1.0f);

float cold_percent = brightness_percent * mix;
float warm_percent = brightness_percent * (1.0f - mix);
```

亮度到 PWM duty 使用 Gamma 校正，改善低亮度手感：

```c
static uint16_t percent_to_duty(float percent)
{
    if (percent <= 0.0f) {
        return 0;
    }
    if (percent >= 100.0f) {
        return HAL_LED_DUTY_MAX;
    }

    const float gamma = 1.8f;
    float normalized = percent / 100.0f;
    float corrected = powf(normalized, gamma);
    return (uint16_t)(corrected * HAL_LED_DUTY_MAX + 0.5f);
}
```

冷暖通道允许加入校准系数：

```c
cold_percent *= LED_C_GAIN;
warm_percent *= LED_W_GAIN;
```

初始值：

```c
#define LED_C_GAIN 1.0f
#define LED_W_GAIN 1.0f
```

当前算法在 6000 K 以上仍会驱动暖光通道，只是暖光占比会随色温升高逐步减小。
100% 亮度下的典型 duty 关系如下：

| 色温 | 冷光 duty | 暖光 duty | 说明 |
|---:|---:|---:|---|
| 2700 K | 0 | 4095 | 暖光单通道满输出 |
| 6000 K | 3177 | 106 | 暖光仍参与，但占比很低 |
| 6500 K | 4095 | 0 | 冷光单通道满输出 |

样机联调中若 2700 K / 6500 K / 6000 K 以上在 100% 亮度出现关机或重启，
优先排查 LED 恒流设定、升压峰值限流、电感饱和、电池压降和低压保护，
而不是混光算法硬切通道。当前 LED 驱动电路使用 250 mOhm LED 电流采样
电阻和 20 mOhm MOS 源极峰值电流采样电阻；必须在样机上测量 IFB/CS
波形、VSYS、3V3 和 BAT_ADC，确认满载余量。若满载余量不足，应通过增大
LED 电流采样电阻、调整硬件功率链路，或在固件中增加单通道最大 duty /
最大亮度限制来降额。

### 11.6 LED 安全要求

1. `hal_led_init()` 完成后必须保持两路 duty = 0。
2. 电池严重低压时禁止打开 LED。
3. NTC 异常时禁止高亮输出。
4. 过温时必须关闭或降额 LED。
5. 用户调节亮度时必须经过 `app_safety` 限制最大亮度。

---

## 12. 风扇控制

### 12.1 硬件输出

| 网络 | GPIO | 外设 |
|---|---:|---|
| `FAN_PWM` | GPIO45 | LEDC PWM |

### 12.2 HAL 接口

```c
#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t hal_fan_init(void);
esp_err_t hal_fan_set_percent(uint8_t percent);
```

### 12.3 PWM 参数

| 参数 | 推荐值 |
|---|---:|
| PWM 外设 | LEDC |
| 频率 | 20kHz |
| 分辨率 | 10 bit |
| 最小 duty | 0% |
| 最大 duty | 100% |

### 12.4 风扇温控曲线

第一版曲线：

| NTC 温度 | 风扇占空比 |
|---:|---:|
| < 40°C | 0% |
| 40°C | 25% |
| 45°C | 60% |
| ≥ 50°C | 100% |

手动风扇设置：

1. `Fan Manual = ON` 时，用户请求风扇 100% 运行。
2. `Fan Manual = OFF` 时，只取消手动强制，风扇仍按自动温控与安全保护运行。
3. 过温、NTC 异常和关机流程优先级高于手动风扇设置。

如果风扇低占空比无法启动，应加入启动脉冲：

```text
当风扇从 0% 切到 1%~40%：
先输出 100% 300ms，再降到目标占空比。
```

### 12.5 异常策略

| 条件 | 风扇动作 |
|---|---|
| NTC 正常且温度低 | 自动曲线 |
| 温度 > 50°C | 加速 |
| 温度 > 65°C | 100%，并保持系统上电继续散热 |
| NTC 开路/短路 | 100% |
| 系统准备关机且温度高 | 手动/低压关机按电源保持能力处理 |

---

## 13. ADC、电池与 NTC

### 13.1 HAL 接口

```c
#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t hal_adc_init(void);
esp_err_t hal_adc_read_battery_mv(uint32_t *mv);
esp_err_t hal_adc_read_ntc_mv(uint32_t *mv);
```

HAL 返回的是 ADC 引脚处经过校准后的毫伏值，不是原始 ADC 码值。

### 13.2 ADC 配置

| 通道 | GPIO | ADC | 衰减建议 | 说明 |
|---|---:|---|---|---|
| `NTC` | GPIO3 | ADC1_CH2 | 12dB | 输入范围接近 0~3.3V |
| `BAT_ADC` | GPIO9 | ADC1_CH8 | 12dB | 2S 电池经 4:1 分压，最高约 2.1V |

实现要求：

1. 使用 ADC oneshot 模式。
2. 启用 ADC 校准；若校准不可用，必须打印警告，并允许降级运行。
3. 每次读取建议采样 8~32 次取平均。
4. 应用层再做 IIR 滤波或滑动平均。

### 13.3 电池电压计算

硬件分压：

```text
BAT+ -- 300k -- BAT_ADC -- 100k -- GND
```

分压比：

```text
V_BAT_ADC = V_BAT × 100k / (300k + 100k)
V_BAT = V_BAT_ADC × 4
```

代码：

```c
static float battery_adc_mv_to_pack_voltage_v(uint32_t adc_mv)
{
    return ((float)adc_mv / 1000.0f) * 4.0f;
}
```

### 13.4 2S 电池百分比表

第一版使用开路/轻载近似表。实际产品必须根据电池型号和负载重新标定。

| 电池包电压 | 百分比 |
|---:|---:|
| 8.40V | 100% |
| 8.20V | 90% |
| 8.00V | 80% |
| 7.80V | 70% |
| 7.60V | 60% |
| 7.40V | 50% |
| 7.20V | 40% |
| 7.00V | 25% |
| 6.80V | 15% |
| 6.60V | 8% |
| 6.40V | 3% |
| 6.20V | 1% |
| 6.00V | 0% |

使用线性插值：

```c
typedef struct {
    float voltage_v;
    uint8_t percent;
} battery_lut_t;
```

电量显示要求：

1. 放电时百分比不应频繁反向上升。
2. 负载突变时应滤波，避免 UI 跳变。
3. 低压保护使用电压判断，不只依赖百分比。

### 13.5 NTC 参数

当前温度传感器为 10k B3950 NTC。

```c
#define NTC_PULLUP_OHM   10000.0f
#define NTC_R0_OHM       10000.0f
#define NTC_BETA         3950.0f
#define NTC_T0_K         298.15f
#define NTC_VREF_MV      3300.0f
```

若电路为 3.3V 上拉电阻 + NTC 下拉到 GND，中点接 ADC：

```c
static float ntc_mv_to_temp_c(uint32_t ntc_mv)
{
    float v = (float)ntc_mv;
    float vref = NTC_VREF_MV;

    if (v < 50.0f || v > (vref - 50.0f)) {
        return NAN;
    }

    float r_ntc = NTC_PULLUP_OHM * v / (vref - v);
    float inv_t = (1.0f / NTC_T0_K) + (logf(r_ntc / NTC_R0_OHM) / NTC_BETA);
    float temp_k = 1.0f / inv_t;
    return temp_k - 273.15f;
}
```

NTC 异常判断：

| ADC 电压 | 判断 |
|---:|---|
| < 50mV | 短路或下拉异常 |
| > 3250mV | 开路或上拉异常 |
| 非有限数值 | 算法异常 |

---

## 14. 安全保护策略

### 14.1 低压保护

| 条件 | 动作 |
|---|---|
| `Vbat < 6.40V` 持续 3s | UI 提示低电量，最大亮度限制为 50% |
| `Vbat < 6.20V` 持续 5s | 最大亮度限制为 10% |
| `Vbat < 6.00V` 持续 3s | 关闭 LED，保存设置，关机 |
| `Vbat > 6.60V` 持续 5s | 解除低压降额 |

说明：

1. 高亮输出时电池电压会瞬降，保护必须带持续时间和迟滞。
2. 低压保护动作应记录日志。
3. 关机前必须先关闭 LED。

### 14.2 过温保护

| 条件 | 动作 |
|---|---|
| `Temp > 50°C` | 风扇进入高转速区 |
| `Temp > 60°C` | 最大亮度限制为 70% |
| `Temp > 65°C` | 关闭 LED，风扇 100%，UI 显示过温并继续散热 |
| `Temp > 70°C` | 保持 LED 关闭、风扇 100%，不因过温自动断电 |
| `Temp < 55°C` | 允许从过温关灯状态恢复，是否自动恢复由设置决定 |

### 14.3 传感器异常保护

| 异常 | 动作 |
|---|---|
| NTC 异常 | LED 关闭或限制到 20%，风扇 100% |
| BAT_ADC 异常 | 禁止高亮，UI 显示电池异常 |
| ADC 初始化失败 | 不允许打开 LED |

### 14.4 安全优先级

安全优先级从高到低：

```text
严重低压关机
NTC/ADC 异常保护
过温关灯
低压降额
用户亮度设置
```

`app_light` 在输出前必须查询 `app_safety_get_light_limit()`，不得直接按用户请求输出。

---

## 15. LCD 与 UI

### 15.1 显示硬件

| 信号 | GPIO |
|---|---:|
| `LCD_SCK` | GPIO11 |
| `LCD_MOSI` | GPIO12 |
| `LCD_CS` | GPIO14 |
| `LCD_DC` | GPIO13 |
| `LCD_RST` | GPIO10 |
| `LCD_BACKLIGHT` | GPIO21 |

### 15.2 显示驱动要求

1. 使用 `esp_lcd` 驱动 SPI LCD。
2. 使用 `esp_lvgl_port` 或自建 LVGL port。
3. 不使用逐像素阻塞刷屏。
4. 使用 DMA 分块刷新。
5. LCD 控制器暂按 ST7789 兼容处理；若点亮失败，应从屏幕规格书确认控制器、偏移、颜色顺序和初始化序列。

开机画面要求：

1. 正常开机不显示彩条，只显示固件内置默认 Logo。
2. 开机 Logo 不从 NVS、可写数据分区或 OTA 页面读取，避免启动路径依赖可写 Flash 数据。
3. 彩条、红绿蓝白纯色测试保留在隐藏硬件测试模式中，不作为日常启动画面。

### 15.3 UI 页面

第一版只有一个主页面：

```text
┌────────────────────────┐
│ [▰▰▰ ] 7.60V 60%   32.4C │
│                        │
│ Brightness:  50%       │
│ [████████░░░░░░░░]     │
│                        │
│ CCT:       5100K       │
│ [██████████░░░░░]      │
│                        │
└────────────────────────┘
```

必须显示：

1. 亮度百分比。
2. 色温 Kelvin。
3. 电池图标、电池百分比和电池电压。
4. 温度图标和 NTC 温度数字。
5. 故障状态：低电量、过温、传感器异常。

主页面顶部不使用 `BAT` / `TEMP` 文字前缀。电池状态使用电池图标表达，
图标内部按电量百分比填充，并在旁边保留电池百分比与电压数字。温度状态
使用温度计图标表达，并保留温度数字。

主页面不显示风扇转速，避免压缩亮度和色温控制区。风扇百分比保留在设置页
`Fan Curve` 只读项中显示。

### 15.4 UI 更新规则

1. UI 不直接调用 HAL。
2. UI 只响应状态变化或定时刷新。
3. LVGL API 只允许在 UI 任务中调用，或通过 LVGL lock 保护。
4. 传感器数据刷新周期建议 500ms~1000ms。
5. 输入导致的亮度/色温变化应立即更新 UI。

---

## 16. 电源管理

### 16.1 电源保持

`PWR_EN` 是系统电源保持信号。固件进入 `app_main` 后必须尽快执行：

```c
hal_power_hold(true);
```

推荐 `hal_power_init()` 只做 GPIO 配置，不做耗时初始化。

### 16.2 HAL 接口

```c
#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t hal_power_init(void);
esp_err_t hal_power_hold(bool enable);
bool hal_power_key_pressed(void);
esp_err_t hal_power_shutdown(void);
```

### 16.3 关机流程

关机必须按顺序执行：

```text
1. 长按达到阈值后点亮屏幕并提示松开电源键
2. 松开电源键或等待超时后显示关机提示
3. 忽略新的用户调光输入
4. 关闭 LED_C / LED_W
5. 保存 NVS 参数
6. 关闭 LCD 背光
7. 如温度较高，风扇保持短时间运行；若硬件断电无法支持，则跳过
8. 拉低 PWR_EN
9. 系统掉电
```

伪代码：

```c
void app_power_shutdown(void)
{
    app_light_off();
    app_settings_save_now();
    hal_display_set_backlight(0);
    vTaskDelay(pdMS_TO_TICKS(100));
    hal_power_hold(false);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

---

## 17. 设置保存

使用 NVS，namespace 建议：

```text
nvs namespace: light_cfg
```

保存项：

| Key | 类型 | 默认值 | 说明 |
|---|---|---:|---|
| `brightness` | u8 | 50 | 亮度百分比 |
| `cct` | u16 | 5100 | 色温 Kelvin |
| `light_on` | u8/bool | 1 | 开机是否恢复 LED |
| `backlight` | u8 | 80 | LCD 背光百分比 |
| `fan_manual` | u8/bool | 0 | 是否手动强制风扇满速 |
| `ui_rev` | u16 | 1 | 配置结构版本 |

保存策略：

1. 用户旋钮调节时，不要每步都写 NVS。
2. 最后一次变化后延迟 2s~5s 保存。
3. 关机前强制保存。
4. 读取到非法值时恢复默认值并重写。
5. 设置页背光调节只更新缓存并延迟保存，实际背光 duty 立即应用。

---

## 18. 任务划分

### 18.1 推荐任务表

| 任务 | 优先级 | 周期/触发 | 职责 |
|---|---:|---|---|
| `input_task` | 高 | 事件触发 | 处理编码器/按键事件 |
| `sensor_task` | 中 | 500ms~1000ms | 采样电池和 NTC |
| `safety_task` | 中 | 500ms | 更新保护状态和限额 |
| `light_task` | 中 | 事件触发 | 根据状态输出 LED duty |
| `ui_task` | 中 | 5ms~20ms | LVGL handler / UI 刷新 |
| `settings_task` | 低 | 延迟触发 | NVS 保存 |

第一版允许合并：

```text
sensor_task + safety_task
input_task + light_task
```

但 UI 任务应独立，避免 ADC 或 NVS 阻塞导致界面卡顿。

### 18.2 看门狗

1. 所有长循环任务必须 `vTaskDelay` 或阻塞在 queue 上。
2. 禁止 while 死循环不让出 CPU。
3. LCD 初始化失败时应进入错误页面或日志循环，不允许反复重启功率输出。

---

## 19. HAL 实现规范

### 19.1 错误处理

HAL 函数统一返回 `esp_err_t`：

```c
esp_err_t hal_xxx_init(void);
```

要求：

1. 初始化阶段可使用 `ESP_ERROR_CHECK`。
2. 运行阶段不要因普通错误直接 abort，应返回错误并由 app 层处理。
3. 所有错误必须有 `ESP_LOGE` 或 `ESP_LOGW`。

### 19.2 日志 Tag

建议：

```text
BOARD
HAL_POWER
HAL_LED
HAL_FAN
HAL_ADC
HAL_INPUT
HAL_DISPLAY
APP_LIGHT
APP_SENSOR
APP_SAFETY
APP_UI
APP_SETTINGS
```

### 19.3 ISR 规范

ISR 内禁止：

1. 调用 LVGL。
2. 写 NVS。
3. 做浮点计算。
4. 打印大量日志。
5. 分配内存。

ISR 内允许：

1. 读取 GPIO 电平。
2. 记录时间戳。
3. 发送 queue 到任务。
4. 清中断标志。

---

## 20. Codex 开发任务清单

Codex 应按以下顺序实现，不要一次性生成不可验证的大块代码。

### 阶段 A：工程骨架

- [ ] 创建 ESP-IDF 工程结构。
- [ ] 添加 `board_pins.h`。
- [ ] 添加所有组件目录和 `CMakeLists.txt`。
- [ ] `app_main` 可编译并打印启动日志。

验收：

```text
idf.py build 成功。
```

### 阶段 B：电源保持

- [ ] 实现 `hal_power`。
- [ ] 启动后立即拉高 `PWR_EN`。
- [ ] 读取 `EC_KEY_DET`。
- [ ] 实现长按关机流程。

验收：

```text
按键开机后系统保持上电，长按可关机。
```

### 阶段 C：PWM 输出

- [ ] 实现 `hal_led`。
- [ ] 实现 `hal_fan`。
- [ ] 默认 duty 为 0。
- [ ] 提供测试函数或日志命令逐步输出 duty。

验收：

```text
冷光、暖光、风扇 PWM 都能独立控制。
```

### 阶段 D：ADC 与算法

- [ ] 实现 ADC oneshot 初始化。
- [ ] 实现 ADC 校准。
- [ ] 实现电池电压计算。
- [ ] 实现 NTC 温度计算。
- [ ] 实现滤波。

验收：

```text
电池电压接近万用表测量值，NTC 温度接近环境温度。
```

### 阶段 E：输入系统

- [ ] 实现 SW1/SW2 编码器。
- [ ] 实现 SW2_DOWN 和 EC_KEY_DET 消抖。
- [ ] 输出统一 `input_event_t`。

验收：

```text
旋钮方向正确，短按/长按稳定识别。
```

### 阶段 F：LCD 与 UI

- [ ] 初始化 SPI LCD。
- [ ] 点亮背光。
- [ ] 显示纯色测试。
- [ ] 接入 LVGL。
- [ ] 完成主 UI 页面。

验收：

```text
LCD 无花屏，UI 能显示亮度、色温、电量、温度。
```

### 阶段 G：应用逻辑

- [ ] SW1 调亮度。
- [ ] SW2 调色温。
- [ ] SW2 短按开关 LED，电源键短按亮屏/息屏。
- [ ] SW2 长按进入/退出设置页，设置页可调整背光。
- [ ] LED 冷暖混光输出。
- [ ] 风扇按温度自动调速。
- [ ] 设置保存。

验收：

```text
用户可完整操作灯具，关机重启后恢复设置。
```

### 阶段 H：安全保护

- [ ] 低压降额。
- [ ] 严重低压关机。
- [ ] 过温降额。
- [ ] 过温关灯。
- [ ] NTC/ADC 异常保护。

验收：

```text
模拟异常输入时，LED 不会危险输出，系统进入可预期保护状态。
```

---

## 21. 最小可运行版本定义

MVP 固件必须满足：

1. 能编译、烧录、串口输出日志。
2. 开机后 `PWR_EN` 保持。
3. LCD 显示基础 UI。
4. 两个旋钮可分别控制亮度和色温。
5. 冷暖两路 LED 可稳定输出 PWM。
6. 电池电压和 NTC 温度能在 UI 显示。
7. 风扇能自动调速。
8. 长按电源键可关机。
9. 低压和过温保护能关闭 LED。

---

## 22. 量产/联调建议

建议增加隐藏测试模式，可通过上电时按住某个按键进入。

测试模式功能：

1. 显示固件版本、编译时间、芯片信息。
2. 单独测试冷光 PWM：0%、10%、50%、100%。
3. 单独测试暖光 PWM：0%、10%、50%、100%。
4. 单独测试风扇 PWM：0%、50%、100%。
5. 显示 BAT_ADC 原始值、校准毫伏、电池包电压。
6. 显示 NTC 原始值、校准毫伏、温度。
7. 显示每个按键和编码器事件。
8. 显示 LCD 颜色测试：红、绿、蓝、白、黑。
9. 验证默认 Logo 显示稳定。

---

## 23. 未确认项

以下内容需要硬件或样机联调确认，代码中应保留清晰 TODO：

1. LCD 控制器型号是否为 ST7789。
2. LCD 分辨率、X/Y offset、RGB/BGR 顺序。
3. LCD 背光是高电平有效还是低电平有效。
4. LED 驱动 PWM 推荐频率范围。
5. 风扇低占空比能否可靠启动。
6. `GPIO45` 用作 `FAN_PWM` 是否会影响启动。
7. `EC_KEY_DET` 按下时电平是高还是低。
8. 样机验证 `SW2_DOWN` active-low 按键行为与消抖效果。
9. TP5100 的 `CHRG#` / `STDBY#` 是否需要在下一版硬件接入 MCU。
10. BAT_ADC 分压电阻实际精度和 ADC 标定误差。
11. NTC 安装位置与 LED 温度之间的滞后关系。

---

## 24. 编码风格要求

1. C 代码优先，除非项目明确改用 C++。
2. 文件名使用小写下划线。
3. 公开接口放在 `include/*.h`。
4. 私有函数使用 `static`。
5. 所有魔法数字必须定义为宏或 `static const`。
6. 所有 GPIO 使用 `board_pins.h` 宏。
7. 所有任务栈大小、优先级集中定义。
8. 所有模块初始化函数可重复调用时应返回合理错误或保持幂等。
9. 禁止在 UI 文件中写硬件驱动代码。
10. 禁止在 HAL 中写业务策略。

---

## 25. 版本信息接口

建议提供：

```c
typedef struct {
    const char *fw_version;
    const char *build_date;
    const char *idf_version;
    const char *board_name;
} app_version_info_t;

const app_version_info_t *app_get_version_info(void);
```

默认版本命名：

```text
fw_version = "0.1.0-dev"
board_name = "25w-light-esp32s3"
```

---

## 26. 开发完成标准

本固件达到以下条件才算第一版完成：

1. `idf.py build` 无错误。
2. 所有 HAL 初始化失败都有日志。
3. 上电 LED 默认关闭。
4. LED 调光稳定，无肉眼闪烁。
5. LCD 显示稳定，无明显撕裂或偏色。
6. 旋钮输入稳定，不乱跳。
7. 电池电压读数误差经过校准后可接受。
8. NTC 温度读数经过实测验证。
9. 风扇能在高温时强制满速。
10. 低压/过温/传感器异常时不会继续高功率输出。
11. 长按关机可靠。
12. 设置保存不会频繁擦写 Flash。
13. 日志能支持硬件联调。

---

## 27. 推荐实现顺序总结

```text
工程骨架
→ board_pins
→ hal_power
→ hal_led / hal_fan
→ hal_adc
→ app_sensor
→ app_safety
→ hal_input / app_input
→ hal_display / app_ui
→ app_light
→ app_settings
→ 联调与保护策略
```

开发时每完成一个阶段都必须能单独编译和验证，避免在没有硬件闭环的情况下堆叠过多未验证代码。
