# AI 接入与调用约定

2026.10.24.2 新增 H2 双向无线串口：`serial configure --channel h2`，`serial send --text/--hex --idempotency-key ...`；HTTP 增加 `POST /v1/serial/send`。完整请求、去重与恢复边界见 [无线串口及 AI 调用](docs/WIRELESS_SERIAL.md)。旧调用未指定 channel 时仍为 H1。

2026-10-23 新增从机 H1 独立串口日志接口：`pidctl.cmd serial configure/read/close/status`，HTTP 为 `/v1/serial/configure`、`/v1/serial/read`、`/v1/serial/close`、`/v1/serial/status`。固定游标重读及 HTTP 批次缓存避免丢失回复时消耗日志，`gap/dropped/rx_errors` 明确报告溢出；与烧录共享硬件锁。GUI 支持短暂断线自动重试及会话丢失后重建；CLI 调用者需自行执行恢复策略并尊重主动关闭和其他客户端。详见 [串口日志与 AI 接口](docs/SERIAL_LOG.md)。

使用结构化 CLI 或本地 HTTP API，GUI 与 AI 共用同一任务引擎。以下方法不需要模拟鼠标。接口只管理已经适配的硬件操作，不执行任意 shell 命令。

固定硬件路径：**电脑 → 主机 USB → ESP-NOW → 从机 → UART / SWD → 目标 MCU**。`port` 始终是主机 USB，`target_interface` 表示从机到目标板的接口。每次先查询 capabilities，仅当对应接口 `available:true` 才提交操作。

## CLI 快速开始

在工作台目录调用，或用绝对路径运行 `pidctl.cmd`。直接用 Python 时，本机解释器为 `C:/Users/twice/.platformio/penv/Scripts/python.exe`，脚本为 `backend/pidctl.py`。

```powershell
./pidctl.cmd capabilities
./pidctl.cmd ports
./pidctl.cmd device --port auto
./pidctl.cmd plan --chip STM32F103C8T6 --interface uart --firmware 'C:/firmware/app.bin'
./pidctl.cmd flash --chip STM32F103C8T6 --interface uart --firmware 'C:/firmware/app.bin' --idempotency-key robot-build-001
./pidctl.cmd job 任务ID
./pidctl.cmd wait 任务ID --timeout 1800
```

`flash` 默认提交后返回任务 ID；加 `--wait` 会等到终态。输出为单个 JSON，CLI 参数或 API 错误也为 JSON；退出码 0 表示本次请求成功，非 0 表示请求或等待的任务失败。`--help` 为面向人的命令行帮助。

接口值：`uart`、`swd`。SWD 当前适配 STM32F407VGT6/VET6，F407 默认 SWD，其他芯片默认 UART。查询 `chips.interfaces` 确认组合是否支持；未适配组合返回 `unsupported_chip_interface`，不创建任务。SWD 需安装新版两端固件；大于 128 KiB 的镜像需从机 PSRAM 环境。F407VET6 的 SWD 实板烧录与回传已通过；VGT6 和 F407 UART ROM 仍待实板验收。旧 `--method wireless_uart` / `wireless_swd` 是接口别名。`serial_rom` / `stlink_swd` 已移除。`--no-run` 表示不发送烧录后的启动请求。

AI 应为每个用户意图/编译版本生成并保留一个 `idempotency_key`。网络超时后用相同请求和相同键重试，或查询已有任务 ID；不要因没及时收到结果而换键重烧。同键不同固件 SHA256/参数会报 `idempotency_conflict`。失败记录也不自动重执行；明确准备重试实际烧录时，检查原因和目标状态后使用新键。

为了固定镜像，可将 plan 返回的 `sha256` 通过 `--expected-sha256` 传给 flash；文件被改动时会在目标写入前拒绝。后台另外保存独立固件快照。

## PID 与回传

```powershell
./pidctl.cmd pid --p 1.25 --i 0.12 --d 0.15 --step 0.01 --idempotency-key pid-set-001 --wait
./pidctl.cmd stop --idempotency-key stop-001 --wait
./pidctl.cmd run --idempotency-key run-001 --wait
./pidctl.cmd save --idempotency-key save-001 --wait
./pidctl.cmd echo --text '你好，STM32' --idempotency-key echo-001 --wait
./pidctl.cmd echo --file 'C:/data/test.txt' --idempotency-key echo-file-001 --wait
./pidctl.cmd echo --bytes 131072 --idempotency-key echo-large-001 --wait
```

PID ACK 的范围是从机，不代表目标控制器已应用参数或执行运动。回传要求目标已运行对应的 F103 或 F407VET6 UART_ECHO 固件；芯片 ROM 烧录器不会回传任意文本。F407VET6 的应用在相邻 `STM32F407_UART_ECHO` 工程，握手标识为 `F407VE01`。

也可以将完整请求写成 UTF-8 JSON 文件，避免 shell 引号问题：

```powershell
./pidctl.cmd submit --request 'C:/jobs/request.json' --wait
```

## AI 读取无线串口

当前串口 API 使用一个会话和一个消费游标。**GUI 和 AI 不应同时独立读取同一会话。** GUI 中先点「关闭串口」，再让 AI 建立自己的会话；「暂停显示」仍会读取数据，不等于交出读取权。`serial status` 是只读查询，可以与 GUI 并行使用。

推荐顺序：查询状态 → 确认接收已关闭 → 生成新 session → 配置 `channel:h2` → 从 cursor=0 开始读取 → 保存返回的 next_cursor → 关闭自己的会话。不要直接拿 GUI 的 session 另起一个 cursor，也不要为了旁观日志而重新 configure 接管 GUI。当前尚未实现每个订阅者独立缓存。

以下 PowerShell 示例在工作台目录运行，约读取 10 秒，输出原始 HEX 字节块，便于 AI 解析；不会发送目标命令或执行烧录：

```powershell
$receiver = .\pidctl.cmd serial status | ConvertFrom-Json
if (-not $receiver.ok) { throw $receiver.error.message }
if (-not $receiver.data.closed) { throw '请先在 GUI 点关闭串口，或让当前读取者结束会话。' }
$session = [guid]::NewGuid().ToString('N').Substring(0, 8)
if ($session -eq '00000000') { $session = '00000001' }
$opened = .\pidctl.cmd serial configure --session $session --channel h2 --baud 115200 --bits 8 --parity N --stop 1 | ConvertFrom-Json
if (-not $opened.ok) { throw $opened.error.message }
$cursor = [uint32]0
$until = [DateTime]::UtcNow.AddSeconds(10)
try {
    while ([DateTime]::UtcNow -lt $until) {
        $reply = .\pidctl.cmd serial read --session $session --cursor $cursor | ConvertFrom-Json
        if (-not $reply.ok) {
            if ($reply.error.code -eq 'device_busy') { Start-Sleep -Milliseconds 200; continue }
            throw $reply.error.message
        }
        foreach ($chunk in $reply.data.chunks) { $chunk | ConvertTo-Json -Compress }
        $cursor = [uint32]$reply.data.next_cursor
        Start-Sleep -Milliseconds 200
    }
} finally {
    .\pidctl.cmd serial close --session $session
}
```

波特率必须与目标程序相同；其他设备按实际配置修改。该示例遇到会话失效会停止，AI 应重新检查状态并明确建立新会话，不应自动重放旧发送。

- `chunks[].hex` 是原始字节；文本通常按 UTF-8 解码。中文可能跨块，持续读取应使用增量解码器，不能假定每块恰好是完整字符或完整一行。
- `next_cursor` 是下次读取游标。回复丢失时用原 cursor 重试；不要无条件归零、自己跳过字节或重复提交 configure。
- `chunks[].gap`、`dropped`、`rx_errors` 要一起记录；`paused` / `pause_events` 表示 H2 烧录暂停情况。空 chunks 表示暂时没有新字节，不能据此判断目标故障。
- HTTP 409 / `device_busy` 表示烧录或其他硬件操作占用，稍后用同一 cursor 重试。断线期间和暂停期间的数据不保证完整。
- 目标需要运行自行初始化 UART 并打印的应用；目标暂停、没有打印或未实现命令接收时，透传不会生成应答。

需要发回命令、选择 HEX/文本结尾或处理发送去重，见 [无线串口操作指南](docs/WIRELESS_SERIAL.md)。当前主机连接和任务状态由 `device`、`jobs` 查询；它们不消费串口日志。

## HTTP API

基础地址 `http://127.0.0.1:18765`。GUI 或一次 CLI 调用即可启动后台。

```powershell
$service = .\pidctl.cmd service | ConvertFrom-Json
if (-not $service.ok) { throw $service.error.message }
$apiToken = (Get-Content -LiteralPath $service.data.credential_path -Raw).Trim()
$apiHeaders = @{ Authorization = "Bearer $apiToken" }
Invoke-RestMethod 'http://127.0.0.1:18765/v1/capabilities' -Headers $apiHeaders
```

`service` 返回已通过认证验证的凭据文件路径，兼容普通 LocalAppData 和 Codex 应用数据目录；不输出 token 内容。不要把 token 写入提示词、日志或共享文档。

| 请求 | 功能 |
|---|---|
| GET `/v1/capabilities` | 固定硬件路径、芯片、从机目标接口、可执行状态及实测范围 |
| GET `/v1/ports` | 串口与主机/从机身份 |
| GET `/v1/device?port=auto` | 读取主机参数/链路；任务忙时返回带 `cached:true` 的缓存 |
| POST `/v1/plan` | 只检查本地镜像，不写目标 |
| POST `/v1/jobs` | 提交一次明确操作，返回任务 ID |
| GET `/v1/jobs` | 最近任务列表，不含完整日志 |
| GET `/v1/jobs/{id}` | 完整状态、结果和日志尾部 |
| GET `/v1/openapi.json` | OpenAPI 3.1 描述 |
| GET `/v1/serial/status` | 只读查询串口会话、配置、关闭状态和计数，不推进接收游标 |
| POST `/v1/serial/configure` | 打开 H2 双向、H1 日志或内部回环会话 |
| GET `/v1/serial/read?session=...&cursor=...` | 读取原始字节块及 next_cursor、gap、dropped、rx_errors |
| POST `/v1/serial/send` | 用固定发送编号发送 HEX 数据；H1 不支持发送 |
| POST `/v1/serial/close` | 关闭指定串口会话 |

请求头 `Authorization: Bearer <本地token>`；POST 另需 `Content-Type: application/json`。不允许网页跨域调用，不对局域网开放。token 无需写入提示词或日志；CLI 自动读取。

烧录请求示例：

```json
{
  "action": "flash",
  "chip": "STM32F103C8T6",
  "transport": "esp_now",
  "target_interface": "uart",
  "firmware": "C:/firmware/app.bin",
  "port": "auto",
  "run": true,
  "idempotency_key": "robot-build-001"
}
```

成功响应封装为 `{"ok":true,"data":...}`，错误为 `{"ok":false,"error":{"code":"...","message":"..."}}`。同一时间只运行一个硬件任务；忙时返回 HTTP 409 / `device_busy`，稍后可使用原键重试。

`transport` 固定 `esp_now`；省略 `target_interface` 时 F407 默认 SWD，其他默认 UART。旧 `method:wireless_uart` 可继续使用；若与 `target_interface` 同时给出，必须指向同一接口，否则返回 `interface_conflict`。已完成 UART 任务的去重键和记录继续有效。

## AI 如何判断完成

任务状态为 `queued` → `running` → `succeeded` 或 `failed`。后台意外退出后，未完成任务变为 `interrupted`，目标状态可能不确定，不能自动重烧。

1. 烧录成功：任务 `state == succeeded`，`result.verified == true`。
2. 请求启动：UART 要求 `result.go_ack == true`，SWD 要求 `result.start_ack == true`；SWD 是复位启动，不发送 ROM GO。
3. 回传成功：`result.passed == true`，并查看长度、目标 CRC32、返回 SHA256、`preview` 和返回文件。
4. 参数/启停：查看 `result.state.synced`、`ack`、`run_request` 及 `ack_scope`，不把从机确认表述成电机动作完成。

界面关闭不终止后台任务；不要通过杀进程取消正在擦写的操作。当前没有跨掉电自动恢复，也没有无条件“取消后从头重烧”。

## 当前实测范围

本机通过 API/CLI 完成 F103 无线 UART 写入、完整读回、ROM GO、中文原样回传和原参数同步。F407VET6 已完成无线 SWD 写入、完整读回、启动和 131091 字节回传；VGT6 的 1 MiB 算法仅模拟通过，F407 UART ROM 仍待实板验收。G431 实际目标烧录验收待完成。F103 的回传测试固件不能直接给 F407 使用。
