# STM32F407VET6 无线烧录与大文本回传测试

电脑 → 主机 USB → 加密 ESP-NOW → 从机。烧录使用 SWD；程序运行后的回传使用 USART1。当前实板为 512KB 的 STM32F407VET6，应用标识 `F407VE01`；不与 F103 测试程序或 VGT6 的容量配置混用。

## 接线

F407 目标板正常供电。从机 H3 的 VTREF 接目标 3.3V；VTREF 是电平参考，不是 5V 电源输入。

| 从机接口 | F407 目标板 |
|---|---|
| H3-1 VTREF | 3V3 |
| H3-2 DIO | PA13 / SWDIO |
| H3-3 CLK | PA14 / SWCLK |
| H3-4 GND | GND |
| H3-6 NRST | NRST |
| H2-2 TX | PA10 / USART1_RX |
| H2-3 RX | PA9 / USART1_TX |

H3-5 SWO 不需要接。BOOT0 不需要接从机；目标板保持从 Flash 启动的正常配置。回传串口为 115200、8N1。

## 编译与烧录

本程序只配置 PA9/PA10 的 USART1，不运行电机或 PID 控制。使用内部 HSI 16MHz，无需指定外部晶振。PA13/PA14 保留 SWD 功能；Flash 512KiB，主 SRAM 128KiB，静态数据约 840 字节。

在本目录执行 `./build.ps1`，得到从 0x08000000 链接的 `uart_echo.bin`。通过上位机命令行烧录并验证：

```powershell
../PID_Workbench/pidctl.cmd plan --chip STM32F407VET6 --interface swd --firmware ./uart_echo.bin
../PID_Workbench/pidctl.cmd flash --chip STM32F407VET6 --interface swd --firmware ./uart_echo.bin --idempotency-key f407-echo-build-001 --wait
```

烧录会替换 F407 原应用。成功要求任务 `succeeded`、`result.verified:true`、`result.start_ack:true`；`go_ack:false` 是 SWD 模式的正常值。重复提交同一次操作应保留相同 idempotency-key；修改固件或进行新的测试再使用新 key。

## 回传测试

上位机“文本回传”及本地 HTTP API 的 `action:echo` 自动识别 F103/F407 测试程序。独立测试脚本会明确要求 F407，并支持主动故障注入；使用前关闭正在查询同一个串口的 GUI，且不要与后台硬件任务同时运行。

```powershell
./test_text.ps1 -Text '你好 F407，PID 参数与文本回传测试 ABC123。'
./test_text.ps1                         # 131091 字节 UTF-8 文本
./test_text.ps1 -Stress                 # 同样大小，主动丢包及破坏 UART 帧
./test_text.ps1 -File 'C:/data/test.txt'
```

`results/` 保存发送原文、返回内容和 JSON 报告。判定成功需要每块回传字节一致、全文比较一致、两端 CRC32/总字节数/唯一数据块数一致、发送与返回 SHA256 一致。故障注入每第 5 次无线测试发送主动丢弃，每第 7 个数据块首次 UART 发送 CRC 损坏；重试不重复累计目标端数据。测试结束关闭目标转换器并恢复 USB 推送设置，不自动 RUN、不保存或修改 PID 参数。

无线包本身仍可能丢失。该测试验证检测和恢复后的最终数据完整性；持续断电、断线或重试耗尽会明确失败，不会报告 PASS。单次文件上限为 4MiB，STM32 流式处理，不需存下整个文件。

## 当前验证记录

2026-10-19 实板：STM32F407VET6，芯片 ID=0x413、实读容量=512KB。1764 字节测试应用已通过无线 SWD 写入、完整读回和复位启动。98 字节中文回传及 131091 字节大文本全链路回传均通过，正常大文本轮次 53.546 秒、无线/串口重试均为 0，1025 个唯一数据块和 CRC32/SHA256 全部匹配。同样 131091 字节在每第 5 次无线发送主动丢弃、每第 7 个数据块首次 UART 发送损坏的压力测试中也通过：155.954 秒，无线重试 714 次、串口重试 146 次，最终全文/CRC32/SHA256 一致。持续丢弃全部无线测试包时明确失败，返回 0 字节，没有误报成功。详细报告保存于 `../PID_Workbench/validation/f407ve_echo_*.json`。

从机 USB `!swd` 返回最近一次 SWD 的 ACK、调试端口 ID、连接步骤和错误；该命令只读诊断，不发起连接。`!status` 可核对 `translator_oe:1` 和 `target_outputs:"disabled"`，表示未进行目标操作。

`!swdcheck` 短暂启用 B→A 回读，比较 GPIO13 的隔离电平、内部上拉和目标回读值，然后关闭转换器。`!swdhold` 保持同样静态状态 180 秒以供万用表测量；不复位目标、不发时钟，期间拒绝无线烧录/回传请求。`!swdrelease` 提前关闭，超时或 ESP 重启也会退出。测量状态只用于排障，不能当作 SWD 握手或烧录成功。

首次排障实测 H3 VTREF=3.23V、DIO=3.24V，R12=47Ω，用户确认 U4 控制和 A4 电平正常。最终核对到软件引脚映射错误：原理图的 CLK/DIO/SWO 实际为 GPIO12/13/14，旧代码误用 11/12/13，读到的是 CLK 的下拉电平。已将烧录驱动、诊断与引脚定义统一修正，并重烧从机；旧诊断不能据此判断硬件损坏。

同时按 ARM CMSIS-DAP 的时序修正了低电平期间采样 SWDIO、周期结束保持时钟高电平。此前按 VGT6 选择时读到 512KB 会正确阻止擦写；用户已确认目标为 VET6，使用独立 `f407ve` 配置（profile 4）。VGT6 的 profile 3 和 1024KB 检查仍保留，不通过放宽容量检查绕过型号差异。

依据：[ST F407 数据手册](https://www.st.com/resource/en/datasheet/stm32f407vg.pdf)、[RM0090](https://www.st.com/resource/en/reference_manual/rm0090-stm32f407-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)。更完整的传输机制见 [回传协议](../PID_Shared/PIDFlash/ECHO.md)。
