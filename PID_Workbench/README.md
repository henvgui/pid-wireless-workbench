# PID 调试工作台

从机 20261103.3：修复 F407 SWD 下载后串口无响应、需整机断电恢复的问题。启动确认增加系统复位和 Flash 程序入口/中断检查；同一 H2 会话连续下载 3 次及 `S` 指令收发均通过。无需改 Keil 或上位机配置。[修复记录](docs/UART_RESUME_FIX_20261103.md)。

2026.11.03.2：F407 SWD 默认使用 SRAM 1 KiB 块烧录器，GUI / Keil 沿用原入口；保留手动旧算法、完整读回及启动确认。VET6 同一 10,268 B 镜像的 Keil 任务由 9.85 s 缩短至 4.09 s，约 2.4 倍。[算法与回退说明](../PID_Shared/PIDFlash/RAM_PROGRAMMER.md)、[实测记录](docs/RAM_VALIDATION_20261103.md)。

2026-11-03：固件传输已加入 USB 二进制批量、8 包滑动窗口和缺包补发，GUI / Keil 自动协商启用；旧固件继续使用逐包方式。实板正常传输约 27–28 KiB/s，这是到从机暂存区的速度，目标擦写与读回另行计时。[本轮实测与限制](docs/BULK_VALIDATION_20261103.md)。

## 操作指南入口

| 要做的事 | 阅读文档 |
|---|---|
| 启动、界面操作、烧录与工程依赖 | 本 README |
| 给 AI / Codex / Claude 调用 CLI 或本地 API | [AI_USAGE.md](AI_USAGE.md) |
| H2 双向无线串口、H1 日志、接线与发送命令 | [无线串口操作指南](docs/WIRELESS_SERIAL.md) |
| Keil 点击 Download 经 ESP-NOW 烧录 | [Keil 接入说明](docs/KEIL_DOWNLOAD.md) |
| F407 的 SWD 接线 | [F407 接线表](docs/F407_SWD_WIRING.md) |
| 已修问题、测试结果及当前限制 | [项目检查记录](docs/PROJECT_AUDIT_20261024.md) |

### 无线串口快速开始

1. 主机 USB 连接电脑，从机和 STM32 正常供电；H2 的 TX→目标 RX、RX←目标 TX，并共地、保留 VTREF 接线。
2. 双击桌面「PID 调试工作台」，打开「03 无线串口」。
3. 选择「H2 双向串口」，设置与目标程序一致的参数；当前 F407 为 **115200 / 8N1**，然后点「连接串口」。
4. 上方接收日志，下方可发文本或 HEX；按目标协议选择无结尾、CR、LF 或 CRLF。
5. 发送结果不确定时用「重试上次」保留原编号；正常需要再发一次则点「发送」。
6. 停止读取请点「关闭串口」。「暂停显示」仍继续接收。

STM32 程序需要自行打印和处理命令；透传不会让任意程序自动回显。目标处于暂停状态时，也不会产生应用日志。

**AI 读取前，先在 GUI 点「关闭串口」，由 AI 单独管理会话和游标。** 当前 GUI 和 AI 共用接收会话，没有独立订阅缓存；同时推进游标可能影响另一方接收。只查询 `serial status` 可以并行，详细步骤见 [AI 串口读取流程](AI_USAGE.md#ai-读取无线串口)。

## 版本与功能

2026.10.24.3：完成项目检查和本轮实板回归，修正服务断线恢复、主动关闭串口、凭据路径、传输失败暂存释放与过期状态提示，并加固共享库指令调度。两端 ESP32 和上位机已更新，见 [检查与实板验证记录](docs/PROJECT_AUDIT_20261024.md)。

2026.10.24.2：新增 **03 无线串口**，支持 H2 双向发送/接收、文本/HEX、CR/LF/CRLF、发送编号去重、烧录自动暂停与恢复；保留 H1 日志。两端 ESP32 固件已升级，详情见 [无线串口使用说明](docs/WIRELESS_SERIAL.md)。

2026.10.23.2：串口接收增加断线重试、从机会话失效后自动重建，并保留已有日志；新增 `serial status` / `/v1/serial/status` 状态查询。重建时明确提示中断期间可能缺失数据。

H1 日志模式：从机 H1 RX/GPIO44 接收 STM32 输出，经 ESP-NOW 和主机 USB 显示。支持波特率、数据位、校验位、停止位、文本/HEX、时间戳、暂停、清空和保存。H1 接线见 [串口日志使用说明](docs/SERIAL_LOG.md)。

Windows 本地上位机，专门服务这套 ESP32 主从机硬件，支持无线 PID 调参、目标板烧录和数据回传。双击 `PID_Workbench.exe` 启动；桌面快捷方式名为 **PID 调试工作台**。

已提供 [Keil Download 按钮接入](docs/KEIL_DOWNLOAD.md)：Keil 调用后台适配器，将当前 AXF/HEX 转为固定 BIN 快照，再经主机 USB → ESP-NOW → 从机 SWD 下载、完整读回校验和启动，无需打开 GUI。当前 `f407` 工程配置为 STM32F407VET6；重新打开工程一次后生效。

2026-10-22：Keil 下载增加独立实时进度窗，显示真实阶段、百分比、活动动画和耗时，成功后自动收起。即使 Keil 等到外部程序结束才刷新日志，也能持续看到下载状态；不弹空白终端。

完整操作见 [中文使用手册](docs/USAGE_MANUAL.md)，可打印版见 [使用手册 PDF](output/pdf/PID项目使用手册_V1.2_2026-10-19.pdf)。新增 [F407 无线 SWD 接线与使用](docs/F407_SWD_WIRING.md)。

所有目标操作统一经过：**电脑 → 主机 USB → ESP-NOW → 从机 → 目标接口 → 目标 MCU**。界面选择的是从机到目标板的 UART / SWD 接口。其他接口在从机协议与驱动实现后扩展。

深色界面、PerMonitorV2 DPI 适配。GUI、命令行和本地 HTTP API 共用一套任务引擎，AI 可直接选择芯片、检查固件、提交烧录、查询结果和做文本回传，不需要操作鼠标。

## 使用界面

1. 打开「固件烧录」，选择芯片、从机目标接口及主机 USB 端口。
2. 选择对应芯片的 `.bin` 文件，链接地址必须为 `0x08000000`。
3. 「检查镜像」验证文件、向量表、容量范围和 SHA256，不写目标。
4. 「开始烧录」直接提交选定操作。任务采用固件快照，读回一致后才发送所选启动请求。
5. 在日志和「结构化结果」查看结果，可打开任务目录或复制 JSON。按钮执行期间界面保持响应。

本机主机 USB 通常为 COM14，从机 USB 为 COM18；重插可能改变编号，选择「自动识别 ESP32 主机」即可。所有目标接口共用这个主机连接，从机通过 ESP-NOW 接收任务。端口列表只提供主机；从机 USB 用于维护从机固件。

当前芯片目录：STM32F103C8T6、STM32G431CBT6、STM32G431C8T6、STM32G431C6T6、STM32G431KBU6、STM32F407VGT6、STM32F407VET6。F407 默认从机 SWD，无需 BOOT0；F407 UART 亦已适配。F407VET6 的 SWD 烧录与回传已实板通过；VGT6 与 F407 UART ROM 待验收。GD32、其他芯片尚未接入。GUI/API 直接输入仍只接受 BIN，Keil 适配器另支持 AXF/ELF/HEX 转换。

## 从机目标接口

| 接口 | 路径 | 当前状态 |
|---|---|---|
| UART | 主机 USB → ESP-NOW → 从机 UART → STM32 UART ROM | 可用：F103C8T6 实板写入、完整读回和 GO 通过；G431 待目标实物验收 |
| SWD | 主机 USB → ESP-NOW → 从机 SWCLK/SWDIO → 目标 SWD | F407VET6 实板写入、完整读回、启动与回传通过；VGT6 模拟通过、待实板验收；其他芯片 SWD 暂未适配 |

其他接口暂不列为可执行烧录方式。接入时需要同时完成从机驱动、芯片算法、无线任务协议和读回验证。电脑端安装 ST-Link/CubeProgrammer 不能替代从机的 SWD 固件。

无线接线：从机 H2 的 VTREF→目标3.3V，TX→目标 PA10，RX→目标 PA9，BOOT→F103 独立 BOOT0（G431 为 PB8），NRST→目标 NRST，GND→目标 GND。F103 BOOT1/PB2 保持低。

UART 烧录由从机自动控制 BOOT0、NRST，以 115200/8E1 访问 ROM。F407 SWD 使用 H3：1 VTREF→3.3V、2 DIO→PA13、3 CLK→PA14、4 GND→GND、6 NRST→NRST；5 SWO 可不接。SWD 不需要 BOOT0。超过 128 KiB 的镜像需从机 `slave_flash_psram`；全部功能需同步更新两端固件。

F103C8 始终限制 64 KiB Flash / 20 KiB RAM；兼容实板容量寄存器返回 128 KiB，但不会使用后面的未标称空间。不会自动解除读写保护或改选项字节。

## PID 与文本回传

「PID 参数」支持读取和设置 P/I/D/STEP、RUN、STOP、保存和载入主机参数。同步确认目前来自从机；目标 MCU 的 PID 参数与执行反馈协议待接入，不能把从机 ACK 当成电机已运行。

「文本回传」支持输入中文、选择文件或生成 128 KiB 文本。目标需运行对应 `STM32F103_UART_ECHO` 或 `STM32F407_UART_ECHO` 应用；当前 F407 测试程序按 VET6 构建。成功须满足逐块内容、总字节数、唯一数据块数、CRC32 和整文件 SHA256 全部一致。

烧录或回传任务先 STOP，不自动恢复 RUN。界面上次选择保存在 `%LOCALAPPDATA%/PIDWorkbench/ui_settings.json`，不会因此改写主机已保存的 PID 参数。

## AI 操作

详见 [AI_USAGE.md](AI_USAGE.md)。命令行入口为 `pidctl.cmd`；输出 JSON，并用退出码表示成功或失败。也可直接调用 `backend/pidctl.py`。

```powershell
./pidctl.cmd capabilities
./pidctl.cmd ports
./pidctl.cmd device
./pidctl.cmd flash --chip STM32F103C8T6 --interface uart --firmware 'C:/firmware/app.bin' --idempotency-key project-build-001 --wait
./pidctl.cmd echo --text '你好，STM32' --idempotency-key echo-test-001 --wait
```

本地接口为 `http://127.0.0.1:18765`，由 GUI/CLI 自动启动；仅监听本机，使用本地 token 认证。关闭窗口不强行中断正在运行的任务，AI 可继续查询。无需安装 Windows 服务或管理员权限。

同一个任务键和同一请求只执行一次，完成或失败后再提交会返回原记录。更换固件、参数或明确开始新任务时用新键。服务意外退出时，未完成任务标为 `interrupted`，不自动重烧。

## 任务与文件

记录在 `%LOCALAPPDATA%/PIDWorkbench/jobs/任务ID/`：固件快照、事件日志、烧录/回传结果、原文和回传数据。`jobs.sqlite3` 保存任务状态与去重键。

只有 `state:succeeded` 且相应 `result.verified:true` / `result.passed:true` 才能视为验证成功。UART 若选择启动还需 `go_ack:true`，SWD 则要求 `start_ack:true`。`queued`、`running` 或高进度均不代表已完成。中断或失败后先看结果，不自动重复擦写。

## 构建与依赖

本机可直接运行。依赖 Windows .NET Framework 4.8 和已有 PlatformIO Python（含 pyserial）。源码及协议工具随工程提供，`./build.ps1` 同步协议工具并用 Windows 自带 C# 编译器构建。

其他机器可通过 `PID_WORKBENCH_PYTHON` 指向装有 pyserial 的 Python。`PID_WORKBENCH_DATA` 供后端测试隔离数据使用；正常桌面界面使用默认 LocalAppData 目录。

本项目保留旧 RGB 上位机，当前烧录工作台使用新的桌面图标；两者操作同一串口时需先关闭旧程序连接。

验证记录位于 `validation/`：后端协议/任务/API 测试、GUI 页面与 DPI 检查，以及真实 AI 无线烧录、幂等重试、参数同步和中文回传。

2026-10-18 接口模型修正：已移除早期误加的电脑直连 USB-TTL / ST-Link 入口；旧任务记录保留。旧 `method:wireless_uart` 请求仍兼容，新请求推荐使用 `transport:esp_now` 和 `target_interface:uart`。早期验证记录里关于电脑直连适配的内容不代表从机拥有 SWD 烧录能力；`backend/rom_uart.py` 仅保留作旧 ROM 协议回归参考，不接入任务引擎。
