$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    & arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -Os -g3 -ffreestanding -fno-builtin -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -Wall -Wextra -Werror -nostdlib '-Wl,--gc-sections' '-Wl,-Map=pa3_high.map' -T flash.ld startup.S main.c -o pa3_high.elf
    if ($LASTEXITCODE -ne 0) { throw 'Compilation failed' }
    & arm-none-eabi-objcopy -O binary pa3_high.elf pa3_high.bin
    if ($LASTEXITCODE -ne 0) { throw 'Binary export failed' }
    & arm-none-eabi-size pa3_high.elf
} finally {
    Pop-Location
}
