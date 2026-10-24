# STM32F407VET6 / VGT6 无线 SWD 接线

更新：2026-10-19。路径为：电脑 → 主机 USB → ESP-NOW → 从机 H3 → F407 SWD。**不需要 BOOT0。**

2026-10-23 更换目标后的复验：新 F407VET6 的 ID=0x413、Flash=512 KiB、RDP=0xAA，使用当前主从固件与原工程 8880 字节镜像完成无线写入、完整读回、启动确认，任务 `ccceb936d25638e73bdc59c1`，重试 0 次，用时约 12.44 秒。此次没有解除读保护。旧目标的 Flash/UID/容量读取异常未在新目标上复现；旧目标的具体故障原因尚未独立确认。

当前用户 `f407.uvprojx` 的 Download 按钮仍为 ST-Link 有线配置；上述无线复验通过 CLI/Keil 适配器直接调用，未切换工程配置。记录见 `../validation/verify_replacement_f407_20261023/`。

## 接线表

按从机 2026-10-16 版原理图的 H3 脚号接线；以排针丝印的 1 脚方向为准。

| 从机 H3 | 标注 | 从机 GPIO | F407 目标板 |
|---|---|---|---|
| 1 | VTREF | 电平转换器目标侧参考 | 目标 3.3V |
| 2 | DIO / SWDIO | GPIO13，经双向电平转换 | PA13 / SWDIO |
| 3 | CLK / SWCLK | GPIO12 | PA14 / SWCLK |
| 4 | GND | GND | 目标 GND |
| 5 | SWO | GPIO14 | 本版不需要接；预留可对应 PB3/SWO |
| 6 | NRST | GPIO15 | 目标 NRST / RESET |

使用 1、2、3、4、6 共五根线。目标板自行正常供电，VTREF 接目标 3.3V，不接 5V。BOOT0 和 BOOT1 不需要通过从机连接或切换；保留目标板正常启动配置。SWD 时从机 BOOT 控制脚保持高阻。

H1 是 ESP32 自身调试串口，H2 是目标 UART 接口，本次使用 H3。SWCLK 和 SWDIO 按名称一一对应，不像 UART 那样交叉。烧录时避免另一个下载器同时驱动这些信号。

## 需要安装的固件

- 主机：`ESP32_S3_RGB_Chase` 的新版 `host_flash`。
- 从机：新版 `slave_flash` 可接收不超过 128 KiB 的镜像；F407 较大镜像使用 `slave_flash_psram`，该环境仅适用于当前 N16R8 的 Octal PSRAM 硬件。
- 上位机：后台版本 `2026.10.19.2` 或以后；选择 `STM32F407VET6`（512KB）或 `STM32F407VGT6`（1MB）时默认选中从机 SWD。

仅更新上位机不够，两块 ESP32 均需安装新版固件。PSRAM 初始化或暂存分配失败会报错，不会开始擦写目标。较大的旧会话不能跨重启保留。

## 上位机与 AI

在“固件烧录”选择目标芯片的实际型号。本次实板为 `STM32F407VET6`，接口选 `SWD · 从机 → STM32F407`；USB 端口仍选择主机。选择针对 F407 编译、从 `0x08000000` 开始的 BIN，检查镜像后提交烧录。

```powershell
./pidctl.cmd plan --chip STM32F407VET6 --interface swd `
  --firmware 'C:/firmware/f407.bin'
./pidctl.cmd flash --chip STM32F407VET6 --interface swd `
  --firmware 'C:/firmware/f407.bin' `
  --idempotency-key f407-build-001 --wait
```

F407VE 按 512 KiB 校验，VG 按 1024 KiB 校验；型号与实读容量不符会在擦写前停止。初始栈要求 `0x20000000 < SP <= 0x20020000`，不接受 CCM 初始栈。VE 有 8 个 Flash 扇区，VG 有 12 个：前 4 个各 16 KiB、第 5 个 64 KiB，后续各 128 KiB。只擦除镜像覆盖的完整扇区，同一扇区内镜像之外的旧数据也会随整扇区擦除。

成功要求 `state:succeeded`、`result.verified:true`。如果勾选启动，还要求 `result.start_ack:true`。SWD 通过复位启动，`go_ack` 不作为 SWD 成功判据；它是 UART ROM GO 的确认字段。不请求启动时，目标在验证后保持调试暂停，按目标 RESET 可运行。

## 验证边界

2026-10-19，F407VET6 实板无线 SWD 烧录、完整读回、复位启动及 131091 字节 UART 回传通过。芯片 ID=0x413，实读 Flash=512KB。VGT6 的 1MiB 算法仅完成模拟验证，UART ROM 路径未在 F407 实板验收。较大镜像的 PSRAM 分配已实测，尚未给真实目标烧录满容量镜像。测试项目为 `STM32F407_UART_ECHO`，不能用 F103 的同名 BIN 给 F407 烧录。

本次修正了从机错误 GPIO 映射和 SWD 采样边沿：CLK/DIO/SWO 实际为 GPIO12/13/14，不是旧文档的 11/12/13。H3 的外部脚序保持不变。

官方依据：[ST F407 数据手册](https://www.st.com/resource/en/datasheet/stm32f407vg.pdf)、[RM0090](https://www.st.com/resource/en/reference_manual/rm0090-stm32f4xx-reference-manual-stmicroelectronics.pdf)、[ST 官方 F407 寄存器定义](https://github.com/STMicroelectronics/cmsis-device-f4/blob/master/Include/stm32f407xx.h)。
