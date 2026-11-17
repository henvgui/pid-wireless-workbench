# STM32 无线烧录型号与 ESP32 从机维护索引

本文件汇总现有的 STM32 烧录入口、型号支持和从机硬件差异。各入口共用下面的烧录链路。

**上位机 GUI / CLI / HTTP / Keil → ESP32-S3 主机 USB → ESP-NOW → 当前从机 → UART ROM 或 SWD → STM32**

## STM32 型号支持

| STM32 完整型号 | 标称 Flash | 从机算法 | 上位机型号目录 | 实板状态 |
| --- | ---: | --- | --- | --- |
| STM32F103C8T6 | 64 KiB | UART ROM，`f103c8` / ID 2 | 已列出 | UART 已写入、完整读回及启动 |
| STM32G431C6T6 | 32 KiB | UART ROM，`g431` / ID 1 | 已列出 | 待真实目标擦写验收 |
| STM32G431C8T6 | 64 KiB | UART ROM，`g431` / ID 1 | 已列出 | 待真实目标擦写验收 |
| STM32G431CBT6、STM32G431KBU6 | 128 KiB | UART ROM，`g431` / ID 1 | 已列出 | 待真实目标擦写验收 |
| STM32G431RBT6 | 128 KiB | 底层 Python 可识别型号，复用 `g431` UART ROM 参数 | 待加入 | 同板直连待适配及验收 |
| STM32F407VET6 | 512 KiB | 默认 SWD，也可选 UART ROM，`f407ve` / ID 4 | 已列出 | SWD 已写入、完整读回、启动及 H2 恢复。UART 待验收 |
| STM32F407VGT6 | 1024 KiB | 默认 SWD，也可选 UART ROM，`f407vg` / ID 3 | 已列出 | 真实 1 MiB 目标待验收 |

表中的 ID 是现有无线协议的目标编号。STM32 的器件 ID 和容量由从机在擦除前核对。镜像应按实际 STM32 型号编译。各 G431 型号的无线 SWD 均待适配，现有 Keil 一键配置脚本仅支持两款 F407 的 SWD。[历史烧录验收](../../PID_Shared/PIDFlash/README.md) · [F407 串口恢复验收](UART_RESUME_FIX_20261103.md)

## 现有烧录代码入口

| 层 | 现有代码 | 职责 |
| --- | --- | --- |
| 上位机 | [workbench_core.py](../backend/workbench_core.py) 的 `CHIPS`、`flash_method()`、`prepare()` | 提供型号与接口目录，GUI 从 `/v1/capabilities` 读取，CLI、HTTP 和 Keil 走同一后台预检 |
| 电脑侧传输 | [flash_stm32.py](../../PID_Shared/PIDFlash/tools/flash_stm32.py) 的 `target_profile()`、`validate_image()` | 处理料号、镜像容量和向量表，暂存镜像到从机。构建工作台时，[build.ps1](../build.ps1) 将工具同步到 `backend/protocol` |
| Keil | [configure_keil.py](../integrations/keil/configure_keil.py)、[pid_keil.py](../integrations/keil/pid_keil.py) | 给当前 Target 配置外部下载器，将当前编译产物转为 BIN 并提交后台 |
| ESP32 主机 | [host_local_app.cpp](../../ESP32_S3_RGB_Chase/src/host_local_app.cpp)、[peer_catalog.h](../../PID_Shared/PIDLink/src/peer_catalog.h)、[PIDFlash.cpp](../../PID_Shared/PIDFlash/src/PIDFlash.cpp) | 保存并选择从机，处理 USB 烧录指令及无线传输，锁定进行中的烧录目标 |
| ESP32 从机 | [PIDFlash.cpp](../../PID_Shared/PIDFlash/src/PIDFlash.cpp) 的 `programWorker()`、[flash_core.cpp](../../PID_Shared/PIDFlash/src/flash_core.cpp) 的 `profile()` | 校验暂存镜像，按目标和接口调用算法，返回设备、容量、进度及结果 |
| UART ROM | [stm32_rom.cpp](../../PID_Shared/PIDFlash/src/stm32_rom.cpp) | 共用握手、擦除、写入、读回和 GO 流程，按芯片配置使用相应页大小及写入对齐 |
| SWD | [swd_programmer.cpp](../../PID_Shared/PIDFlash/src/swd_programmer.cpp)、[F407 RAM 烧录器](../../PID_Shared/PIDFlash/ram_loader/f407_loader.c) | `ArmSwd` 提供调试访问，`F407Swd` 负责 F407 Flash 扇区、容量和擦写流程 |

## 多从机如何分版本

用三份信息描述一次烧录。**STM32 型号**决定使用哪套算法和镜像检查。**从机板型**决定 GPIO、驱动和可用接口。**从机名称与 MAC**决定这次任务送到哪一台实体设备。

| 从机板型 | 现状 | 维护方式 |
| --- | --- | --- |
| 外接版，带 U4/U5 | 当前 GREEN 使用的板型，正式环境为 `slave_flash_psram`，支持外接目标 | 同电路、同 ESP32 Flash/PSRAM 规格的多台从机可共用固件。每台设置自己的身份色，按各自实际 MAC 登记到主机 |
| ESP32-S3 与 STM32G431RBT6 同板直连版 | 设计中。拟使用直连 UART、BOOT0、开漏 NRST 和 SWD | 在同一从机工程中为直连版建立独立的 PlatformIO 环境，复用无线、烧录任务和芯片算法，单独实现接口电气行为 |

目前从机板级引脚和操作分布在 [board_pins.h](../../ESP32_S3_Slave_Diagnostic/include/board_pins.h)、[slave_target_pins.h](../../PID_Shared/PIDFlash/src/slave_target_pins.h)、[PIDFlash.cpp](../../PID_Shared/PIDFlash/src/PIDFlash.cpp)、[serial_bridge.cpp](../../PID_Shared/PIDFlash/src/serial_bridge.cpp) 和 [motor_bridge.cpp](../../PID_Shared/PIDFlash/src/motor_bridge.cpp)。直连版应统一处理 BOOT0、NRST、SWDIO 收发切换和接口释放，让烧录、H2 串口和电机转发使用同一套板级操作。现有 [PlatformIO 配置](../../ESP32_S3_Slave_Diagnostic/platformio.ini) 中的 USB 序列号用于部署从机 ESP32 固件时定位实体设备。多台同板型从机可使用同一个编译产物，部署时仍要按实际 USB 身份选板。

当前主机名单保存颜色和 MAC。点击 Keil Download 或在工作台烧录时，传输工具读取当前从机的 MAC，主机在 Begin 时再次核对并锁定。后续混用不同板型时，可让从机报告板型、固件版本和可用接口，供工作台与 Keil 在烧录前核对。对于 STM32 和 ESP32 固定在一起的板卡，还可以登记固定 STM32 料号，发现与任务不匹配时在擦除前拒绝。[从机选择与验收](PEER_SELECTION.md)

## 新增型号或板型的顺序

1. **先判断能否复用算法。** 同一芯片族的新料号，先核对设备 ID、容量、向量范围、擦除单位和写入对齐。若已有匹配的从机配置及接口，优先补电脑端型号目录和预检。新增 UART 芯片族时，根据 ST 文档核对 ROM 入口、实际 UART 引脚、擦写和启动命令。新增 SWD 芯片族时，复用调试访问并按该族实现 Flash 擦写。[ST AN2606](https://www.st.com/resource/en/application_note/an2606-stm32-microcontroller-system-memory-boot-mode-stmicroelectronics.pdf) · [ST AN3155](https://www.st.com/resource/en/application_note/an3155-usart-protocol-used-in-the-stm32-bootloader-stmicroelectronics.pdf)
2. **按硬件选从机固件。** 原外接版接线兼容时可继续使用原板型。新电路单独加从机编译环境。主机继续共用 `host_flash`，只有引入新协议目标、能力字段或选择逻辑时，才检查是否需要同步更新主机。
3. **先验证预检，再做实板验收。** 先覆盖错误型号、容量、接口和从机选择。实板按对应路径验收写入、完整读回、启动及 H2 恢复。两台真实从机同时在线时，再验证选中目标、忙时锁定和另一台从机的行为。
4. **按阶段接入 G431RBT6。** 先加入工作台 UART 型号目录并验证镜像预检。直连板完成后适配板级驱动并验收 UART ROM。需要从 Keil 直接下载时，补该型号的 Keil 配置入口。无线 SWD 作为单独的算法和实板验收项。

当前型号规则分别在电脑端目录、Python 预检和 ESP32 固件中。后续改代码时，可先让电脑端目录和预检共用一份型号定义，并用回归检查核对 ESP32 的目标编号、容量和接口。原有协议编号保留，板级配置和芯片算法各自维护。日常操作及部署顺序见 [操作与升级手册](AI_USER_MANUAL.md)。
