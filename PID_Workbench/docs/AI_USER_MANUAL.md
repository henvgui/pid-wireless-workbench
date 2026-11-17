# ESP32 无线 PID 工作台 AI 操作与升级手册

更新于 2026-11-09。交给接手的 AI 时，提供本文件及仓库目录。文中的命令按任务选用。示例中的镜像、芯片、端口和任务键以当次操作为准。

电脑通过 USB 接入 ESP32-S3 主机，主机经加密 ESP-NOW 与从机通信。从机通过 UART 或 SWD 访问 STM32。PID 工作台、CLI 和 Keil 下载共用本机后台任务引擎。

## 1. 找到工程并检查状态

在包含 `ESP32_S3_RGB_Chase`、`ESP32_S3_Slave_Diagnostic`、`PID_Shared`、`PID_Workbench` 的仓库根目录运行 PowerShell。原电脑的仓库位于 `C:/Users/twice/Documents/Codex/2026-10-01/pid/outputs`。以下相对路径均从仓库根目录计算。

| 部件 | 源码或入口 | 本机配置 |
|---|---|---|
| ESP32-S3 主机 | `ESP32_S3_RGB_Chase`，PlatformIO 环境 `host_flash` | N16R8，常见 COM14，USB 序列号 `44:1B:F6:8D:10:20` |
| ESP32-S3 从机 | `ESP32_S3_Slave_Diagnostic`，PlatformIO 环境 `slave_flash_psram` | N16R8，常见 COM18，已登记身份 GREEN，USB 序列号 `44:1B:F6:8D:10:18` |
| 共享协议 | `PID_Shared/PIDLink`、`PID_Shared/PIDFlash` | 加密配对、参数同步、镜像传输、烧录和无线串口 |
| PID 工作台 | `PID_Workbench/PID_Workbench.exe`、`PID_Workbench/pidctl.cmd` | GUI、JSON CLI 和 `127.0.0.1:18765` 上的本地 HTTP API |
| Keil 外部下载器 | `PID_Workbench/integrations/keil` | 已配置的 `f407` 工程使用 STM32F407VET6、SWD |

先执行这些状态查询。CLI 会在需要时启动本机后台。

```powershell
& .\PID_Workbench\pidctl.cmd capabilities
& .\PID_Workbench\pidctl.cmd ports
& .\PID_Workbench\pidctl.cmd device --port auto
& .\PID_Workbench\pidctl.cmd jobs
& .\PID_Workbench\pidctl.cmd serial status
```

按 USB 序列号区分两块 ESP32，COM 号可能变化。`device`、烧录、PID 和无线串口的 `--port` 都选主机，通常用 `auto`。从机 USB 用于维护从机固件和读取本机诊断。设备查询带 `cached:true` 时，等占用结束后重新读取。

从 `capabilities` 确认实际支持的芯片与接口。从 `device` 确认链路、当前选择的 `peer_name`、`peer_mac` 和 PID 状态。烧录前还要核对 STM32 的完整型号、供电、接线和固件用途。F407VET6 为 512 KiB，F407VGT6 为 1024 KiB。

| 目标或板型 | 可用路径及验证范围 |
|---|---|
| STM32F103C8T6 | UART ROM 烧录、完整读回、启动及回传已实板验证 |
| STM32F407VET6 | SWD 烧录、完整读回、启动及 H2 回传已实板验证，也是现有 Keil 示例 |
| STM32F407VGT6 | 已有 SWD 实现，真实 1 MiB 目标待验收 |
| STM32G431 | 已有 UART ROM 路径，实板验收待完成。底层 `flash_stm32.py` 能解析 RBT6，工作台 GUI、CLI、API 和 Keil 共用的芯片目录尚未列出 RBT6 |
| G431RBT6 与 ESP32 同板直连设计 | 新硬件需适配从机 GPIO、开漏 NRST、BOOT0 和空闲释放。G431 无线 SWD 仍待适配。现有 U4/U5 转换器版固件对应原从机 |

F407 的 H3 接 SWDIO、SWCLK、NRST、GND 和目标 3.3V 参考。SWO 可省略。H2 的 TX/RX 交叉连接目标 USART1 的 PA10/PA9。详细脚号见 [F407 SWD 接线表](F407_SWD_WIRING.md)。

## 2. 选择从机、调参和收发串口

主机双击 P 切换已登记从机，单击 P 无动作，旋转 P 调参。OLED 和工作台顶部显示所选名称。仅登记 GREEN 时显示 `GREEN 1/1`。主机 LINK LED 常亮表示链路有效，从机选中并同步后以身份色常亮，离线时呼吸。STEP 长按 2 秒开关主机整圈 RGB。新增从机时，在从机设置 `!identity`，再按真实 MAC 登记到主机。切换前结束烧录和在线串口会话，按需要先 STOP。见 [从机选择与颜色身份](PEER_SELECTION.md)。

读取 PID 状态用 `device`。需要修改时用 `pid --p ... --i ... --d ... --step ...`，确认需要持久保存时用 `save`。`run` 和 `stop` 的确认范围为从机，STM32 是否执行要看目标应用反馈。板上 SAVE 与 CLI `save` 写入主机 NVS。

H2 用于双向串口，H1 用于单向日志。波特率和数据格式与 STM32 应用一致，常见为 115200/8N1。UART ROM 烧录使用另一套 8E1 配置，由烧录程序管理。

- AI 旁观会话时用 `serial status`。需要读取时，先让 GUI 或原读取者关闭会话，再由 AI 打开自己的会话。GUI 的「暂停显示」仍会读取数据。
- 执行 `serial configure` 指定通道和非零的八位十六进制会话 ID。从游标 0 开始 `serial read`，每次使用返回的 `next_cursor`。输出 `chunks[].hex` 是原始字节，连续文本用增量解码。检查 `gap`、`dropped`、`rx_errors` 和暂停状态。
- 发送文本用 `serial send --text` 并按应用协议设置 `--ending`。发送二进制用 `--hex`。同一次发送重试使用同一 `--idempotency-key`。读取结束后执行 `serial close` 关闭自己的会话。

完整可复制的会话命令见 [AI 串口操作](../AI_USAGE.md#ai-读取无线串口) 和 [H1/H2 指南](WIRELESS_SERIAL.md)。HTTP 调用可用 `pidctl.cmd schema` 查看结构，用 `pidctl.cmd service` 获得凭据文件路径。将 token 留在本机客户端，CLI 会自行处理认证。见 [HTTP API](../AI_USAGE.md#http-api)。

## 3. 用工作台或 CLI 无线烧录

先确定芯片、接口和当前从机，再用 `plan` 校验 BIN。工作台和 CLI 的直接输入为链接到 `0x08000000` 的 BIN，预检会验证容量、初始向量和 SHA256。

以下为已验证的 F407VET6 SWD 示例。换成当次明确要写入的应用镜像，运行完 `plan` 再决定是否提交。`flash` 会擦除并重写镜像覆盖的扇区。

```powershell
$appBin = 'C:/firmware/app.bin'
$imagePlan = & .\PID_Workbench\pidctl.cmd plan --chip STM32F407VET6 --interface swd --firmware $appBin | ConvertFrom-Json
if (-not $imagePlan.ok) { throw $imagePlan.error.message }
$imagePlan.data
```

实际烧录时给本次意图分配一个任务键并记录。重试同一次请求时沿用该键。把上一步 SHA256 传入，固定已检查的镜像。

```powershell
$flashKey = 'f407ve-release-001'
& .\PID_Workbench\pidctl.cmd flash --chip STM32F407VET6 --interface swd --firmware $appBin --expected-sha256 $imagePlan.data.sha256 --idempotency-key $flashKey
```

提交后用 `pidctl.cmd job <任务ID>` 或 `pidctl.cmd wait <任务ID> --timeout 1800` 查询。成功条件为 `state=succeeded`、`result.verified=true`。请求启动时还要检查 SWD 的 `start_ack=true` 或 UART ROM 的 `go_ack=true`。需要只写入并校验时在提交时指定 `--no-run`。

后台记录包含固件快照、事件和结果。已知任务 ID 时查原任务。提交回应丢失时用相同请求和原任务键去重。`queued`、`running` 或等待超时仍需继续查原任务。明确失败后，先定位原因，再按用户的重烧意图创建新任务。全片擦除、解除读保护和改选项字节是独立操作，需要明确授权。

## 4. 在 Keil 里直接点 Download

已配置的示例工程为 `C:/Users/twice/Desktop/STM32PRO/f407/MDK-ARM/f407.uvprojx`，Target `f407`，芯片 `STM32F407VET6`。保持主机 USB 在线、从机无线连接和目标供电。Keil 中先 Build，再点 **Flash → Download**。适配器自动启动或复用工作台后台，GUI 可关闭。

外部下载器接受 AXF/ELF/HEX/BIN。AXF/ELF 用对应编译器的 `fromelf.exe` 导出带地址 HEX，再转换为固定 BIN。任务用转换后的镜像和 SHA256。Keil 的下载进度窗对应后台阶段，成功时输出 `Programming Done. Verify OK. Application running.`。此入口提供下载、读回校验和启动，单步或断点调试使用独立的调试器。

若迁移目录、换电脑或 CubeMX 重新生成了 Utilities，先构建启动器，再配置明确的工程与 Target。以下路径是原电脑示例，迁移时调整。配置脚本校验 Device，备份原 `.uvprojx` 后修改对应 Target 的 Utilities。已打开的 Keil 需重新加载工程。

```powershell
$pidPython = Join-Path $env:USERPROFILE '.platformio/penv/Scripts/python.exe'
$keilProject = 'C:/Users/twice/Desktop/STM32PRO/f407/MDK-ARM/f407.uvprojx'
$fromElf = 'C:/Keil_v5/ARM/ARMCC/bin/fromelf.exe'
& .\PID_Workbench\integrations\keil\build_launcher.ps1
& $pidPython .\PID_Workbench\integrations\keil\configure_keil.py --project $keilProject --target f407 --chip STM32F407VET6 --fromelf $fromElf
```

新编译产物可先用 `pid_keil.py --firmware <产物> --project <工程> --chip STM32F407VET6 --interface swd --fromelf <路径> --check` 做本地转换和校验。`--check` 保存检查记录，跳过提交和实板操作。

下载记录在目标工程的 `flash_records/keil/`，优先查看对应目录的 `attempt.json`、`job.json`、`events.log` 和 `fromelf.log`。结果不明时用 `pid_keil.py --resume <原 attempt.json>` 续查。失败窗的「重新烧录」及 `--retry-record` 都先核对原任务，仍在运行就继续等待，明确失败且收到重烧操作后才使用原镜像快照发起新任务。完整选项、凭据排查及恢复见 [Keil 接入说明](KEIL_DOWNLOAD.md)。

## 5. 升级各组件

### 升级前保留可回退状态

1. 确定改动范围。面板灯效通常只需主机，目标 GPIO/SWD 算法通常只需从机，共享协议、配对和 USB 协议变更则检查主机、从机、工作台及 Keil 脚本的版本兼容。
2. 记录改动前源码和可用的旧固件，保存主机当前 RAM 中的 P/I/D/STEP、保存状态、USB 推送状态和从机选择。原 NVS 参数、从机名单、工作台任务数据库与 API 凭据按原数据目录保留。
3. 编译和软件测试先通过。实际部署前确认任务已结束、串口会话已释放、目标应用处于允许维护的状态。需要更新 EXE 时，先关闭工作台窗口。
4. 主从沿用同一份本地配对密钥 `PID_Shared/PIDLink/private/pair_keys.h`。新克隆用 `prepare_keys.py` 准备密钥并同时部署两端。恢复原设备的配对时使用原密钥。配对文件、凭据、固件镜像和运行记录放在本机维护目录。源码在本地验证和保存，GitHub 推送按用户明确要求执行。

### 编译主机和从机

```powershell
$pidPython = Join-Path $env:USERPROFILE '.platformio/penv/Scripts/python.exe'
$pidPlatformIo = Join-Path $env:USERPROFILE '.platformio/penv/Scripts/platformio.exe'
& $pidPython .\PID_Shared\PIDLink\prepare_keys.py
& $pidPlatformIo run -d ESP32_S3_RGB_Chase -e host_flash
& $pidPlatformIo run -d ESP32_S3_Slave_Diagnostic -e slave_flash_psram
```

`prepare_keys.py` 保留已存在的密钥。`slave_flash_psram` 对应 N16R8 的 8 MB Octal PSRAM。不同型号板先核对 Flash、PSRAM、USB 身份及目标 GPIO 配置。正式升级选表中的环境，诊断和丢包注入环境留作专项验证。

### 写入 ESP32

完成备份并确认升级的板后，执行对应上传命令。主机的示例 COM14 应替换成刚按 USB 序列号确认的端口。从机的上传脚本按 `platformio.ini` 中的 `custom_usb_serial` 自动寻找同一设备，并核对 ROM 身份。换从机时修改该配置，再按新板 MAC 登记。

```powershell
& $pidPlatformIo run -d ESP32_S3_RGB_Chase -e host_flash -t upload --upload-port COM14
& $pidPlatformIo run -d ESP32_S3_Slave_Diagnostic -e slave_flash_psram -t upload
```

先尝试 USB 自动下载。若串口重枚举导致失败，重新按序列号定位。确认正确板及写入状态后，再使用该板 BOOT/RESET 进入 ROM 下载模式恢复。保留上传日志。重新启动后以主机 `!get`、`!link` 和从机 `!info`、`!link` 检查实际运行。使用原生 USB 串口发送诊断命令前，释放工作台和串口监视器的占用。

主机重启会从 NVS 加载参数。需要恢复升级前未保存的值时，通过 `!pid`、`!step` 或工作台恢复 RAM 值，再核对 `!get`；仅在用户要求持久保存时 SAVE。检查原来的从机名单、选择、同步状态与 USB 推送设置。

### 更新工作台和 Keil 下载器

```powershell
$env:PID_WORKBENCH_PYTHON = $pidPython
& $pidPython -m pip install -r PID_Workbench/requirements.txt
& .\PID_Workbench\build.ps1
& .\PID_Workbench\integrations\keil\build_launcher.ps1
```

工作台构建会将共享协议工具同步到 `backend/protocol`。部署时保留 EXE 与 `backend`、`integrations` 的目录关系。修改后台 Python 或协议工具后，还需要重启后台。确认队列空闲并退出客户端，读取 `http://127.0.0.1:18765/health` 的 PID，核对对应 Windows 进程确实在运行本仓库的 `backend/server.py`，再停止该进程。用 `pidctl.cmd service` 或重新启动 GUI 拉起新进程。保留原数据目录。仅更新 Keil 启动器时，原 Utilities 路径有效便可继续使用。桌面快捷方式可用 `PID_Workbench/install-desktop.ps1` 重新创建。

## 6. 验收与回退

先做和改动范围匹配的软件检查。以下测试从仓库根目录运行，使用软件模拟和测试夹具。

```powershell
& $pidPython scripts/run_native_tests.py
& $pidPython -m unittest discover -s PID_Workbench/tests
& $pidPython -m unittest discover -s PID_Shared/PIDFlash/tests -p 'test_*.py'
& $pidPython -m unittest discover -s ESP32_S3_Slave_Diagnostic/tests
```

| 改动范围 | 部署后的验收 |
|---|---|
| 主机面板或从机选择 | 对照 OLED、旋钮、LINK LED、`peer_name`、`peer_mac` 和参数同步，验证原 PID 值与选择 |
| 从机或无线协议 | 核对版本、目标接口空闲状态、链路和 H2 收发。传输测试可用暂存与校验，真实擦写用已确认的应用 |
| 工作台 | 查询 `service`、`capabilities`、`device`、`jobs`，检查 GUI 显示和现有任务记录 |
| Keil | 先跑 `--check`，授权实板下载时再用 Keil Download 验证任务结果、完整读回和启动，随后按目标应用协议验证串口 |

如果升级后异常，先保存任务与串口记录，再按受影响组件回退。ESP32 用已保存的旧固件或旧源码重建，配对及协议关联变更按已验证的版本组合恢复。工作台恢复对应 EXE、后台和协议工具，重启后台。Keil Utilities 从安装器保存的 `.uvprojx.before_pid_*.bak` 恢复对应 Target 设置，保留用户之后修改的其他工程内容。回退后重新检查设备和任务状态。

## 7. 常见故障从哪里查

| 现象 | 检查入口 |
|---|---|
| USB 端口消失或 `device_busy` | 查询 `ports`、`jobs` 和 `serial status`，核对主机序列号与当前占用 |
| Keil 报认证错误或缺少 `api_token.txt` | 用 `pidctl.cmd service` 查询本机服务和已验证的凭据路径，检查 `PID_WORKBENCH_DATA` 是否指向同一数据目录。详见 [Keil 凭据排查](KEIL_DOWNLOAD.md#keil-提示找不到-api_tokentxt) |
| 下载进度停住或回应丢失 | 读取 `attempt.json` 和 `job.json`，按原记录 `--resume`。已在执行的任务继续等终态 |
| 下载后 H2 没有日志 | 检查 `verified`、`start_ack`、会话暂停和恢复状态，确认应用已初始化 UART、参数匹配，再按应用协议发测试命令。见 [串口恢复验证](UART_RESUME_FIX_20261103.md) |

日常接口参数见 [AI 调用细则](../AI_USAGE.md)。构建依赖和迁移见 [开发与配置说明](../../docs/DEVELOPMENT.md)。最近一次多从机功能的实板范围见 [从机选择验收](PEER_SELECTION.md#已验证范围)。
