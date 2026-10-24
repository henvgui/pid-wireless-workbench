$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $echoSource = '../PID_Shared/PIDFlash/src'
    & arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -Os -g3 -ffreestanding -fno-builtin -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -Wall -Wextra -Werror -nostdlib '-Wl,--gc-sections' '-Wl,-Map=uart_echo.map' -I $echoSource -T ../STM32F103_PA3_HIGH/flash.ld ../STM32F103_PA3_HIGH/startup.S main.c "$echoSource/echo_target.c" -o uart_echo.elf
    if ($LASTEXITCODE -ne 0) { throw 'Compilation failed' }
    & arm-none-eabi-objcopy -O binary uart_echo.elf uart_echo.bin
    if ($LASTEXITCODE -ne 0) { throw 'Binary export failed' }
    & arm-none-eabi-size uart_echo.elf
} finally {
    Pop-Location
}
