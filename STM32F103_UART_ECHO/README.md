# F103 大文本双向回传测试

链路为：电脑 → 主机 USB → 加密 ESP-NOW → 从机 → USART1 → F103 → USART1 → 从机 → ESP-NOW → 主机 USB → 电脑。F103 仍保持 PA3 高电平；PA9 为 TX、PA10 为 RX，应用串口为 115200、8N1。ROM 烧录时使用 8E1，两者由从机自动切换。

两块 ESP32 使用 `host_flash` / `slave_flash` 环境。F103 的 `uart_echo.bin` 已通过无线烧录并启动；主机的 OLED、旋钮、RGB 和原有 PID 参数功能保留。F103 当前运行的是回传测试程序，不是执行电机 PID 的程序。

## 直接使用

在当前目录打开 PowerShell。输入一段文字：

```powershell
./test_text.ps1 -Text '你好 STM32，测试中文、ABC123 和 PID 参数回传。'
```

发送已有文本文件：

```powershell
./test_text.ps1 -File 'C:/Users/twice/Desktop/test.txt'
```

也可以先编辑随附的 `sample_text.txt`，执行 `./test_text.ps1 -File ./sample_text.txt`。

不带参数时自动生成 128 KiB UTF-8 文本。主动制造丢包/损坏的压力测试：

```powershell
./test_text.ps1 -Stress
```

每次运行在 `results/` 保存原文 `_sent.txt`、返回文本 `_returned.txt` 和 JSON 报告。只有每块内容、完整文件 SHA256、F103 回报的总字节数、CRC32 和唯一数据块数都一致，才显示 `PASS`。文件按原始字节传输，支持中文和二进制；二进制的 `.txt` 输出文件应按二进制查看。

测试开始会 STOP 并等待从机确认，结束后恢复主机原来的 USB 推送设置，不自动 RUN，不改动已保存的 PID 参数。单次电脑测试限制为 4 MiB；F103 使用约 840 字节静态 RAM 流式处理，无需把整份文件存进 MCU RAM。

## 如何检测丢失和恢复

- 每块最多 128 字节，携带两端启动会话、传输 ID、序号、字节偏移、长度和 CRC32。每次等上一块的 F103 回包通过验证后再发下一块，避免接收端缓冲被灌满。
- 同一帧格式贯穿无线和串口。只有 F103 真正处理并回传的数据，才会成为成功的数据确认；从机不能用收到无线包来代替 F103 的确认。
- 无线 150ms 超时重发，每轮最多 20 次；电脑再做最多 3 轮有界重试。串口 180ms 超时，最多发送 4 次。相同请求重放缓存回复；数据重试不会重复累计字节或重复推进偏移。
- F103 对整条数据流累积 CRC32、总字节数和唯一数据块数；电脑逐块比较并对整份返回数据计算 SHA256。
- 重试用尽时明确失败，报告中 `passed:false`，保存已返回的部分文本。链路失效或会话空闲超过 10 秒，从机关闭目标电平转换器。无线断网、断电、持续干扰不能保证成功完成，本版不会将不完整结果报告为成功。
- `-Stress` 仅对回传测试主动丢弃每第 5 个无线发送，并破坏每第 7 个数据块首次串口发送的 CRC；心跳/PID 包不参与主动丢弃。运行后关闭测试会话，正常使用不注入故障。

## 重新编译和烧录

`./build.ps1` 使用 Arm GNU Toolchain 构建 `uart_echo.bin`，链接到 0x08000000，限定 F103C8 的 64 KiB Flash / 20 KiB RAM。启动代码和链接脚本复用相邻 `STM32F103_PA3_HIGH` 项目。

在 `../PID_Shared/PIDFlash` 目录执行以下命令重新烧录并运行：

```powershell
& "$env:USERPROFILE/.platformio/penv/Scripts/python.exe" tools/flash_stm32.py ../../STM32F103_UART_ECHO/uart_echo.bin --part STM32F103C8T6 --execute --run
```

烧录会替换目标原应用。H2 的 VTREF、交叉 TX/RX、BOOT0、NRST 和 GND 接线见 [烧录说明](../PID_Shared/PIDFlash/README.md)。

测试结果和更底层指令见 [回传协议说明](../PID_Shared/PIDFlash/ECHO.md)。

2026-10-18 实板验收：两轮各 131091 字节，正常传输和主动故障测试均全文一致。正常轮次 56.954 秒；故障轮次无线重试 714 次、串口重试 146 次，158.172 秒。持续丢弃所有无线测试包时，工具明确报告失败，未误报成功。验收汇总在 `../PID_Shared/PIDFlash/validation/acceptance_echo_20261018.json`。
