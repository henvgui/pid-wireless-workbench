# H2 双向无线串口使用说明

2026.10.24.3：已补齐后台中断后的自动恢复、断线主动关闭、ERASE 前传输失败后的 H2 恢复，并完成实板回归；详见 [项目检查记录](PROJECT_AUDIT_20261024.md)。

版本：2026.10.24.3。路径：电脑上位机 → 主机 USB → 加密 ESP-NOW → 从机 H2 UART → STM32；接收沿反向传回。电脑只需连接主机，从机 USB 平时用于供电/固件维护。

## 接线与使用

| 从机 H2 信号 | 当前 STM32F407 USART1 |
|---|---|
| TX / ESP32 GPIO17 | PA10 / RX |
| RX / ESP32 GPIO18 | PA9 / TX |
| GND | GND |
| VTREF | 目标板 3.3V，按原有接线保留 |

H3 的 SWD 排线保留。不要把 H1 ESP32 调试 TX 当成 H2 目标发送口。

1. 打开「PID 调试工作台」→「03 无线串口」。选择 **H2 双向串口**。
2. 选择与 STM32 程序一致的波特率、数据位、校验和停止位；当前接线按 **115200 / 8N1**。
3. 点「连接串口」。STM32 打印的内容会显示在接收区。
4. 在下方输入框写命令，选择文本或 HEX。文本可选择无结尾、CR、LF、CRLF，按目标程序协议设置。HEX 允许空格，不追加文本结尾；例如 `01 02 FF 00`。
5. 点「发送」。同一条命令需要再次实际发送时再点「发送」；如果上次超时且结果不确定，点「重试上次」，沿用原发送编号，避免重复输出。
6. 接收区支持文本/HEX、UTF-8/GB18030/ASCII、时间戳、暂停显示、清空、保存文本或原始接收字节。暂停显示仍继续接收。

**目标 STM32 必须自行初始化 UART，并实现打印/接收处理。透传不会使任意目标程序自动回显或解析命令。** 发送确认仅表示数据已进入从机 UART 发送队列；目标实际执行成功需要它返回应用层应答。软件不会为串口测试启动电机。

H1 日志通道仍可选择：STM32 TX → H1 RX/GPIO44，共地；只接收，不发送。一个工作台串口会话一次选择一个通道，切换前先关闭串口。

「从机内部自检」使用 ESP32 UART 的内部回环，外部电平转换器保持禁用，数据不送到 STM32。它可检查电脑到从机的收发路径，不能证明外部排线和目标应用正常；平时使用 H2。

## 与烧录共用

开始无线烧录时，后台独占主机 USB；从机先等待 H2 已确认的 TX 排空，暂停 H2，然后执行 UART ROM 或 SWD 烧录。任务完成后恢复原波特率和串口会话，不自动重放旧发送。H1 接收与 H2 独立，但烧录期间电脑读取仍因共享硬件锁而等待。

普通串口模式不会插入原 PID 二进制控制帧；关闭串口后也不会自动恢复该控制帧。要启用专用目标电机协议，须由对应功能明确接管。主机旋钮、OLED、RGB 和主从连接指示保留。

通信中断、缓冲溢出或烧录暂停期间，目标继续打印的数据可能缺失。界面显示丢弃字节、UART 错误和暂停次数。配置波特率不等于承诺该速度能长期满载透传；没有硬件 RTS/CTS 时，持续大流量应降低打印频率，或在目标应用中增加分包序号、CRC、ACK、重传和发送节流。

## AI / CLI

GUI 与 AI 当前共用串口会话及消费游标。让 AI 读取前，先在 GUI 点「关闭串口」；「暂停显示」仍会读数据。不要让 GUI 和 AI 同时推进同一会话的 cursor，也不要用新会话静默接管现有读取者。独立订阅缓存尚未实现；只查 `serial status` 可以并行。完整的状态检查与读取循环见 [AI 读取流程](../AI_USAGE.md#ai-读取无线串口)。

在工作台目录调用 `pidctl.cmd`，输出 JSON；端口始终是主机 USB。会话 ID 为非零 8 位小写十六进制，实际使用请生成新值。

```powershell
.\pidctl.cmd serial configure --session 12345678 --channel h2 --baud 115200 --bits 8 --parity N --stop 1
.\pidctl.cmd serial send --session 12345678 --text 'status' --ending crlf --idempotency-key status-001
.\pidctl.cmd serial read --session 12345678 --cursor 0
.\pidctl.cmd serial status
.\pidctl.cmd serial close --session 12345678
```

二进制发送：`serial send --session 12345678 --hex 0102ff00 --idempotency-key packet-001`。文本默认 UTF-8，CLI 文本结尾默认 none，可用 `--encoding gb18030/ascii` 和 `--ending cr/lf/crlf`。每次读取后保存返回的 `next_cursor` 作为下次游标；HTTP 回复丢失时用原游标重试。

HTTP 使用现有本机 Bearer token；不需要 GUI，也不要把 token 发到外部服务。

```json
POST /v1/serial/configure
{"session":"12345678","port":"auto","channel":"h2","baud":115200,"bits":8,"parity":"N","stop":1}

POST /v1/serial/send
{"session":"12345678","idempotency_key":"status-001","hex":"7374617475730d0a"}

GET /v1/serial/read?session=12345678&cursor=0
GET /v1/serial/status

POST /v1/serial/close
{"session":"12345678"}
```

发送返回 `accepted`、`tx_total`、SHA256 和 `delivery:slave_uart_queued`。单次 1～1024 字节，内部每块最多 128 字节；每会话最多 4096 个发送编号。相同 session + 编号 + 内容重试返回原结果；同编号内容不同会拒绝。前一发送结果未确认时，新编号发送会被拒绝，先重试原请求。

去重适用于当前后台/从机有效会话；从机重启或后台重启后，旧发送不会自动重放。GUI 会重建接收会话并提示数据可能缺失。需要跨断电的业务「只执行一次」，必须在 STM32 应用协议中实现持久事务编号及结果查询。

API 不指定 channel 时沿用旧版 H1 默认，AI 需要双向时必须明确 `channel:h2`。GUI 默认 H2。完整字段见 `/v1/openapi.json`。

## 本次实测

- 主机 COM14、从机 COM18，保留 P=16、I=14、D=0、STEP=1，未执行 SAVE。
- 实际无线 + 从机内部 UART 回环：16 轮共 13,852 字节，含中文及 00～FF，逐字节一致；丢弃、UART 错误、重复发送均为 0。
- 真实 WinForms 发送/接收中文及「重试上次」通过，重试不产生额外 RX。
- H2 会话保持打开时，无线 SWD 重烧此前已验证的相同 F407VET6 镜像，完整读回校验通过（2026.10.24.3 约 9.08 秒），H2 暂停一次后自动恢复，发送会话编号不变。目标未自动启动。
- 以上回环测试隔离了目标板；当前 STM32 业务命令解析和实际外部串口应答仍取决于目标程序。
