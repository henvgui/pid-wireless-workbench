# ESP32-S3 从机启动与指令测试

20261103.2：`slave_flash_psram` / `slave_flash` 增加 F407 SRAM 块烧录器和 `!ramcheck`（复位暂停目标，仅修改 SRAM）。正常烧录保持完整读回和启动确认，详见 [算法与回退说明](../PID_Shared/PIDFlash/RAM_PROGRAMMER.md)。

## 2026-10-23：USB 自动下载

在 VS Code 打开本工程，直接点 PlatformIO 的 **Upload**，或运行任务「从机：USB自动烧录（无需BOOT）」。默认环境为 `slave_flash_psram`，保留完整无线烧录、PID 转发和 PSRAM 支持。正常更新不需要手按 BOOT/RESET，完成后自动启动并通过 USB `!info` 确认应用运行。

```powershell
platformio run -t upload
```

`tools/usb_upload.py` 按从机 USB 序列号 `44:1B:F6:8D:10:18` 识别设备，再核对 ROM MAC，避免选到同时连接的主机。USB 复位使旧句柄失效时，重新按身份查找端口并同步；确认身份后保持同一个连接写入。采用已实测的 ROM 下载方式，关闭 RAM stub，保留写后哈希校验。当前完整写入约 45 秒。

烧录前关闭占用从机端口的串口监视器。记录保存在 `.pio/build/环境名/usb_upload.json`；只有写入校验和应用回读都成功才报告成功。自动进入模式最多尝试三次，写入开始后不会自动反复擦写。手动 BOOT 仅作为无法自动连接时的恢复办法。

2026-10-23 最新复验：用户更换 F407 后，当前从机 `20261023.2` 在不改固件、不解除保护的情况下，通过原工程 8880 字节的无线 SWD 写入、完整读回和启动确认，任务 `ccceb936d25638e73bdc59c1`，Flash 实读 512 KiB、RDP=0xAA、重试 0 次，用时约 12.44 秒。记录见 `../PID_Workbench/validation/verify_replacement_f407_20261023/`。ESP32 USB 自动下载也已通过。

此前旧 F407 目标断电后读到 `FLASH_OPTCR=0x0FFFFFED`（RDP=0xFF），相关恢复没有通过持久化验收，具体故障原因未独立确认。这是旧目标的记录，不能视作新目标的当前状态。普通烧录继续保持保护与容量检查，不自动解除保护。

排错新增 `!swdprobe`：通过 H3 执行复位并暂停 F407，以 5µs 半周期读取固定诊断地址，再通过 Cortex-M 软件复位复核；不擦写 Flash/RAM，也不启动应用。响应的 `values` 和 `after_reset` 依次是 DBGMCU_IDCODE、DHCSR、FLASH_ACR、FLASH_SR、FLASH_CR、FLASH_OPTCR、0x1FFF7A20 容量字、0x08000000/04/08 向量字、0x1FFF7A10 UID。遇错即停，后续零值不表示成功读取。`!swd` 返回最近的烧录/恢复诊断。

人工恢复入口为 `!swdunprotect ERASE_ALL_F407VE`，只接受已确认的 VE 型号、0x413 设备 ID、两次一致的 RDP=0xFF。它会解除读保护并擦除整片 STM32 用户 Flash，保留其他选项配置；需先取得用户明确同意，不能作为自动排错、常规下载或重试步骤。完成状态由 `!swd` 的 `recovery_state` 判断：0未触发、1进行中、2完成、3失败。`20261023.2` 及以后在恢复末尾重新复位并读取选项字节，防止将影子寄存器即时读回 AA、复位又变 FF 的情况误判为成功。完成仍不等于目标程序已恢复，还需检查容量、重新烧录、完整读回和启动验收。超时或失败不自动重放。

2026-10-19 当前从机 COM18 已安装 `slave_flash_psram`，8MB PSRAM 已启用。F407VET6 的无线 SWD 写入、完整读回、启动及 131091 字节串口回传已实板通过，见 [F407 测试记录与接线](../STM32F407_UART_ECHO/README.md)。SWD CLK/DIO/SWO 已校正为 GPIO12/13/14；VGT6 和 UART ROM 路径仍待实板验收。`!swd` 只读返回最近一次握手的 `ack`、`dpidr`、`connect_step` 和错误。下列 2026-10-18 / 2026-10-16 记录对应历史版本。

2026-10-18 当前从机已安装 `slave_flash`，包含 F103/G431 UART 烧录和 F103 文本回传。重新上传这一版本请使用任务「从机：STM32 烧录版（F103/G431）：烧录」；使用入口见 [F103 回传测试说明](../STM32F103_UART_ECHO/README.md)。

基础 `slave_wireless` 环境不包含目标烧录/文本回传扩展，详见 [主从无线使用说明](../ESP32_S3_RGB_Chase/README_WIRELESS.md)。普通编译/烧录现已默认完整 `slave_flash_psram`。任务「从机：烧录诊断版（关闭无线）」可恢复以下 `slave_diagnostic` 测试。下文 2026-10-16 的验证记录对应关闭无线的历史版本。

当前无线版状态灯已调整为：未连接绿色呼吸，连接且收到有效主机参数快照后绿色常亮；RUN/STOP 不改变灯色。`!status` 的自动模式为 `link_breath` / `link_solid`；手动 `!rgb` / `!off` 仍可覆盖灯光，`!auto` 恢复连接指示。

依据 `C:/Users/twice/Desktop/SCH_Schematic2_2026-10-16.pdf`，2026-10-16。原图副本保存在 [reference/slave_schematic_20261016.pdf](reference/slave_schematic_20261016.pdf)。

独立 PlatformIO 工程、VS Code 任务、编译及命令解析测试已通过。已识别从机为 COM18（USB 序列号 `44:1B:F6:8D:10:18`），成功烧录并通过写入校验，19 项真实 USB 指令检查通过；随后 30 秒连续观察的 31 个状态样本启动标识一致、运行时间持续增加，未发生断线或重启。用户现场确认「绿色呼吸正常」。

## 打开与烧录

在 VS Code 打开 `ESP32_S3_Slave_Diagnostic.code-workspace`。关闭无线的诊断版用 `platformio run -e slave_diagnostic` 编译，任务「从机：烧录诊断版（关闭无线）」上传，「从机：USB监视」查看输出。上传按 USB 身份自动选择端口；监视器当前端口为 COM18，重新分配时修改 `monitor_port`。烧录前关闭占用该端口的监视器。

仅当自动连接失败而需要手动恢复时：按住 SW8（BOOT），按下并松开 SW7（RESET），等待约 1 秒后松开 SW8。正常 Upload 会自动进入下载模式并复位运行。

本项目为从机单独配置；主机的代码、默认环境和烧录任务未修改。

## 看什么算正常

烧录后从机状态 RGB 每秒绿色呼吸一次。USB 或 UART0 以 115200 接收命令，开机/连接时输出一行 `ready` JSON。

- `!ping` 能返回 `pong`，说明固件正在处理命令。
- 间隔数秒发 `!status`，`uptime_ms`、`loops` 和 `rgb_frames` 应增加，`boot_id` 保持相同；运行时间反复归零或 `boot_id` 改变说明期间发生了重启。
- `rgb_error=0`、`rgb_failures=0` 表示 RGB 驱动调用正常；灯珠是否真的发光需现场观察。
- `!info` 返回芯片型号、版本、实际 Flash 容量和复位原因。N16R8 预期 Flash 为 16,777,216 字节。

这些检查确认启动、程序循环、通信和 RGB 输出；本测试不初始化 PSRAM，不进行射频或完整 Flash 存储测试。

## 指令

命令以英文 `!` 开头，每条以 LF 或 CRLF 结束，命令名小写。USB 与 H1 UART0 均支持同一套指令，回复只发到接收该命令的接口。

| 指令 | 结果 |
|---|---|
| `!ping` | 返回 `pong`、递增序号、启动标识和运行时间 |
| `!info` | 固件/芯片信息、Flash、复位原因和当前接口 |
| `!status` | 运行时间、循环计数、堆内存、RGB 状态及 OE 电平 |
| `!pins` | 返回依据原理图核对的 GPIO 分配 |
| `!rgb 255 0 0` | 状态灯红色常亮 |
| `!rgb 0 255 0` | 绿色常亮 |
| `!rgb 0 0 255` | 蓝色常亮 |
| `!rgb 255 255 255` | 白色常亮 |
| `!off` | 熄灭 RGB，固件和通信继续运行 |
| `!auto` | 恢复绿色呼吸心跳 |
| `!watch on` / `!watch off` | 当前接口每秒上报一次状态 / 停止自动上报 |
| `!help` | 返回指令列表 |

普通字符直接回显，例如发 `1` 回 `1`。RGB 三项为 0～255 的整数，输出统一按 32/255 亮度缩放；自动上报默认关闭。长命令会拒绝整行，未换行的命令在 2 秒后超时，不会执行半条指令。

例如：

```text
!info
!ping
!watch on
```

观察数秒，确认 `uptime_ms` 持续增加，随后发 `!watch off`。

## 已核对的原理图连接

| 功能 | GPIO 或连接 |
|---|---|
| USB D− / D+ | GPIO19 / GPIO20，22Ω 串联电阻及 USBLC6-2SC6 |
| EN | R2 10k 上拉、C1 1µF 对地、SW7 复位 |
| BOOT | GPIO0，R3 10k 上拉、SW8 接地 |
| 状态 RGB | **GPIO38，模组第 31 脚**，经 U3 电平转换和 R22 470Ω |
| U4/U5 OE# | **GPIO47**，高电平关闭对外输出；与主机 RGB 引脚用途不同 |
| UART0 调试口 H1 | 1脚 3V3；2脚 RX/GPIO44；3脚 TX/GPIO43；4脚 GND |
| 对目标板 UART TX/RX | GPIO17 / GPIO18 |
| 目标 NRST / BOOT0 | GPIO15 / GPIO16 |
| SWD CLK / DIO / SWO | GPIO12 / GPIO13 / GPIO14 |
| SWD 方向 | GPIO21 |
| I²C SDA / SCL | GPIO8 / GPIO9 |
| EXT1～4 | GPIO4 / GPIO5 / GPIO6 / GPIO7 |

固件先把 GPIO47 置高以关闭 U4/U5，再初始化其他功能。目标 NRST/BOOT、SWD 和 EXT 引脚保持输入，不发送目标板控制信号。GPIO35/36/37 不使用，它们在 N16R8 上连接模组的 Octal PSRAM。

H1 是 ESP32 自身的调试 UART0，H2 是经过电平转换的目标板接口；本轮备用串口使用 H1。

## 本次枚举与下载记录

最初电脑未枚举出从机 USB 设备，供电 3V3 及 EN 实测均约 3.3V。后来 COM18 出现，但读取时断开重连；用户保持 SW8 按下并复位后，成功读取芯片并烧录。芯片为 ESP32-S3 rev0.2，16MB Flash、内置 8MB PSRAM。松开 SW8 再复位后，新固件开始正常回应 USB 命令。

若以后 USB 再次无法识别，可用 USB 转 TTL 读取启动日志：从机继续 USB 供电，模块 GND 接 H1 的 GND（4脚），模块 RXD 接 H1 的 TX（3脚），以 115200 监听后按 SW7。先只接这两根线，模块的 TXD 和电源脚不接。本次无需 TTL 已完成 USB 烧录与指令测试，UART0 实物收发仍未测试。

需要经 UART 烧录或双向发命令时，先确认 TTL 的 **TXD 实测为 3.3V 逻辑电平**，再把它接 H1 的 RX（2脚）。不向从机 3V3 引脚接入 5V；BOOT/RESET 仍使用 SW8/SW7。

## 验证记录与自动检查

- `validation/build_20261016.txt`：PlatformIO 编译成功。
- `validation/native_20261016.txt`：命令参数、颜色范围、错误拒绝及引脚区分测试通过。
- `validation/preparation_20261016.json`：最初准备阶段的历史记录，当时尚未烧录。
- `validation/chip_identification_boot_held_20261016.txt`、`upload_20261016.txt`：芯片应答、Flash 容量及烧录校验。
- `validation/serial_result.json`：19 项实际 COM18 检查通过，含应答、状态、颜色命令、错误处理、12 次连续查询及普通字符回显。
- `validation/stability_20261016.json`：30 秒连续运行、USB、RGB 驱动及 OE 状态检查通过。
- `validation/acceptance_20261016.json`：当前已烧录及验证状态摘要。
- `validation/verify_serial.py`：重跑实际串口检查，不会烧录固件。

端口识别后可运行：

```powershell
& "$env:USERPROFILE/.platformio/penv/Scripts/python.exe" validation/verify_serial.py --port COM实际编号
```

脚本测试结束恢复绿色呼吸，关闭自动上报。其 JSON 报告写入 `validation/serial_result.json`，不会烧录固件。

硬件依据：[ESP32-S3-WROOM-1 官方数据手册](https://documentation.espressif.com/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)、[SN74AVC4T774 官方数据手册](https://www.ti.com/lit/ds/symlink/sn74avc4t774.pdf)、[SN74AHCT1G125 官方数据手册](https://www.ti.com/lit/ds/symlink/sn74ahct1g125.pdf)。本次只核对测试所需的关键连接，未查看这版 PCB 的实际走线或焊接情况。
