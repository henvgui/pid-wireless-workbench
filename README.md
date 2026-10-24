# PID Wireless Workbench

ESP32-S3 无线 PID 调试、STM32 固件烧录与双向串口工作台。主机负责旋钮、OLED、RGB 和 USB；从机通过 ESP-NOW 接收任务，再经 UART / SWD 连接目标单片机。Windows 上位机提供 GUI、CLI 和本地 HTTP API，便于人和 AI 使用。

```text
电脑 GUI / CLI / HTTP API
          │ USB
     ESP32-S3 主机
          │ 加密 ESP-NOW
     ESP32-S3 从机
          ├─ H2 UART：日志、命令、STM32 ROM 烧录
          ├─ H3 SWD：STM32 调试接口烧录
          └─ H1 UART：独立单向日志
```

## 文档入口

| 内容 | 文档 |
|---|---|
| 日常界面与使用指南 | [工作台 README](PID_Workbench/README.md) |
| AI 调用、串口读取游标及重试规则 | [AI 操作指南](PID_Workbench/AI_USAGE.md) |
| H2 双向串口、H1 日志与接线 | [无线串口操作指南](PID_Workbench/docs/WIRELESS_SERIAL.md) |
| 在 Keil 中点击 Download 无线烧录 | [Keil 接入](PID_Workbench/docs/KEIL_DOWNLOAD.md) |
| F407 的 SWD 接线 | [F407 接线表](PID_Workbench/docs/F407_SWD_WIRING.md) |
| 已验证范围、已修问题与限制 | [检查记录](PID_Workbench/docs/PROJECT_AUDIT_20261024.md) |
| 首次构建、更换电脑与源码管理 | [开发与配置说明](docs/DEVELOPMENT.md) |

## 当前功能

- 主机：P/I/D/STEP 编码器、OLED、RUN/STOP、SAVE、本地参数保存、渐变呼吸/拖影 RGB、STEP 长按开关 RGB。
- 主从：加密 ESP-NOW、状态同步与连接指示。
- 无线烧录：从机 UART ROM 或 SWD 写入，镜像 SHA256 检查、完整读回校验、任务去重、Keil 接入及进度显示。
- 无线串口：H2 双向文本/HEX、串口格式与文本结尾、发送重试去重、烧录暂停/恢复；H1 单向日志。
- 上位机：Windows GUI、CLI、本机认证 HTTP API；支持后台断线恢复。

实板目标为 STM32F103C8T6（UART）与 STM32F407VET6（SWD）。F407VGT6、G431 及其他接口的实际验收范围见文档，不能将“已有算法”直接视为实板验证。目标应用仍需自行处理打印、命令及 PID；主从同步 ACK 不能代表电机已执行操作。

## 源码布局

| 目录 | 用途 |
|---|---|
| `ESP32_S3_RGB_Chase` | ESP32-S3 主机 PlatformIO 工程；默认 `host_flash` |
| `ESP32_S3_Slave_Diagnostic` | ESP32-S3 从机工程；默认 `slave_flash_psram` |
| `PID_Shared/PIDLink` | ESP-NOW 链路与本机配对密钥生成工具 |
| `PID_Shared/PIDFlash` | 暂存、UART ROM、SWD、无线串口、回传协议与测试 |
| `PID_Workbench` | Windows 上位机、API/CLI、Keil 适配器及手册 |
| `STM32F103_PA3_HIGH` | F103 最小 GPIO 示例及回传示例所需启动代码 |
| `STM32F103_UART_ECHO` | F103 回传测试应用 |
| `STM32F407_UART_ECHO` | F407VET6 回传测试应用 |
| `scripts` | 不操作硬件的测试入口 |

用户的车辆业务工程与原始 PCB 工程没有复制进本仓库；本机历史实验、原始验证日志、打包备份和编译产物保留在原目录中。

## 快速构建

需 Windows .NET Framework 4.8、Python 3.11（含 pyserial）、PlatformIO；构建 STM32 示例另需 Arm GNU 工具链。PlatformIO 工程固定使用 `espressif32@7.0.1`。

```powershell
python -m pip install -r PID_Workbench/requirements.txt
python PID_Shared/PIDLink/prepare_keys.py
pio run -d ESP32_S3_RGB_Chase -e host_flash
pio run -d ESP32_S3_Slave_Diagnostic -e slave_flash_psram

$env:PID_WORKBENCH_PYTHON = (Get-Command python).Source
& .\PID_Workbench\build.ps1
& .\PID_Workbench\PID_Workbench.exe
```

以上 ESP32 命令仅编译。接入其他板子前先核对 MAC、USB 身份、端口、引脚和供电，再明确执行上传；当前代码绑定这套已验证的主从硬件。

**配对密钥不会上传 GitHub。** 原电脑已存在的 `PID_Shared/PIDLink/private/pair_keys.h` 保持原样；新克隆中运行生成工具会创建新密钥，主从必须使用同一份密钥并同时更新，不能只更新一侧。

## 软件测试

```powershell
python -m unittest discover -s PID_Workbench/tests
python -m unittest discover -s PID_Shared/PIDFlash/tests -p 'test_*.py'
python -m unittest discover -s ESP32_S3_Slave_Diagnostic/tests
python scripts/run_native_tests.py
```

原生测试需要 `g++`，可用 `--compiler` 指定路径；它们不访问真实串口或烧录目标。GUI 与实板故障测试属于单独的维护验收，不应在用户正在接收日志或烧录时并行执行。

AI 读取串口前，应先让 GUI 关闭串口接收；目前 GUI 和 AI 共用消费游标，独立订阅缓存尚未实现。仅查询状态可以并行。详见 AI 操作指南。
