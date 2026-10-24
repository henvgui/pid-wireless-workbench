# 无线 PID 调试与烧录工作台：AI 使用手册

2026-10-24 补充：H2 双向无线串口、文本/HEX 发送、发送重试去重、烧录自动暂停与恢复已经接入。最新调用以 [WIRELESS_SERIAL.md](WIRELESS_SERIAL.md) 和 [AI_USAGE.md](../AI_USAGE.md) 为准；本页以下为原有 V1.2 手册。

版本：V1.2-AI；更新：2026-10-19；后台版本：2026.10.19.2。

本文用于向 Codex、Claude 等 AI 交接当前项目，包含实际硬件、接线、程序位置、调用方法和验证边界。命令为使用示例，是否执行以用户当前任务为准。

## 1. 先理解这套设备

这是“无线 PID 调参器 + 远程固件烧录与数据回传工具”。两块 ESP32-S3 分别作为主机、从机，为 STM32 等目标控制板服务。

```text
电脑上的 GUI / CLI / HTTP API
            ↓ 主机 USB
ESP32-S3 主机：旋钮、OLED、RGB、参数保存、任务转发
            ↓ 加密 ESP-NOW
ESP32-S3 从机：无线收发、镜像暂存、目标接口驱动
            ↓ SWD 或 UART
STM32 目标板
```

烧录方式选的是“从机到 STM32 的接口”。`swd` 和 `uart` 都经过主机 USB、ESP-NOW 和从机。当前软件不使用电脑直连 ST-Link 或 USB-TTL 来替代这条链路。

ESP-NOW 需要启用 Wi-Fi 射频，无需连接路由器。目前是固定配对的一主一从；多从机和颜色身份方案尚未实现。

## 2. 当前硬件与识别

| 对象 | 当前配置 |
|---|---|
| 主机 | ESP32-S3-WROOM-1 N16R8；40 颗 RGB、P/I/D/STEP 编码器、OLED、RUN/STOP、SAVE、LINK LED |
| 从机 | ESP32-S3 N16R8；状态 RGB、目标 UART/SWD、受控电平转换器；8MB Octal PSRAM 已启用 |
| 当前目标 MCU | **STM32F407VET6，512KiB Flash** |
| VGT6 的区别 | STM32F407VGT6 是 1024KiB；不能把当前 VET6 当作 VGT6 选择 |
| 主机 USB | 当前 COM14；序列号 `44:1B:F6:8D:10:20` |
| 从机 USB | 当前 COM18；序列号 `44:1B:F6:8D:10:18` |
| USB VID/PID | 两板均为 `303A:1001`；通过序列号区分身份 |

端口号可能随 Windows 重新分配。先查询 `ports`，通常用 `port:auto`。**目标操作的 port 始终是主机 USB，不能选从机 COM18。** 从机 USB 用于更新 ESP 固件和诊断。

F407VET6 当前运行的是 USART1 回传测试程序，没有运行电机 PID 控制逻辑。验收结束保留主机 PID 参数和 USB 推送设置，保持 STOP，不自动 SAVE 或 RUN。

## 3. 已实现功能与边界

| 功能 | 状态 |
|---|---|
| P/I/D/STEP 旋钮、OLED、RGB | 已实板通过 |
| 参数保存 | SAVE / `save` 写主机 NVS；调参本身只更新 RAM |
| RGB | 空闲渐变流水并整体呼吸；操作旋钮进入单个拖影；5 秒无操作恢复，切换平滑 |
| 主机 LINK LED | 有有效无线连接时亮，失联时灭 |
| 从机自动状态 RGB | 未连接绿色呼吸，有效连接后绿色常亮 |
| 参数无线同步、RUN/STOP 请求 | 从机 RAM 确认已实现 |
| STM32 实际应用 PID 参数、运动控制 | **尚未接入目标应用协议** |
| F103C8T6 无线 UART ROM 烧录 | 实板写入、完整读回、GO 和回传通过 |
| F407VET6 无线 SWD 烧录 | 实板写入、完整读回、复位启动通过 |
| F407VET6 串口回传 | 中文、131091 字节正常/故障测试通过 |
| F407VGT6 | 代码与模拟验证；真实 1MB 目标待验收 |
| G431 与 F407 UART ROM | 已适配；对应目标擦写待实板验收 |
| GD32、其他芯片 SWD、实时 PID 曲线、多从机 | 尚未实现 |

“无线已连接”“参数已同步”“RUN 已确认”均不能表述为 STM32 已执行 PID 或电机已运动。`ack_scope:slave` 表示确认到从机。

## 4. F407VET6 接线

### 4.1 SWD 烧录：从机 H3

| H3 脚号 | 标注 | 从机 GPIO | F407 目标板 |
|---|---|---|---|
| 1 | VTREF | 转换器目标侧电平参考 | 目标 3.3V |
| 2 | DIO / SWDIO | **GPIO13** | PA13 / SWDIO |
| 3 | CLK / SWCLK | **GPIO12** | PA14 / SWCLK |
| 4 | GND | GND | GND |
| 5 | SWO | GPIO14 | 不需要接；预留可对应 PB3 |
| 6 | NRST | GPIO15 | NRST / RESET |

目标板正常供电。VTREF 接目标 3.3V，不接 5V；它是参考电源，不应当作目标板 5V 供电入口。SWDIO/CLK 按名称对应，不交叉。SWD 不需要 BOOT0，目标板保持正常从 Flash 启动配置。

**已纠正的历史错误：CLK/DIO/SWO 实际是 GPIO12/13/14，不是旧版的 11/12/13。** H3 排针脚序没有变化。当前驱动和诊断共用 `slave_target_pins.h`，避免映射再次分离。

### 4.2 应用串口回传：从机 H2

SWD 负责烧录；本次回传测试另需 UART 两根数据线。

| H2 脚号 | 标注 | 从机 GPIO | F407 目标板 |
|---|---|---|---|
| 1 | VTREF | 电平参考 | 目标 3.3V，与 H3 同网 |
| 2 | TX | GPIO17 | PA10 / USART1_RX |
| 3 | RX | GPIO18 | PA9 / USART1_TX |
| 4 | BOOT0 | GPIO16 | 本次 SWD 烧录不需要接 |
| 5 | NRST | GPIO15 | H3 已接时不用重复接 |
| 6 | GND | GND | 共地；H3 已接时不用重复接 |

回传应用为 **115200、8N1**。UART ROM 烧录使用 8E1，由从机切换。STM32 ROM Bootloader 不会自动回传任意文字，必须先运行对应回传应用。

H1 是 ESP32 自身调试 UART0，不是目标 UART：1=3V3，2=RX/GPIO44，3=TX/GPIO43，4=GND。

### 4.3 主机关键引脚

| 功能 | 主机 GPIO |
|---|---|
| RGB 数据 | 47 |
| LINK LED | 38，低电平有效 |
| OLED SDA / SCL | 8 / 9；已验证地址 0x3C |

主机和从机 GPIO 用途不同：从机 GPIO47 控制 U4/U5 OE#，高电平关闭目标转换器；从机 RGB 使用 GPIO38。

## 5. 本机工程与文件

当前工程总目录：

```text
C:/Users/twice/Documents/Codex/2026-10-01/pid/outputs
```

| 相对总目录的路径 | 用途 |
|---|---|
| `PID_Workbench/PID_Workbench.exe` | Windows GUI |
| `PID_Workbench/pidctl.cmd` | 推荐 AI 命令行入口 |
| `PID_Workbench/backend/pidctl.py` | 同一 CLI 的 Python 入口 |
| `PID_Workbench/backend/server.py` | 本地 HTTP 服务 |
| `ESP32_S3_RGB_Chase` | 主机 PlatformIO 工程，使用 `host_flash` |
| `ESP32_S3_Slave_Diagnostic` | 从机 PlatformIO 工程，当前使用 `slave_flash_psram` |
| `PID_Shared/PIDLink` | ESP-NOW 参数同步与会话 |
| `PID_Shared/PIDFlash` | 分包、UART ROM、SWD、目标回传协议及独立测试工具 |
| `STM32F407_UART_ECHO` | 当前 F407VET6 回传应用源码和 BIN |
| `STM32F103_UART_ECHO` | F103 回传应用，不能给 F407 烧录 |
| `PID_Workbench/validation/f407ve_echo_acceptance_20261019.json` | 完整实板验收汇总 |
| `STM32F407_UART_ECHO/results` | 正常、故障和持续丢包报告及原文/返回数据 |

本机 Python：`C:/Users/twice/.platformio/penv/Scripts/python.exe`。

后台数据目录：`%LOCALAPPDATA%/PIDWorkbench`，包括 `jobs.sqlite3`、`jobs/`、`ui_settings.json`、`server.log`、`api_token.txt`。本文不包含 token 的实际值。

## 6. AI 推荐操作流程

以下在 `PID_Workbench` 目录的 PowerShell 中执行；从别处调用请使用绝对路径。

### 6.1 只读检查

```powershell
./pidctl.cmd capabilities
./pidctl.cmd ports
./pidctl.cmd device --port auto
./pidctl.cmd jobs
```

先检查 `chips.interfaces`、接口 `available`、`physical_validation` 和设备状态。当前型号必须选 `STM32F407VET6`；SWD ID=0x413，容量寄存器应为 512KB。仅有家族 ID 不能区分 VE 与 VG。

### 6.2 编译回传应用

```powershell
& '../STM32F407_UART_ECHO/build.ps1'
```

需要 Arm GNU Toolchain。本机已安装 `arm-none-eabi-gcc`。输出 `uart_echo.elf`、`uart_echo.bin` 和 map 文件。当前工程按 Cortex-M4、512KiB Flash、主 SRAM 128KiB 构建，静态数据约 840 字节；使用 HSI 16MHz，无需配置外部晶振。应用标识为 `F407VE01`。

### 6.3 检查镜像，再提交一次烧录

```powershell
$firmware = 'C:/Users/twice/Documents/Codex/2026-10-01/pid/outputs/STM32F407_UART_ECHO/uart_echo.bin'
./pidctl.cmd plan --chip STM32F407VET6 --interface swd --firmware $firmware
./pidctl.cmd flash --chip STM32F407VET6 --interface swd --firmware $firmware --idempotency-key f407ve-example-build-001 --wait
```

`plan` 只校验本地镜像，不写目标。`flash` 会替换镜像覆盖扇区内的原应用/数据。示例 key 用于示例；实际应为本次用户意图与固件版本生成一个新 key 并保存。

只接受从 `0x08000000` 链接的 BIN；当前不接受 HEX/ELF 或偏移应用。VE 上限 512KiB，VG 上限 1024KiB；初始栈要求 `0x20000000 < SP <= 0x20020000`，不接受 CCM 初始栈。型号与实读容量不符时会在擦除前停止，不应通过放宽容量检查绕过。

F407 前 4 个扇区各 16KiB，第 5 个 64KiB，后续各 128KiB；VE 共 8 个、VG 共 12 个。只擦覆盖镜像的完整扇区，同一扇区内镜像之外的旧数据也会擦除。

### 6.4 查询任务

```powershell
./pidctl.cmd job 任务ID
./pidctl.cmd wait 任务ID --timeout 1800
```

不加 `--wait` 时，提交后返回任务 ID。任务状态为 `queued`、`running`、`succeeded`、`failed`；后台异常退出的未完成任务可变为 `interrupted`。

**不能仅凭外层 `ok:true` 判断任务成功。** 它可能只表示查询或提交请求成功，还要看内部任务状态与结果。

### 6.5 串口回传

```powershell
./pidctl.cmd echo --text '你好 STM32F407VET6，测试 ABC123。' --idempotency-key f407ve-text-001 --wait
./pidctl.cmd echo --bytes 131091 --idempotency-key f407ve-large-001 --wait
./pidctl.cmd echo --file 'C:/data/test.txt' --idempotency-key f407ve-file-001 --wait
```

支持 UTF-8 和二进制。输入文本最多 16384 个字符；文件或生成数据为 1 字节至 4MiB。GUI 的“文本回传”自动识别对应 F103/F407 测试应用。

### 6.6 参数、启停与保存

```powershell
./pidctl.cmd pid --p 1.25 --i 0.12 --d 0.15 --step 0.01 --idempotency-key pid-example-001 --wait
./pidctl.cmd stop --idempotency-key stop-example-001 --wait
./pidctl.cmd run --idempotency-key run-example-001 --wait
./pidctl.cmd save --idempotency-key save-example-001 --wait
./pidctl.cmd load --idempotency-key load-example-001 --wait
```

参数和启停 ACK 目前只到从机。`save` 写主机 NVS；`load` 读取已保存值。日常读状态、烧录或回传不需要额外 SAVE。烧录/回传先 STOP，不自动恢复 RUN；工具会恢复原 USB 推送设置。

## 7. 成功判据与重试规则

| 操作 | 必须满足 |
|---|---|
| 烧录 | 任务 `state:succeeded` 且 `result.verified:true` |
| SWD 烧录并启动 | 上述条件加 `result.start_ack:true`；SWD 的 `go_ack:false` 正常 |
| UART ROM 烧录并启动 | 烧录成功加 `result.go_ack:true` |
| 回传 | 任务成功、`result.passed:true`，字节数、逐块内容、目标 CRC32、唯一块数与全文 SHA256 一致 |
| 参数同步 | 查看 `synced`、`ack_scope`；不能当作 STM32 已应用参数 |

同一操作使用同一个 `idempotency_key`。提交超时后先用任务 ID 查询，或用完全相同的请求和 key 重试；不要因没收到响应就换 key 重烧。同 key 不同参数/镜像会返回 `idempotency_conflict`，已失败任务也不会自动重执行。只有确认故障原因并准备发起新操作时才用新 key。

可把 `plan` 返回的 SHA256 通过 `--expected-sha256` 传给 `flash`，防止检查后文件变化。后台另存独立镜像快照。同一时间只有一个硬件任务；`device_busy` 时稍后以原 key 重试。GUI 关闭不会取消后台任务；当前无自动跨掉电续传或取消后重烧功能。

## 8. HTTP API

基础地址：`http://127.0.0.1:18765`，只监听本机。打开 GUI 或调用一次 CLI 可启动服务。

```powershell
$apiToken = (Get-Content "$env:LOCALAPPDATA/PIDWorkbench/api_token.txt" -Raw).Trim()
$apiHeaders = @{ Authorization = "Bearer $apiToken" }
Invoke-RestMethod 'http://127.0.0.1:18765/v1/capabilities' -Headers $apiHeaders
```

| 方法和路径 | 用途 |
|---|---|
| GET `/health` | 服务身份、版本、进程；无需 token |
| GET `/v1/capabilities` | 芯片、接口、能力和实际验收范围 |
| GET `/v1/ports` | USB 端口与主从身份 |
| GET `/v1/device?port=auto` | 主机参数与无线状态；忙时可能是 `cached:true` 的缓存 |
| POST `/v1/plan` | 校验本地镜像，不写目标 |
| POST `/v1/jobs` | 提交操作 |
| GET `/v1/jobs` | 最近任务列表 |
| GET `/v1/jobs/{id}` | 完整状态、结果、日志尾部 |
| GET `/v1/openapi.json` | OpenAPI 描述；实际型号支持以 capabilities 为准 |

除 `/health` 外使用 `Authorization: Bearer <本地token>`；POST 另用 `Content-Type: application/json`。不要把 token 的实际值写到日志、公开文档或给其他 AI 的交接文本里，CLI 会自动读取。

F407VET6 SWD 烧录请求：

```json
{
  "action": "flash",
  "chip": "STM32F407VET6",
  "transport": "esp_now",
  "target_interface": "swd",
  "port": "auto",
  "firmware": "C:/Users/twice/Documents/Codex/2026-10-01/pid/outputs/STM32F407_UART_ECHO/uart_echo.bin",
  "run": true,
  "idempotency_key": "f407ve-example-build-001"
}
```

回传请求：

```json
{
  "action": "echo",
  "port": "auto",
  "bytes": 131091,
  "idempotency_key": "f407ve-example-echo-001"
}
```

`text`、`file`、`bytes` 三种回传来源只能选择一种。`target_interface` 仅用于烧录；回传固定走从机 UART，不要给 echo 请求附加该字段。`action` 支持 `flash`、`echo`、`pid`、`run`、`stop`、`save`、`load`。

也可把请求存为 UTF-8 JSON 文件后调用：

```powershell
./pidctl.cmd submit --request 'C:/jobs/request.json' --wait
```

响应为 `{"ok":true,"data":...}` 或 `{"ok":false,"error":{"code":"...","message":"..."}}`。以任务终态和结果字段判断硬件操作是否完成。CLI stdout 为 JSON，失败有非零退出码。

## 9. 丢包与数据完整性

每块最多 128 字节，携带两端启动会话、传输 ID、序号、偏移、长度和 CRC32。上一块经 STM32 真正处理并回传确认后，才发下一块；从机不能用无线接收成功代替 STM32 确认。

无线侧 150ms 超时，每轮最多发送 20 次；电脑工具再做有界重试。UART 侧 180ms 超时、最多发送 4 次。重复请求回放缓存回复，不重复累计目标端字节数或推进偏移。

目标对整条流累积总字节数、CRC32 和唯一块数；电脑逐块比较，并对全文比较及计算 SHA256。数据是流式处理的，无需在 STM32 RAM 中存下整份文件。

这保证的是“检测并重试后的最终内容通过校验，或明确失败”，不是无线物理层永远不丢包。持续断链、掉电或重试用尽会报错，并保留部分返回内容；不能把这种结果报告为成功。

### 独立故障注入测试

当前 GUI/API 不提供故障注入参数，使用独立脚本。先确保无后台硬件任务，并关闭持续查询主机串口的 GUI，避免与 GUI/API 同时占用 COM14。

在 `STM32F407_UART_ECHO` 目录：

```powershell
./test_text.ps1
./test_text.ps1 -Stress
./test_text.ps1 -Text '你好 F407'
./test_text.ps1 -File 'C:/data/test.txt'
```

默认生成 131091 字节，明确要求 `STM32F407VET6` 应用。`-Stress` 每第 5 次无线测试发送主动丢弃、每第 7 个数据块首次 UART 发送破坏 CRC；结束后关闭测试会话，正常功能不继续注入故障。

## 10. 真实验收结果

2026-10-19，当前接线与环境下：

| 测试 | 结果 |
|---|---|
| 无线 SWD 烧录 | 1764 字节 F407VET6 应用完整写入、读回一致、复位启动确认 |
| 短文本回传 | 98 字节 UTF-8 中文原样返回，识别 `F407VE01` |
| 正常大文本 | 131091 字节、1025 唯一块，53.546 秒，无线/串口重试均 0 |
| 主动丢包与串口损坏 | 同样 131091 字节，155.954 秒，无线重试 714 次、串口重试 146 次，全文一致 |
| 持续丢弃所有无线测试包 | 有界重试后明确失败，返回 0 字节，无误报成功 |

两轮大文本的目标流 CRC32 均为 `7b874363`，原文/返回 SHA256 均为：

```text
a630440e22bad91f79c1a46483da9b6d112d89057516b679037a118a8adeca9d
```

F407VET6 测试 BIN 的 SHA256：

```text
0c641df05e9b829c6688d1789ac66df9d8e19c9cd658d3781867b53b1dbc2000
```

这些是指定测试版本的记录，后续重新编译应重新计算 SHA256。F407VGT6 的满 1MiB、真实目标满容量镜像烧录以及 F407 UART ROM 尚未实板验收；1MiB PSRAM 分配和相关模拟测试不能替代真实目标烧录验收。

## 11. ESP32 固件维护

只有修改或恢复 ESP 固件时才需要执行以下上传；普通 STM32 烧录无需重复上传两块 ESP32。

主机工程目录下：

```powershell
& "$env:USERPROFILE/.platformio/penv/Scripts/platformio.exe" run -e host_flash -t upload --upload-port COM14
```

从机工程目录下：

```powershell
& "$env:USERPROFILE/.platformio/penv/Scripts/platformio.exe" run -e slave_flash_psram -t upload --upload-port COM18
```

先按序列号确认端口，确保无正在进行的目标擦写/回传。主机默认环境 `host_wireless`、从机默认 `slave_wireless` 只是基础无线版；当前目标烧录需明确选上述环境。`slave_flash_psram` 仅用于当前 N16R8 的 Octal PSRAM 硬件；普通 `slave_flash` 用内部 RAM 暂存不超过 128KiB 的镜像。

上传前记录主机 RAM 中当前 P/I/D/STEP，重启可能恢复 NVS 的旧值；需要保留时在重启后恢复 RAM 值，不要擅自 SAVE。先尝试自动下载，不必每次让用户按 BOOT；自动下载失败时再使用 SW8/BOOT 与 SW7/RESET。

## 12. 诊断与常见误判

从机 COM18 的 USB 命令：

| 指令 | 用途 |
|---|---|
| `!info` | 固件、芯片、Flash、真实可用 PSRAM、启动标识 |
| `!status` | uptime、RGB 错误、转换器 OE 和目标输出状态 |
| `!pins` | 当前 GPIO 映射 |
| `!link` | 无线在线、会话、镜像参数、收发统计 |
| `!swd` | 最近一次 SWD 的 ACK、DPIDR、连接步骤、错误；本身不发起连接 |
| `!swdcheck` | 短暂静态回读检查，结束后关闭目标转换器 |
| `!swdhold` | 静态回读状态保持 180 秒，期间拒绝其他目标操作 |
| `!swdrelease` | 提前结束静态测量，恢复关闭转换器 |
| `!auto` | 恢复自动 RGB 连接指示 |

只在需要诊断时直接打开串口，完成后释放。空闲的 `translator_oe:1` / `target_outputs:"disabled"` 是正常状态；它表示不驱动目标，不代表 STM32 未供电。FW 回复的 `target_enabled:true` 是编译能力标记，不能代替这两个运行状态字段。

本次已修复两个软件问题：从机 SWD GPIO 映射错位、SWD 接收采样边沿错误。当前 CLK/DIO/SWO=12/13/14；SWDIO 在时钟低电平期间、上升沿前采样，周期结束保持时钟高电平。不要按旧错误映射推断 U4 或 ESP32 损坏。

容量报错时先核对 VE/VG 和实读值；`device_busy` 先查任务与串口占用；不要反复更换任务键导致重复擦写。回传失败先确认正确目标应用、PA9/PA10 交叉接线及共地。当前软件没有把参数 ACK 接入 STM32 PID 应用，不应将 ACK 的成功扩大为目标控制已闭环。

## 13. 给接手 AI 的入口

建议先读本手册，再按实际任务查看：

- `PID_Workbench/backend/workbench_core.py`：芯片目录、任务校验、任务引擎。
- `PID_Workbench/backend/pidctl.py`、`server.py`：CLI 与 HTTP。
- `PID_Shared/PIDFlash/src/slave_target_pins.h`：正确的 SWD GPIO。
- `PID_Shared/PIDFlash/src/swd_programmer.cpp`：SWD 协议与 F407 Flash 算法。
- `PID_Shared/PIDFlash/src/PIDFlash.cpp`：从机 GPIO、暂存、互斥、无线与 UART 桥。
- `PID_Shared/PIDFlash/src/echo_target.c`：目标 CRC、帧解析、去重与回包。
- `STM32F407_UART_ECHO/main.c`、`startup.S`、`flash.ld`：当前目标测试固件。
- `PID_Workbench/validation/f407ve_echo_acceptance_20261019.json`：真实验收依据。

不应因为缺少聊天历史就重新猜硬件引脚、将当前目标改回 VGT6，或把尚未实现的多从机/目标 PID 控制描述成已完成。

## 14. Keil 下载按钮接入（2026-10-19）

当前工程 `C:/Users/twice/Desktop/STM32PRO/f407/MDK-ARM/f407.uvprojx` 的 `f407` Target 已配置外部下载工具。重新打开工程后，先 Build，再点击 **Flash → Download**，后台通过现有无线 SWD 通道写入本工程 STM32F407VET6，无需打开工作台界面。主从固件和硬件接线沿用现状。

- 适配器：`PID_Workbench/integrations/keil/pid_keil.py`。
- 安装器：同目录 `configure_keil.py --project <uvprojx绝对路径> --target f407 --chip STM32F407VET6`。仅修改 Utilities，修改前保存原工程备份。
- Keil Command：`C:/Users/twice/Documents/Codex/2026-10-01/pid/outputs/PID_Workbench/integrations/keil/PidKeilDownload.exe`。
- Arguments：`"C:/Users/twice/.platformio/penv/Scripts/python.exe" "C:/Users/twice/Documents/Codex/2026-10-01/pid/outputs/PID_Workbench/integrations/keil/pid_keil.py" --firmware "#L" --project "#P" --chip STM32F407VET6 --interface swd --fromelf "C:/Keil_v5/ARM/ARMCC/bin/fromelf.exe"`。
- **Use External Tool for Flash Programming** 选中；**Run Independent** 不勾选。当前已打开的 Keil 要重新加载工程，避免继续使用缓存的旧下载设置。
- 输入支持 AXF/ELF/HEX/BIN；AXF 先用 fromelf 导出带地址 HEX，严格检查地址、容量、校验和，再转换为 BIN。GUI/API 的直接输入仍仅接受 BIN。固件必须链接于 `0x08000000`，不支持自动迁移偏移应用。
- 每次 Download 保存固定产物、SHA256 和原任务键到工程根目录的 `flash_records/keil/日期时间_编号/attempt.json`；任务完整结果为同目录 `job.json`。
- 只接受 `state=succeeded`、`result.verified=true`、`result.start_ack=true`；成功输出 `Programming Done. Verify OK. Application running.`。失败或等待超时退出码非零，不报告成功。
- 提交响应不确定或等待超时时，运行 `pid_keil.py --resume <原attempt.json绝对路径>`；不更换键，不自动重新擦写。已有任务 ID 只查询；无 ID 则以原请求、原键提交，由后台去重。
- `--check` 仅转换并本地验证，不提交 API 任务；`--no-run` 校验完成后不启动；`--records-dir` 可指定日志目录。
- 本功能没有接入断点、单步或变量查看等在线调试。当前 `f407` 应用不同于 UART_ECHO，不能使用 echo 测试帧代替应用验证。

入口已改为 Windows 子系统启动器，避免直接运行 python.exe 弹出空白终端；子进程不创建控制台，进度/错误实时回传给 Keil，保留退出码，结束输出 `Download tool finished (exit 0).`。源码 `integrations/keil/KeilDownloadLauncher.cs`，重建用 `build_launcher.ps1`。更新入口后重新加载工程一次。

2026-10-22 进度改进：本机 Keil 会在外部程序结束后批量显示输出，启动器因此增加独立实时进度窗，显示真实阶段、百分比、活动动画与耗时；百分比不按时间虚构，只有最终结果确认并正常退出后才显示 100% 完成。成功约 1.4 秒自动收起，失败显示红色错误、8 秒后自动收起；处理中“收起”或关闭窗口只隐藏显示，不中断烧录。窗口支持 PerMonitorV2 高 DPI。进度源码 `integrations/keil/DownloadProgress.cs`；下一次 Download 即使用更新，原路径入口无需改配置。自动化可设置该进程的 `PID_KEIL_NO_PROGRESS=1` 保持无窗口调用。

2026-10-22 SWD 提速：从机 `slave_flash_psram` 已更新，继承 `slave_flash` 中的 `PID_FLASH_SWD_HALF_PERIOD_US=2`（原 5 µs）。同一份 8,708 字节 F407VET6 镜像实测 20.08 秒 → 12.60 秒，保留完整读回校验；任务 `6fc4bd9178dc915eabd15be0` 的 `verified/start_ack=true`，重试 0 次。无需改 Keil。将来接线更长而出现 SWD 错误时可将此值改回 5 并更新从机；不自动重试不确定的擦写。记录：`PID_Workbench/validation/speed_benchmark_20261022/`。主机参数及 USB 推送设置保持原值；这项验收仅证明烧录和启动，不证明当前 STM32 应用响应主机 STOP 或实现闭环控制。

完整操作和故障恢复说明：`PID_Workbench/docs/KEIL_DOWNLOAD.md`。

凭据路径注意：Codex 的 MSIX 环境可能把 `%LOCALAPPDATA%/PIDWorkbench` 重定向到 `%LOCALAPPDATA%/Packages/OpenAI.Codex_*/LocalCache/Local/PIDWorkbench`，普通 Keil 进程看不到同一个逻辑路径。`backend/api_client.py` 已自动查找同一用户已有的 Codex 服务凭据；不需要复制 token、重建数据库或更改 Keil 参数。客户端只在明确的 HTTP 401 后尝试其他凭据，不会在超时/断连后自动重放 POST。显式 `PID_WORKBENCH_DATA` 保持优先且不跨目录回退。

## 15. 主机 STEP 长按开关 RGB（2026-10-19）

主机完整功能固件为 `ESP32_S3_RGB_Chase` 的 `host_flash` 环境。STEP 按键是 GPIO17、低电平按下。

- **长按 2 秒**：切换整圈 40 颗 RGB 的关闭 / 开启，达到时间立即生效；持续按住只触发一次，松手不会额外执行切色。
- **短按**：在松手并完成消抖后切色；P/I/D 的原短按逻辑保留。
- 关灯后旋钮仍调节 P/I/D/STEP，OLED 与 USB / ESP-NOW 通信继续运行；普通旋钮操作和空闲动画不会重新开灯。再长按一次恢复原灯效及设置亮度。
- 此开关不写 NVS，重新上电默认开启，不改变 SAVE 的参数记录。
- USB `!rgb on` / `!rgb off` / `!rgb toggle` 操作同一开关；`!status` 的 `rgb_enabled` 为当前开关，`step_hold_ms=2000`，`step_holds` 为物理长按计数。USB 指令不伪造物理长按计数。
- 灯效测试 `!rgbtest on` 也受这个总开关约束；关灯后 RGB 数据输出为全黑帧，保留现有 RMT 驱动及渐变补偿。

软件检查覆盖短按与长按互斥、每次长按只触发一次、松手跨过消抖截止时间、上电已按住、毫秒计数回绕，以及关灯时调参和动画超时仍保持全黑。


## 16. 从机 H1 无线串口日志（2026-10-23）

从机 H1 第2脚 RX/GPIO44 接 STM32 的打印 TX；第4脚接 GND，H1 TX 留空。当前 f407 为 PA9/USART1、115200/8N1。工作台左侧「03 串口日志」可配置波特率、5～8数据位、N/O/E校验、1/2停止位，显示文本/HEX、时间戳，支持暂停、清空和保存。本版单向接收，不改 H2/H3 烧录通道和主机保存参数。

AI 使用 `pidctl.cmd serial configure --session <非零8位小写十六进制> --baud 115200 --bits 8 --parity N --stop 1`，再用 `serial read --session <同一号> --cursor <上次next_cursor>` 读取；完成 `serial close --session <同一号>`。`serial status` 不需要 session，只读本地会话、配置、关闭状态、最后读取时间及接收字节数，不访问硬件。HTTP 为 `/v1/serial/configure`、`/v1/serial/read`、`/v1/serial/close`、`/v1/serial/status`。与烧录共用硬件锁，busy 时保留原游标稍后重读；必须检查 gap/dropped/rx_errors，不能承诺无限高速或断线永不丢字节。

2026.10.23.2 GUI 会自动重试短暂断线，确认从机或后台丢失日志会话后重建接收，保留已有日志并提示中断期间可能缺失数据。主动停止或其他客户端接管会取消自动恢复。CLI/HTTP 客户端自行实现同样策略，详见 `docs/SERIAL_LOG.md`。

接线和缓冲边界见 `PID_Workbench/docs/SERIAL_LOG.md`。UI 原始保留2MiB，从机32KiB，暂停显示仍接收。H1 日志不能代替下载所需的 H3 SWD 五根线。
