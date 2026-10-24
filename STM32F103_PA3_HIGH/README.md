# STM32F103C8T6：PA3 持续高电平

上电后启用 GPIOA，将 PA3 配置为 2 MHz 通用推挽输出，并持续输出高电平。使用复位后的内部时钟，不依赖外部晶振，不初始化串口或 PID 功能。预期 PA3 对目标板 GND 约为目标的 3.3V 供电电压。

镜像链接到 `0x08000000`，按 64 KiB Flash、20 KiB RAM 构建。`pa3_high.bin` 共 436 字节，SHA256：

```text
9a626ea5d2045393307ed86f91e58b9969438b0e9e778a63f0ca52c8f2a03eef
```

安装 Arm GNU Toolchain 后，在当前目录执行 `./build.ps1` 重新编译。

通过当前 ESP32 主从无线链路烧录并启动：在 `../PID_Shared/PIDFlash` 目录执行：

```powershell
& "$env:USERPROFILE/.platformio/penv/Scripts/python.exe" tools/flash_stm32.py ../../STM32F103_PA3_HIGH/pa3_high.bin --part STM32F103C8T6 --execute --run --journal validation/pa3_program_final_20261018.json
```

这会擦除目标 Flash 第一页并写入测试程序，替换原应用的入口。完整读回一致后才请求启动。目标接线和完整使用说明见 [无线烧录 README](../PID_Shared/PIDFlash/README.md)。

2026-10-18 已通过这条无线链路完成实板写入、436 字节完整读回校验和 ROM GO 确认，传输重试 0 次。记录见 `../PID_Shared/PIDFlash/validation/pa3_program_final_20261018.json`。用户实测 PA3 对目标 GND 约 3.3V，确认输出符合预期。
