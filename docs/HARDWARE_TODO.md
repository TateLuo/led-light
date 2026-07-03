# Hardware Confirmation TODO

Phase 1 records the provisional 2026-06-01 schematic mapping without driving
hardware. Confirm these items on the final schematic, PCB netlist, and sample
unit before the corresponding implementation phase.

## GPIO Mapping

- [ ] Confirm `PWR_EN` is GPIO18. Firmware provisionally uses active-high hold
  and low-level release based on the 2026-06-01 schematic notes.
- [ ] Confirm `EC_KEY_DET` is GPIO6, determine the pressed level, and determine
  whether an internal pull-up or pull-down is required. Firmware provisionally
  enables active-low power-key handling with an internal pull-up for bench
  shutdown testing.
- [ ] Confirm encoder inputs: `SW1_A` GPIO4, `SW1_B` GPIO5, `SW2_A` GPIO2,
  `SW2_B` GPIO1. Firmware currently relies on the external 10 kOhm pull-ups
  and 100 nF debounce capacitors, with internal pulls disabled. Verify that
  clockwise rotation produces the expected event direction.
- [x] Confirm `SW2_DOWN` is GPIO42. The schematic shows R42 10 kOhm pulling
  the node up to 3V3, so the pressed level is low and no internal pull is
  required.
- [x] Confirm cold and warm PWM outputs: `LEDC_PWM` GPIO38 and `LEDW_PWM`
  GPIO40. The warm LED PWM wiring has been adjusted to match the firmware
  mapping. Firmware provisionally treats both PWM inputs as active-high and
  drives them low while off.
- [ ] Add or confirm hardware pull-downs / delayed enable for the cold and
  warm LED driver PWM or enable inputs. Firmware now drives both LED PWM pins
  off in `board_init()`, but it cannot control the pins before the ESP32-S3
  ROM and bootloader hand off to the application.
- [ ] Confirm whether GPIO45 is acceptable for `FAN_PWM` without affecting
  boot strapping. Firmware provisionally treats the fan MOSFET gate as
  active-high and drives it low while off.
- [ ] Confirm ADC inputs: NTC GPIO3 / ADC1_CH2 and battery GPIO9 / ADC1_CH8.
  Firmware validates this provisional GPIO-to-channel mapping during startup.
- [ ] Keep GPIO19 and GPIO20 reserved for native USB D- and D+.
- [ ] Keep GPIO0 reserved for BOOT mode only.

## LCD Module

- [ ] Confirm the LCD controller is ST7789-compatible.
- [ ] Confirm the selected module resolution is 240x240 and validate the
  provisional X/Y offset of 0/0, RGB order, no axis swap, no mirroring, and
  disabled color inversion. Firmware shows RGBW color bars briefly at startup
  to support sample-unit validation.
- [ ] Confirm LCD backlight active level and PWM capability. Firmware
  provisionally treats the backlight as active-high and uses 5 kHz PWM.
- [ ] Confirm the ST7789 module remains stable at the provisional 20 MHz SPI
  clock. Reduce the clock if sample-unit signal integrity requires it.
- [x] Record the provided SPI wiring: RST GPIO10, SCK GPIO11, MOSI GPIO12,
  DC GPIO13, CS GPIO14, BACKLIGHT GPIO21.

## Power And Sensors

- [ ] Confirm the LED driver PWM frequency range. Firmware uses 19531 Hz
  with 12-bit duty resolution.
- [ ] Validate physical LED enable with a current-limited bench supply before
  normal operation. Firmware now restores `light_on` after sensor and safety
  checks, then applies linear cold/warm mixing with Gamma 1.8 correction.
- [ ] Verify the cold/warm endpoints at 2700 K and 6500 K, confirm CCT
  direction, and tune the provisional cold/warm channel gains of 1.0 / 1.0 if
  optical output requires calibration.
- [ ] Characterize LED full-bright endpoint behavior. Current samples have
  shown shutdown risk at 2700 K / 100% and at high-CCT / 100%, where one LED
  channel approaches full duty. Measure LED sense voltage across R27/R36
  250 mOhm, switch current-sense voltage across R23/R33 20 mOhm, VSYS, 3V3,
  and BAT_ADC under 2700 K, 6000 K, and 6500 K full-bright load. Decide whether
  the fix is a larger LED sense resistor, improved power-path margin, or a
  firmware single-channel duty/brightness derate.
- [ ] Confirm the fan PWM frequency range. Firmware provisionally uses 20 kHz
  with 10-bit duty resolution.
- [ ] Measure whether the fan starts reliably at low PWM duty. A startup pulse
  remains intentionally disabled until sample-unit validation.
- [ ] Confirm the battery divider resistor values and ADC calibration error
  against a multimeter. Firmware uses ADC1 oneshot reads, 12 dB attenuation,
  16-sample averaging, and eFuse curve-fitting calibration when available.
- [ ] Recalibrate the provisional 2S battery percentage lookup table against
  the selected battery pack under representative loads.
- [ ] Characterize the degraded ADC conversion error on units without usable
  calibration eFuse data. Firmware falls back to an approximate 3.3 V linear
  conversion and logs a warning.
- [x] Confirm the NTC circuit is a 3.3 V pull-up with R40 10 kOhm and a wired
  R39 10 kOhm B3950 1% NTC to ground. Firmware uses this topology.
- [ ] Measure NTC installation position, thermal lag, and suitable filtering.
  Firmware provisionally samples every 1000 ms and applies an IIR alpha of
  0.25 to battery voltage and NTC temperature.
- [ ] Validate the provisional low-voltage policy against the selected 2S
  battery under representative loads: 6.40 V / 3 s derate, 6.20 V / 5 s
  critical derate, 6.00 V / 3 s shutdown, and 6.60 V / 5 s recovery.
- [ ] Validate the provisional thermal policy against the NTC installation
  position and thermal lag: fan curve from 40 C to 50 C, 60 C brightness
  derate, 65 C LED force-off with continued fan cooling, and recovery below
  55 C.

## Safe Placeholders

The following optional signals currently use `GPIO_NUM_NC` in `board_pins.h`
because no MCU routing has been confirmed:

- `BOARD_GPIO_USB_VBUS_DETECT`
- `BOARD_GPIO_TP5100_CHRG`
- `BOARD_GPIO_TP5100_STDBY`

Confirm whether TP5100 `CHRG#` / `STDBY#` or USB VBUS detection should be
routed in a future hardware revision.
