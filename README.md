# STM32F103C8T6 Blue Pill Console

STM32CubeIDE/CubeMX-compatible project using STM32 HAL, FreeRTOS and USB CDC.

## Build

The repository also includes a GCC Makefile for a repeatable build:

```bash
make GCC_PATH=/opt/st/stm32cubeide_1.18.0/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.0.0.202410170706/tools/bin
```

Generated artifacts are written to `build/`. The linker reserves the last 1 KiB of the nominal 64 KiB flash for persistent configuration.

## STM32CubeIDE

Import `stm32f103-console.ioc` with STM32CubeIDE. The target device is `STM32F103C8T6`. Keep USB FS on PA11/PA12, LED on PC13, ADC examples on PA0/PA1/PA2/PB0/PB1, and SWD on PA13/PA14.

Use ST-Link V2 over SWD for download/debug. The Blue Pill Micro USB enumerates as the firmware's USB CDC virtual serial port.

## Shell examples

```text
help
version
version info
rtos
led on
led off
led blink
gpio mode PA1 input
gpio read PA1
adc read PA0
gpio mode PB0 output
gpio write PB0 1
config save
config load
config reset
disconnect
exit
```

`config save` also persists the LED mode selected by `led on`, `led off`, or
`led blink`. The saved mode is restored after reset or power cycling.

The shell keeps the last eight non-empty commands. ANSI terminal Up/Down
arrows browse history; `exit` closes the current shell session and the next
input reopens it. `disconnect` forces a USB detach/reattach cycle, after which
the welcome banner and `stm32> ` prompt are emitted again.

The current firmware version is `0.0.2`. The `version` and `version info`
commands report it, and `status` includes the same version.

`rtos` reports the current and minimum-ever FreeRTOS heap, task count, task
state, priority, and minimum remaining task stack in words. `rtos status` and
`rtos tasks` are aliases.

The current implementation intentionally has no DAC or PWM command.

For the LED/USB root-cause analysis, debugging detours, final fixes and
lessons learned, see [the debugging retrospective](docs/debugging-retrospective.md).

## Hardware validation

ST-Link V2 was detected and the firmware was programmed and verified successfully. OpenOCD reported target voltage about 3.26 V, device ID `0x410`, and 128 KiB target flash. The build remains conservatively linked for the documented 64 KiB layout, with the final 1 KiB reserved for configuration.

The USB CDC port requires a separate data-capable Micro USB connection from the Blue Pill to the host. The ST-Link USB cable alone only provides SWD programming/debugging.
