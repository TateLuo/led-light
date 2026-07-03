# led-light-25W

ESP32-S3 firmware for the 25 W fill light.

Final integration implements the power hold, cold/warm LED PWM, fan PWM, ADC HAL, sensor
algorithms, input event system, ST7789-compatible SPI LCD shell, the
mutex-protected application state model, the safety manager, and NVS-backed
user settings. It also connects the requested light state to safety-limited
physical LED output. On
startup, firmware asserts `PWR_EN`, configures the LED outputs at 19531 Hz
with 12-bit duty resolution, configures the fan output at a provisional
20 kHz with 10-bit duty resolution, keeps the LED outputs off until the first
sensor and safety checks pass, and starts the sensor, safety, light, and input
event tasks. The display HAL initializes the provisional
240x240 ST7789 panel over 20 MHz SPI, briefly shows RGBW startup color bars,
and starts a DMA-backed LVGL 9 partial-refresh port. Filtered battery and NTC
values are synchronized into `app_state`. Input events update brightness, CCT,
requested light state, and display backlight state atomically. The UI renders
only a read-only `app_state` snapshot. ADC1 oneshot reads use 16-sample
averaging and curve-fitting calibration when available.

`SW2_DOWN` is enabled as an active-low light toggle using its external 10 kOhm
pull-up. The power key uses short press for display backlight on/off and long
press for release-to-shutdown. Startup shows a release-key prompt after the
power hold and UI are ready; shutdown shows a release-key prompt before
turning outputs off. Encoder inputs use the external pull-ups and RC debounce
network with internal pulls disabled; event direction must be checked on a
sample unit. LCD offset, RGB/BGR order,
inversion, orientation, SPI clock, and backlight polarity also remain
provisional. Firmware evaluates safety policy
every 500 ms, applies a temperature-based fan curve, limits brightness for
low-voltage and sensor faults, and forces LED output off for NTC faults or
over-temperature conditions. Sustained severe low voltage releases the power
hold after a best-effort output shutdown; over-temperature keeps the system
powered so the fan can continue cooling with the LED output forced off. The
current board mapping drives `LEDC_PWM` on GPIO38 and `LEDW_PWM` on GPIO40.
`app_light` applies linear cold/warm mixing, Gamma 1.8 duty correction, and the active
safety brightness limit before writing PWM duty. At 100% brightness, 2700 K
drives the warm channel to full duty, 6500 K drives the cold channel to full
duty, and 6000 K still drives a small nonzero warm duty. Endpoint full-bright
shutdowns should be debugged as LED current-limit, boost peak-current, battery
droop, or low-voltage-protection issues before changing the mixing algorithm.
Brightness, CCT, requested
light state, backlight percentage, manual fan request, and the settings schema
revision are stored under the `light_cfg` NVS namespace.
Long-pressing `SW2_DOWN` opens a vertically scrolling settings page: `SW2`
rotation selects an item, `SW1` rotation adjusts the active item, and `SW2`
short press exits. The first editable setting is LCD backlight percentage; fan
manual control can force the fan on, while fan curve and thermal guard are
shown read-only to keep safety thresholds fixed. Turning fan manual off only
removes the manual force request; thermal protection can still run the fan.
Input changes are
coalesced for 3 seconds before committing, while manual and
safety shutdown paths freeze the pre-shutdown snapshot and save synchronously
before releasing the power hold. Safety force-off conditions preserve the user
request for later recovery while keeping the physical LED outputs off. A
display initialization failure is logged and leaves the backlight off without
restarting the power outputs.

The settings page also exposes `WiFi OTA`. Select it with `SW2` rotation, then
rotate `SW1` in either direction to start the OTA page. The firmware first saves
current settings, turns the LED output request off, starts a SoftAP named
`LED25W-OTA-xxxx`, and shows the SSID, password, and `192.168.4.1` browser
address on the screen. Upload a firmware `.bin` from the browser page; during
upload the power key shutdown action is blocked to avoid interrupting flash
writes. After a verified upload the device reboots into the new OTA partition.
Startup always uses the firmware's built-in default logo, so Web OTA only
updates firmware.
See `docs/HARDWARE_TODO.md`.
