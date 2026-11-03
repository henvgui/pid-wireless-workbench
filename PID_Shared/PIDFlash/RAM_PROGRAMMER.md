# F407 SRAM 块烧录器

20261103.3：RUN 阶段增加系统复位确认、Flash 入口/SP/中断上下文检查，修复下载显示成功但目标应用串口未恢复的问题。[修复与实板验收](../../PID_Workbench/docs/UART_RESUME_FIX_20261103.md)。

2026-11-03，适配 F407VE（512 KiB）和 F407VG（1024 KiB），从机 H3 SWD 接线不变。VET6 是当前实板验收对象；VGT6 算法及容量检查已适配，尚未实板验收。

电脑仍走「USB → 主机 → ESP-NOW → 从机」。本次改变的是最后一段 SWD 写入，USB/无线批量协议保持兼容。不需要在 STM32 Flash 安装额外 Bootloader。

## 执行顺序

1. 从机完整接收镜像，检查整份 SHA256 和向量表。
2. SWD 复位并暂停目标，核对设备 ID、实际容量及 RDP；保护设置不自动修改。
3. MEM-AP 连续写入 SRAM 烧录器，跨 1 KiB TAR 边界重新设地址，完整读回代码后执行不写 Flash 的 Probe。
4. 解锁 Flash，仅擦除镜像覆盖的扇区。保持原来的 2 µs SWD 半周期。
5. 每批最多 1024 B 写入目标 SRAM；目标先算 CRC32，再在 SRAM 执行逐字 Flash 编程、检查忙状态及错误。末批以 FF 补齐 4 B。
6. 每次调用检查预期 BKPT、完成标记、调用 nonce、处理字节数和 Flash 状态；任何失败立即停止。
7. 锁定 Flash，逐字节完整读回比较；只有成功且请求 RUN 才复位启动。

SRAM 运行期间屏蔽中断，暂停状态冻结看门狗；算法执行时喂 IWDG，不启动看门狗。目标 PSIZE=x32，供电应在 2.7–3.6 V。算法不擦除 Flash、不写选项字节；擦除仍由原有流程控制。

| SRAM 区域 | 用途 |
|---|---|
| 0x20000000–0x200007FF | 烧录器代码，当前 476 B（含填充） |
| 0x20001000–0x200013FF | 1 KiB 数据缓冲 |
| 0x20001800–0x20001833 | 调用与结果邮箱 |
| 0x20001C00–0x20001FFF | 栈保留区，栈顶 0x20002000 |
| 0x200023FC–0x200029FB | 仅 `!ramcheck` 使用的 SRAM 测试区 |

这些区间在目标复位暂停后临时使用，原应用重新启动时由自己的启动代码初始化 RAM。

## 选择与回退

更新主机 `host_flash` 和从机 `slave_flash_psram` 后，F407 SWD 默认使用 SRAM 算法，GUI / Keil 无需改变调用方式。旧从机继续原来的逐字编程。结果 JSON 的 `algorithm` / `status.swd_algorithm` 为 `ram_1k` 或 `legacy_word`，`target_program_seconds` 记录目标操作时间。

工作台 CLI 手动选择旧算法：

```powershell
./pidctl.cmd flash --chip STM32F407VET6 --interface swd --firmware 'C:/firmware/app.bin' --swd-legacy --idempotency-key build-legacy-001 --wait
```

HTTP `POST /v1/jobs` 的 flash 请求增加 `"swd_legacy": true`；默认 false。仅用于 SWD，且需要新版主机/从机。默认计划不增加此字段，保持旧任务的幂等键兼容。

底层工具 `tools/flash_stm32.py` 的 `--swd-legacy` 选择旧 SWD 写入；原 `--legacy` 只选择旧的无线逐包传输，两者作用不同。底层命令等价于 `!fw flash ID ERASE RUN LEGACY`；省略 RUN 则校验后保持暂停。

**执行回应不确定或超时后，不自动重擦除、不自动切换算法。** 先查询原任务；只有明确决定重新烧录时，使用新幂等键。旧算法回退也是一次新的真实擦写。

## 不擦 Flash 的检查

仅在无烧录/串口任务且 RUN 请求已关闭时，直连从机 USB 发送 `!ramcheck`。它仅支持本次 F407VE 配置，执行 SRAM 边界图样读写、代码读回、Probe 和 CRC Check，返回 `{"type":"ramcheck","error":"ok","flash_written":false,"target_halted":true}`。它会复位并暂停目标、改动 SRAM，不用于运行中应用的无扰动监测。通过后可正常烧录并 RUN，或手动 RESET 恢复原应用。

构建源码是 `ram_loader/f407_loader.c`、链接脚本及协议头。安装 Arm GNU Toolchain 后执行：

```powershell
python PID_Shared/PIDFlash/ram_loader/build.py
python PID_Shared/PIDFlash/ram_loader/build.py --check
python scripts/run_native_tests.py
```

生成的 `src/f407_loader_image.h` 随工程提交，正常编译 ESP32 无需安装 Arm 编译器。原生测试执行相同 C 算法，覆盖 CRC、范围、错误、超时、调用 nonce、部分末批、容量边界、旧算法及失败不重放。

依据：[ST RM0090 Flash 编程章节](https://www.st.com/resource/en/reference_manual/DM00031020.pdf)、[Arm CMSIS 核心调试寄存器](https://arm-software.github.io/CMSIS_5/latest/Core/html/structCoreDebug__Type.html)、[CMSIS-Pack Flash Algorithm](https://open-cmsis-pack.github.io/Open-CMSIS-Pack-Spec/main/html/flashAlgorithm.html)。本实现采用自己的固定 SRAM 邮箱 ABI，并非 Keil FLM 文件加载器。
