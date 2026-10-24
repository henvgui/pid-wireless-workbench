# STM32 无线串口日志

2026-10-24：界面入口已更名为「03 无线串口」。本页介绍 H1 接收模式；H2 双向收发和 AI 发送接口见 [WIRELESS_SERIAL.md](WIRELESS_SERIAL.md)。

更新：2026-10-23。单向接收：STM32 → 从机 H1 UART0 → ESP-NOW → 主机 USB → PID 调试工作台。原有 H2 目标串口和 H3 SWD 烧录功能保留。

## 接线

| STM32 | 从机 H1 |
|---|---|
| 打印串口 TX；当前 f407 工程为 PA9／USART1_TX | 第2脚 RX／ESP32 GPIO44 |
| GND | 第4脚 GND |
| 不接 | 第3脚 TX／GPIO43；日志模式为高阻输入 |
| 不接 | 第1脚 3V3；两板各自供电，信号为 3.3V |

只收日志需要 TX 和 GND 两根线，PA10 不用接 H1。若 PA9 已接 H2 RX，可同时连接 H1 RX，两个都是接收输入。H1 日志线不能代替 H3 的 SWD 烧录线。

STM32 普通 UART 输出即可，例如 `HAL_UART_Transmit(&huart1, (uint8_t*)msg, strlen(msg), 100);`，无需专用帧头。文本、数值和二进制均按原始字节接收。

## 软件操作

1. 打开「PID 调试工作台」→ **03 串口日志**。
2. 设置从机物理 UART 的波特率、数据位、校验位、停止位。当前 f407 工程为 **115200、8、N、1**；这些设置不改变 USB 传输速度或 Bootloader 烧录配置。
3. 点击 **开始接收**。从机 H1 从 ESP32 调试命令口切换到日志接收；H1 TX 不输出。USB 诊断接口仍可用。
4. 选择文本或 HEX；文本支持 UTF-8、GB18030、ASCII。UTF-8 多字节字符跨包时连续解码。
5. 时间戳是电脑接收并显示时的本地时间。精确采样时间应由 STM32 自行发送。
6. **暂停显示**仍继续接收，恢复时重绘近期数据。**清空显示**清除电脑保留的数据，不停止 STM32、不改 PID 参数。
7. **保存日志**可保存当前显示 TXT，或电脑保留的原始 BIN 字节。
8. **停止接收**后 H1 恢复 ESP32 调试串口（115200、8N1）。

### 断线恢复

2026.10.23.2 起，已开始的接收遇到无线超时、临时断线或烧录占用时，按 0.5～5 秒间隔重试，保留原游标和窗口已有日志。从机或后台重启导致会话失效时，界面自动重建会话，并显示 `[日志会话已重建；中断期间的数据可能缺失]`。从机断电会丢失其 RAM 缓冲，自动重连不能补回这些数据。

点击「停止接收」会取消自动重试；若其他客户端已经接管接收，本窗口停止自动恢复，避免相互抢占。`已连接 · 等待 STM32 串口数据` 表示日志接口可读，但最近没有收到字节；不能据此断定 STM32 正在打印。

波特率范围 1200～921600，界面提供常用值；数据位 5～8，校验 N/O/E，停止位 1/2。当前不支持 RTS/CTS，也不向 STM32 发送命令。

## 缓冲和完整性

- 从机有 32 KiB 接收环形缓冲。无线层保留加密对端、会话、序号、CRC 和有界重传。
- 返回数据直到下一次读取游标确认才释放；回复丢失后以同一游标重读，不会因为“已发出”就消耗日志。
- 缓冲满时丢弃最早未读字节，软件显示缺失范围和累计丢弃字节数。UART 帧错误、校验错误、驱动溢出显示事件数，不将事件数当成准确丢字节数。
- GUI、AI、烧录共用后台串口锁。烧录时电脑日志轮询让出通道，H1 仍接收；长时间烧录或持续高速日志可能填满缓冲。
- 电脑原始数据最多保留最近 2 MiB，显示框最多约 25 万字符。恢复显示只重绘近期数据；BIN 保存当前保留原始数据，TXT 保存当前显示内容。
- 当前固定配对一台从机，来源 MAC `44:1B:F6:8D:10:18`。未来多从机需扩展节点选择。
- STM32 不发送数据时，空白窗口正常；配置、读取成功不等于已验证 STM32 实际输出。

## AI 的 CLI / HTTP 接口

生成非零、8 位小写十六进制会话号。以下号码仅为格式示例，同一操作重试保留原会话号：

```powershell
./pidctl.cmd serial configure --session a123bc45 --baud 115200 --bits 8 --parity N --stop 1
./pidctl.cmd serial read --session a123bc45 --cursor 0
# 下一次读取使用上一条返回的 next_cursor。
./pidctl.cmd serial close --session a123bc45
./pidctl.cmd serial status
```

接口复用本机 token 认证：

```text
POST /v1/serial/configure
{"session":"a123bc45","port":"auto","baud":115200,"bits":8,"parity":"N","stop":1}

GET /v1/serial/read?session=a123bc45&cursor=0

POST /v1/serial/close
{"session":"a123bc45"}

GET /v1/serial/status
```

读取返回 `chunks`（每块 `offset/gap/hex`）、`next_cursor`、`dropped`、`rx_errors` 和来源 `slave`。`hex` 是未经解码的原始字节。回复丢失仍用原游标，处理完收到的数据才推进到 `next_cursor`。

`409/device_busy` 表示硬件通道正在使用，稍后用原游标重试；`serial_remote_error` 的 `remote_error` 字段给出从机原因。`wrong_transfer/invalid/offset` 或 `serial_session_expired` 需要核对状态后重新配置新会话，并记录中断边界；`timeout/no_link/link` 保留游标重试。`serial_closed` 表示主动关闭，不能自动重开；其他客户端持有会话时也不要覆盖。CLI/HTTP 调用者自行执行恢复策略，GUI 已实现此流程。

`serial status` 只读后台元数据，不访问硬件，返回 `session/config/closed/last_error/last_read_at/received_bytes`。后台还会缓存整个非空 HTTP 批次，直到客户端推进游标，避免 HTTP 回复丢失时丢掉批次中已在无线侧确认的字节。日志不占用长期任务槽，不写主机 NVS、不调用 SAVE/RUN、不改 P/I/D/STEP。

## 验证文件

主机 `host_flash`、从机 `slave_flash_psram` 均已更新。后端日志新增 9 项测试，与已有测试合计 58 项通过；C++ 覆盖溢出、重读、二进制、无效游标和 CRC，原 UART/SWD 烧录回归通过。WinForms 窗口在 144 DPI 下检查通过；UI 截图的示例文本为本地注入，不能当作 STM32 实测日志。

实板记录：`validation/serial_acceptance_20261023/`；GUI 检查：`validation/serial_ui_20261023/`。实际接收字节及目标烧录结果以实板 JSON 为准。

本轮真实采集 9,492 字节，连续内容为 `T=50,50 actual=0,0 PWM=0,0`，缺失范围、丢弃和 UART 错误事件均为 0。另通过正常桌面消息循环验证窗口实时接收、手动停止和关闭窗口释放接收；截图在 `validation/serial_live_ui_20261023/actual_gui.png`。这些结果只覆盖本轮数据速率和连接，不代表任意条件下永不丢包。

上述早期验收使用临时串口测试构建，仅在副本里将 TIM6 中 `LPWM=800`、`RPWM=800` 改为 0。原项目源码保留，副本在 `validation/serial_f407_test_20261023/`。用户之后已通过 Keil 换回自己的程序；这些早期记录不能用于判断当前目标板的 PWM 行为。

2026.10.23.2 接收恢复修复的后端与恢复策略测试共 65 项通过。本次不修改或烧录 STM32 程序。最新实测与限制见 `validation/serial_recovery_20261023/`。
