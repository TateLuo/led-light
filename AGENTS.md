# AGENTS.md

## Project role

You are developing firmware for an ESP32-S3 based 25W bi-color LED fill light.

The main product specification is in:

* `SOFTWARE_DEVELOPMENT.md`

Always read `SOFTWARE_DEVELOPMENT.md` before implementing product behavior, hardware abstraction, GPIO mapping, control algorithms, safety logic, UI behavior, or test plans.

## Development rules

* Use ESP-IDF style C code.
* Keep hardware access inside HAL components only.
* Application modules must not call ESP-IDF GPIO, ADC, LEDC, SPI, LCD, or PCNT APIs directly.
* Do not hard-code GPIO numbers outside `components/board/include/board_pins.h`.
* If a GPIO number or hardware detail is unknown, create a clearly marked placeholder and add it to `docs/HARDWARE_TODO.md`.
* Prefer small, testable modules.
* Keep public interfaces in `include/*.h`.
* Do not remove safety checks to make code compile.
* Do not invent RGB hardware support unless the schematic defines RGB channels.

## Required commands

After changing C/CMake/Kconfig files, run:

```bash
idf.py set-target esp32s3
idf.py build
```

If the build fails, fix the issue and build again.

## Git workflow

* Work on one phase at a time.
* Commit only after the phase builds successfully.
* Use clear commit messages such as:

  * `phase 1: add board pin map and project skeleton`
  * `phase 2: implement LED and fan HAL`
  * `phase 3: add battery and NTC algorithms`

## Implementation order

Follow this order unless explicitly instructed otherwise:

1. Project skeleton and build system
2. Board pin map
3. Power hold and shutdown HAL
4. LED PWM HAL
5. Fan PWM HAL
6. ADC HAL
7. Battery and NTC algorithms
8. Input event system
9. LCD and LVGL shell
10. Application state machine
11. Safety manager
12. NVS settings
13. Final integration
14. Documentation and hardware test checklist

## Completion criteria

A task is not complete until:

* The code builds successfully.
* New public APIs are documented.
* Hardware placeholders are listed in `docs/HARDWARE_TODO.md`.
* Any unimplemented hardware behavior is represented by a safe stub, not by silent failure.
