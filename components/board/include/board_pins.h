#pragma once

#include "driver/gpio.h"
#include "hal/adc_types.h"

/*
 * Provisional GPIO map from the 2026-06-01 hardware document.
 * Do not initialize power outputs until the corresponding hardware phase.
 */

// Power
#define BOARD_GPIO_PWR_EN            GPIO_NUM_18
#define BOARD_GPIO_EC_KEY_DET        GPIO_NUM_6

/*
 * PWR_EN is documented as active-high in the 2026-06-01 schematic notes.
 * Confirm the active level on a sample unit before hardware validation.
 */
#define BOARD_PWR_EN_HOLD_LEVEL       1
#define BOARD_PWR_EN_RELEASE_LEVEL    0

/*
 * EC_KEY_DET is provisionally enabled for bench shutdown testing as an
 * active-low key with an internal pull-up. Reconfirm the final polarity and
 * bias on hardware before release.
 */
#define BOARD_EC_KEY_DET_CONFIRMED    1
#define BOARD_EC_KEY_PRESSED_LEVEL    0
#define BOARD_EC_KEY_PULL_UP          GPIO_PULLUP_ENABLE
#define BOARD_EC_KEY_PULL_DOWN        GPIO_PULLDOWN_DISABLE

// Native USB: reserved for USB only
#define BOARD_GPIO_USB_DM            GPIO_NUM_19
#define BOARD_GPIO_USB_DP            GPIO_NUM_20

// Encoders / buttons
#define BOARD_GPIO_SW1_A             GPIO_NUM_4
#define BOARD_GPIO_SW1_B             GPIO_NUM_5
#define BOARD_GPIO_SW2_A             GPIO_NUM_2
#define BOARD_GPIO_SW2_B             GPIO_NUM_1
#define BOARD_GPIO_SW2_DOWN          GPIO_NUM_42

/*
 * Encoder A/B signals have external 10 kOhm pull-ups and 100 nF hardware
 * debounce capacitors. Keep the internal pulls disabled for deterministic RC
 * timing. Toggle the direction macros after bench testing if the knob feels
 * reversed in the UI.
 */
#define BOARD_ENCODER_PULL_UP         GPIO_PULLUP_DISABLE
#define BOARD_ENCODER_PULL_DOWN       GPIO_PULLDOWN_DISABLE
#define BOARD_ENCODER_SW1_REVERSED    0
#define BOARD_ENCODER_SW2_REVERSED    0

/*
 * SW2_DOWN has an external 10 kOhm pull-up to 3V3 and is pulled low when
 * pressed. C34 provides hardware debounce filtering on the key node.
 */
#define BOARD_SW2_DOWN_CONFIRMED      1
#define BOARD_SW2_DOWN_PRESSED_LEVEL  0
#define BOARD_SW2_DOWN_PULL_UP        GPIO_PULLUP_DISABLE
#define BOARD_SW2_DOWN_PULL_DOWN      GPIO_PULLDOWN_DISABLE

// LED outputs
#define BOARD_GPIO_LED_C_PWM         GPIO_NUM_38
#define BOARD_GPIO_LED_W_PWM         GPIO_NUM_40

/*
 * The LED PWM inputs are provisionally treated as active-high. Confirm that a
 * low GPIO level disables both LED drivers before hardware validation.
 */
#define BOARD_LED_PWM_OUTPUT_INVERTED 0
#define BOARD_LED_PWM_OFF_LEVEL       0

// Fan: GPIO45 is a strapping pin and requires hardware validation
#define BOARD_GPIO_FAN_PWM           GPIO_NUM_45

/*
 * The fan MOSFET gate is provisionally treated as active-high. Confirm that a
 * low GPIO level keeps the fan off and that the external circuit does not
 * disturb GPIO45 boot strapping before hardware validation.
 */
#define BOARD_FAN_PWM_OUTPUT_INVERTED 0
#define BOARD_FAN_PWM_OFF_LEVEL       0

// LCD SPI: confirmed from the selected 12-pin ST7789 module wiring
#define BOARD_GPIO_LCD_SCK           GPIO_NUM_11
#define BOARD_GPIO_LCD_MOSI          GPIO_NUM_12
#define BOARD_GPIO_LCD_CS            GPIO_NUM_14
#define BOARD_GPIO_LCD_DC            GPIO_NUM_13
#define BOARD_GPIO_LCD_RST           GPIO_NUM_10
#define BOARD_GPIO_LCD_BACKLIGHT     GPIO_NUM_21

/*
 * The selected module is documented as a 240x240 ST7789-compatible panel.
 * The remaining panel parameters are provisional until sample-unit testing.
 */
#define BOARD_LCD_H_RES                       240
#define BOARD_LCD_V_RES                       240
#define BOARD_LCD_X_GAP                       0
#define BOARD_LCD_Y_GAP                       0
#define BOARD_LCD_BGR_ORDER                   0
#define BOARD_LCD_COLOR_INVERTED              1
#define BOARD_LCD_SWAP_XY                     0
#define BOARD_LCD_MIRROR_X                    0
#define BOARD_LCD_MIRROR_Y                    0
#define BOARD_LCD_BACKLIGHT_OUTPUT_INVERTED   0
#define BOARD_LCD_BACKLIGHT_OFF_LEVEL         0

// ADC
#define BOARD_GPIO_NTC_ADC           GPIO_NUM_3
#define BOARD_GPIO_BAT_ADC           GPIO_NUM_9
#define BOARD_ADC_NTC_CHANNEL        ADC_CHANNEL_2
#define BOARD_ADC_BAT_CHANNEL        ADC_CHANNEL_8

// Boot / debug
#define BOARD_GPIO_BOOT              GPIO_NUM_0

// Optional signals are not routed or not confirmed in the current hardware.
#define BOARD_GPIO_USB_VBUS_DETECT   GPIO_NUM_NC
#define BOARD_USB_VBUS_DETECT_ACTIVE_LEVEL 1
#define BOARD_USB_VBUS_DETECT_PULL_UP GPIO_PULLUP_DISABLE
#define BOARD_USB_VBUS_DETECT_PULL_DOWN GPIO_PULLDOWN_DISABLE

/*
 * When the system has no dedicated external-supply detect input, hold LED
 * output off for this fixed delay to allow the USB PD negotiator / voltage
 * booster to stabilize before PWM begins.
 */
#define BOARD_EXTERNAL_SUPPLY_READY_DELAY_MS 2000

#define BOARD_GPIO_TP5100_CHRG       GPIO_NUM_NC
#define BOARD_GPIO_TP5100_STDBY      GPIO_NUM_NC
