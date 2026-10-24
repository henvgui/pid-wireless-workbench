# STM32 无线 UART / SWD 烧录（F103 / G431 / F407）

2026-10-22 SWD 提速：从机完整固件的 `PID_FLASH_SWD_HALF_PERIOD_US` 设为 2，旧值为 5。当前 F407VET6 同一份 8,708 字节镜像实测由 20.08 秒缩短为 12.60 秒，完整读回、设备和容量检查全部保留，任务 `6fc4bd9178dc915eabd15be0` 的 verified/start_ack 均为 true。需更新从机 `slave_flash_psram`，主机和 Keil 配置无需修改。长线不稳定时可将编译参数恢复为 5；失败不自动重放擦写。完整记录见 `../../PID_Workbench/validation/speed_benchmark_20261022/`。

2026-10-19 支持 **STM32F407VET6（512KB）/ VGT6（1MB）**：UART ROM 与从机原生 SWD 两条路径。F407VET6 已实板通过无线 SWD 写入、完整读回、启动和 131091 字节串口回传；VG 和 F407 UART ROM 路径待实测。H3 的 SWDIO、SWCLK、NRST、VTREF、GND 无需 BOOT0；从机 CLK/DIO/SWO 实际是 GPIO12/13/14。完整接线见 [F407 SWD 使用说明](../../PID_Workbench/docs/F407_SWD_WIRING.md)。型号、设备 ID 0x413 和容量检查绑定目标；SWD 使用 `start_ack`，不以 UART `go_ack` 判定成功。

新增从机 `slave_flash_psram` 环境，适用于当前 N16R8；大于 128 KiB 镜像在 PSRAM 暂存，最大 1 MiB。普通 `slave_flash` 保留内部 RAM 小镜像方式。需同步更新主机 `host_flash` 和从机固件。新清单可选携带接口（`!fw begin ID SIZE KB SHA f407vg swd`），接口参与重试清单一致性检查；旧 UART 清单及确认保留兼容。

VET6 使用 `f407ve` / profile 4 / 512KB；VGT6 保留 `f407vg` / profile 3 / 1024KB，既有 G431 profile 1 和 F103 profile 2 不变。

以下 2026-10-18 及更早段落保留历史 F103/G431 验收记录；新的 SWD 实现不改变这些记录的实测范围。

更新：2026-10-18。当前目标切换为 **STM32F103C8T6**，保留 G431 适配。电脑通过主机原生 USB 发送 `.bin`，主机经已有加密 ESP-NOW 链路传给从机；从机将完整文件暂存在内部 RAM，SHA256 和向量表通过后，才允许执行 STM32 系统 ROM UART 烧录。

同日新增 [F103 大文本双向回传测试](ECHO.md)：电脑文本经无线链路到 F103 后从 UART 原样返回，逐块、整文件及目标侧流统计共同验证。与烧录互斥，使用已有 `host_flash` / `slave_flash` 环境。

**默认目标为 STM32F103C8T6：64 KiB Flash、20 KiB SRAM、1 KiB 擦除页。** ROM 设备 ID 0x410，容量寄存器 0x1FFFF7E0。本次实板返回 128 KiB：兼容 64/128 KiB 两种容量回报，但 C8 的镜像、擦除和写入仍限制在前 64 KiB，不使用未标称空间；其他容量回报拒绝。G431x6/x8/xB 的 32/64/128 KiB 适配保留，实际容量必须与所选料号一致。所有镜像从 0x08000000 开始，不支持 HEX/ELF、偏移应用或其他 MCU。必须用对应 MCU 编译的 `.bin`，切换命令中的料号不能把 G431 固件转换成 F103 固件。

文件清单新增明确的 `f103c8` / `g431` 目标标识；该标识参与无线包 CRC、重复清单核对和接收确认。相同传输 ID 不允许换目标；电脑也核对从机回报的目标，避免仅靠同为 64 KiB 的容量误选芯片。切换适配时两块 ESP32 都需要编译新文件协议版本。

## 工程选择

原主机 `host_wireless` / 从机 `slave_wireless` 默认环境保持原来的功能；新增环境为显式选择：

| 环境 | 工程 | 用途 |
|---|---|---|
| `host_flash` | ESP32_S3_RGB_Chase | 含 USB 文件指令及无线分块发送 |
| `slave_flash_transport` | ESP32_S3_Slave_Diagnostic | 接收、暂存及 SHA256 校验，目标输出编译关闭 |
| `slave_flash` | ESP32_S3_Slave_Diagnostic | 包含实际 F103C8/G431 UART 编程，仅显式 `flash ... ERASE` 才启用接口 |
| `host_flash_loss_test` / `slave_flash_loss_test` | 对应两板 | 每第 5 个文件协议发送包主动丢弃；从机目标输出关闭，专用于重传验证 |

在各自工程用 PlatformIO `run -e 环境名` 编译，`run -e 环境名 -t upload` 烧录对应 ESP32。VS Code 添加了相应的编译与烧录任务。测试环境不作为日常使用默认。

## 接线与进入 ROM

依据当前从机 H2 接口，以下是 **从机视角** 的 TX/RX：

| H2 | 标注 | 接目标 STM32F103C8T6 |
|---|---|---|
| 1 | VTREF | 已供电目标板的 3.3V，给电平转换器目标侧供参考 |
| 2 | TX | PA10（USART1_RX） |
| 3 | RX | PA9（USART1_TX） |
| 4 | BOOT | 独立 BOOT0 脚（LQFP48 第 44 脚） |
| 5 | NRST | NRST 复位脚 |
| 6 | GND | 目标 GND |

**F103 的 BOOT1/PB2 必须保持低电平**（开发板 BOOT1 跳帽设 0，或按目标板电路接下拉）。BOOT0 由从机控制，不要用跳帽固定到 GND 或 3.3V。F103 的 BOOT0 不是 PB8；此前 G431 的 BOOT 线若接 PB8，必须改接独立 BOOT0。TX/RX 交叉、双方共地，目标自行正常供电，VTREF 不接 5V。

F103 中容量 ROM 支持 USART1 的 PA9/TX、PA10/RX，程序为 115200、8E1，发送 0x7F 自动波特率握手。保留 G431 时才使用 PB8/BOOT0；其余 UART1/NRST/VTREF/GND 接线一致。

GPIO17/18 为从机 UART TX/RX，GPIO16 控制 BOOT，GPIO15 控制 NRST；GPIO47 为 U4/U5 共用 OE#。初始化及文件接收期间 OE# 保持高，目标接口关闭。实际烧写时先设置 UART 空闲高、BOOT 高和 NRST 低，再使能转换器、释放复位、等待 250ms 并握手。共用方向 GPIO21 在此期间置低，让 SWDIO 保持接收方向；SWD/SWO 不发送数据。

F103 从系统存储器启动的条件是复位采样 BOOT0=1、BOOT1=0；烧录后 BOOT0=0 并复位回到用户 Flash。G431 的启动还受 BOOT_LOCK、nBOOT1 和 BOOT0 选择选项字节影响。这些选项字节和读写保护都不会被程序自动修改或解除。

本板 NRST 经电平转换器推挽驱动，编程时不要让外部下载器同时驱动该线；后续硬件可将 NRST 改为独立开漏控制。

## 电脑工具

使用现有 PlatformIO Python（含 pyserial）：

```powershell
& "$env:USERPROFILE/.platformio/penv/Scripts/python.exe" tools/flash_stm32.py "C:/firmware/f103_app.bin" --part STM32F103C8T6 --port COM14
```

以上默认只传输和校验文件。确认目标接线及固件后，实际烧写使用：

```powershell
& "$env:USERPROFILE/.platformio/penv/Scripts/python.exe" tools/flash_stm32.py "C:/firmware/f103_app.bin" --part STM32F103C8T6 --port COM14 --execute
```

工具先让主机 STOP 并等待从机确认。仅使用 `--execute` 时，成功烧写后 **保留在 ROM，不自动运行应用**。需要写入并立即启动时，显式加上 `--run`：

```powershell
& "$env:USERPROFILE/.platformio/penv/Scripts/python.exe" tools/flash_stm32.py "C:/firmware/f103_app.bin" --part STM32F103C8T6 --port COM14 --execute --run
```

此时从机在完整读回一致后把 BOOT0 拉低，再发送 ROM GO 0x21 跳转到 0x08000000。只有收到 GO 确认才报告 `go_ack:true`；没有完成验证不能执行 GO，重复烧写请求不会重放 GO。GO 确认表示 ROM 接受跳转，应用的实际运行现象还应在目标板检查。

默认料号为 `STM32F103C8T6`；不写 `--part` 也会选择它。`flash_g431.py` 保留为旧入口，省略料号时仍选 G431。

默认写 `stm32_transfer.json` 记录文件 SHA256、料号、传输 ID、接收进度、重试数及烧写结果。`--journal 路径` 可另存，`--resume 旧记录.json` 复用同一传输 ID 并核对文件完全相同。暂存只在从机内部 RAM 中：短暂掉线可查询/继续；从机断电或重启后需重新传完整文件。若烧写期间电脑断开，从机继续用已经校验的本地镜像完成操作，恢复连接后用同一 ID 查询结果；状态查询不会再次擦除。

## USB 指令

英文 `!` 开头、换行结束。ID 为非零 32 位十六进制，长度与偏移为十进制：

```text
!fw begin ID SIZE FLASH_KB SHA256_HEX f103c8
!fw data ID OFFSET HEX_DATA
!fw seal ID
!fw status ID
!fw flash ID ERASE
!fw flash ID ERASE RUN
!fw abort ID
```

`begin` 末项为 `f103c8` 或 `g431`。旧的六段 `begin` 指令仍按 G431 解释，F103 必须显式带上目标。

单块最多 128 字节。回复为 `type:"fw"` JSON，包含目标 `target` / `profile`、传输 ID、请求序号、操作、请求偏移、接收总量、状态、阶段、进度、设备标识、实际容量 `actual_flash_kb`、错误、重传数、`target_enabled` 和 `go_ack`。阶段 1 探测、2 擦除、3 写入、4 读回验证、5 已确认 GO。`programming` 的 `ok:true` 只表示任务已接受；必须查询到 `done` 且 `job_error:"ok"` 才表示写入及完整读回成功。`ERASE RUN` 还需要 `go_ack:true`。

`abort` 只清理尚未编程或已结束的暂存任务；烧写中返回 busy，不在 UART 指令中途强行中断。文件会话期间 RUN 被拒绝。现有 PID、OLED、RGB、LINK LED、SAVE 和 USB 状态接口保留；目标烧写状态通过文件协议查询。

## 丢包与完整性

无线无法承诺零丢包。本版对每块采用以下措施：

1. 固定对端 MAC、加密单播、两端启动会话、传输 ID、请求序号和偏移，拒绝旧会话/不匹配确认。
2. 每个文件协议包含 CRC32。损坏或无效包不会更新接收缓冲。
3. 每次只发送一块，收到从机应用层确认再发送下一块；150ms 超时重发，每轮最多 20 次，电脑工具再做有界重试。无线 MAC 发送成功不算文件已收到。
4. 重复数据块核对已接收内容；重复请求返回缓存确认。相同 transfer ID 与清单继续现有会话；不同清单拒绝。重复烧写指令只查询既有任务，不再次擦除。
5. 完整文件收到后验证 SHA256 和所选芯片的向量表；文件缺块、损坏或超出容量均不允许烧写。
6. 从机本地执行 ROM Get/Get ID/Read Memory/Erase/Write Memory。F103 优先使用 Get 返回的普通擦除 0x43；若只提供扩展擦除 0x44 则选择它。设备 ID、支持指令及实际容量通过后，按 1 KiB 页擦除 F103 镜像覆盖范围，末块按 4 字节补 0xFF；G431 仍为扩展擦除、2 KiB 页、8 字节补齐。两种目标都逐字节读回整个镜像核对。

UART 写后 ACK 超时属于“可能已经写入”的不确定结果，直接报告失败，不盲目重写同一 Flash 双字。恢复时创建新文件任务，从擦除开始。长时间断网会明确失败，不能把超时当成成功。目标单 Bank Flash 的擦写仍受掉电影响，本方案不是断电原子升级；失电后可以重新进入 ROM 恢复烧录。

## 验证与限制

`tests/native_test.cpp` 覆盖 CRC 损坏、请求/ACK 模拟丢失、重复/冲突数据、续传、整文件校验门槛、向量/容量检查，以及独立 ROM 模拟器的擦除、对齐补齐、读回、错误设备/容量/指令和写后 ACK 超时。

主机 `host_flash`、从机 `slave_flash` 及关闭目标接口的传输环境编译通过。两块 ESP32 的实板测试使用主动丢包环境：128 KiB 文件全部接收，实际 SHA256 通过，共发生 576 次重传；11 项传输与拒绝错误检查通过，包括重复清单/数据、乱序偏移、冲突数据、完整文件损坏及未验证镜像禁止编程。目标 OE# 全程保持关闭；`validation/test_only_not_executable.bin` 只是验证传输的测试字节，不能用于目标板运行。记录见 `validation/native_20261017.txt`、`loss_transfer_20261017.json`、`real_transport_20261017.json`。测试也发现并修复了主机原生 USB 默认 256 字节接收队列不足的问题；文件环境现在使用 1024 字节队列。

2026-10-18 的 F103 适配通过主机、从机和关闭目标接口环境编译，并通过严格警告的 C++ 测试：F103 64 KiB/20 KiB 范围、目标清单不可替换、普通/扩展擦除、1 KiB 页、4 字节补齐、全容量镜像、完整读回、错误芯片/容量及 ACK 超时，以及既有 G431 回归。记录为 `validation/native_f103_20261018.txt`。17 日的 128 KiB 无线实测属于此前 G431 协议版本的历史结果；不能当作新 F103 协议的实板验收。

2026-10-18 已将 `host_flash` 和 `slave_flash` 安装到主机 COM14、从机 COM18，完成首次 F103 实板烧录：436 字节的 PA3 常高程序经 USB → 主机 → ESP-NOW → 从机 → USART1 写入目标，完整读回一致，ROM GO 确认成功，文件传输重试 0 次。目标 ID 0x410，实际容量回报 128 KiB，镜像范围仍按 C8 的 64 KiB 限制；本次仅擦除第 0 页。此前探测因容量回报与 64 KiB 不相等而停止，未执行擦除；现已修复这一兼容性问题，并增加实际容量回报字段。新增测试确认容量回报 128 KiB 时仍不允许擦除或写入第 64 KiB 以后的区域，0/32/256/65535 KiB 等异常回报在擦除前拒绝。

结果见 `validation/pa3_program_final_20261018.json`、`pa3_program_final_20261018.txt`、`native_pa3_20261018.txt`；测试程序在 `../../STM32F103_PA3_HIGH`。主机恢复到测试前的 P/I/D/STEP RAM 参数，保留 STOP，未改写保存参数。2026-10-18 用户实测 PA3 对目标 GND 约 3.3V，确认应用输出符合预期；验收记录见 `validation/acceptance_pa3_20261018.json`。G431 仍只有编译、模拟和历史传输测试，尚无真实 G431 擦写验收。

## 官方依据

- [ST AN2606：G431 系统 ROM 接口和启动条件](https://www.st.com/resource/en/application_note/an2606-stm32-microcontroller-system-memory-boot-mode-stmicroelectronics.pdf)
- [ST AN3155：USART Bootloader 命令和校验](https://www.st.com/resource/en/application_note/an3155-usart-protocol-used-in-the-stm32-bootloader-stmicroelectronics.pdf)
- [ST F103x8 数据手册：BOOT0、20 KiB RAM、引脚](https://www.st.com/resource/en/datasheet/stm32f103c8.pdf)
- [ST 官方 F103 CMSIS：Flash 容量寄存器](https://github.com/STMicroelectronics/cmsis-device-f1/blob/master/Include/stm32f103xb.h)
- [ST G431 数据手册：存储器及封装](https://www.st.com/resource/en/datasheet/stm32g431cb.pdf)
- [ST 官方 CMSIS：Flash 容量寄存器地址](https://github.com/STMicroelectronics/cmsis-device-g4/blob/master/Include/stm32g431xx.h)
