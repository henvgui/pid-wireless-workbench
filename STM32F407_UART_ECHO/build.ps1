$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $echoSource = '../PID_Shared/PIDFlash/src'
    & arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -g3 -ffreestanding -fno-builtin -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -Wall -Wextra -Werror -nostdlib '-Wl,--gc-sections' '-Wl,-z,noexecstack' '-Wl,-Map=uart_echo.map' -DPID_ECHO_F407VE -I $echoSource -T flash.ld startup.S main.c "$echoSource/echo_target.c" -o uart_echo.elf
    if ($LASTEXITCODE -ne 0) { throw 'F407 compilation failed' }
    & arm-none-eabi-objcopy -O binary uart_echo.elf uart_echo.bin
    if ($LASTEXITCODE -ne 0) { throw 'F407 binary export failed' }
    & arm-none-eabi-size uart_echo.elf
} finally {
    Pop-Location
}
