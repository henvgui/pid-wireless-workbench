# Keil 直接无线下载使用说明

更新：2026-10-19。当前工程：`C:/Users/twice/Desktop/STM32PRO/f407/MDK-ARM/f407.uvprojx`，Target 为 `f407`，芯片为 **STM32F407VET6，512 KiB**。

## 日常使用

1. 本次配置修改前已打开的 Keil，需要关闭并重新打开上述工程一次，使磁盘上的下载设置生效。若 Keil 提示工程文件被外部修改，重新加载磁盘版本。
2. 保持 ESP32 主机通过 USB 接电脑，从机和目标 STM32 正常供电，原来的 SWD 五根线保持连接。
3. 在 Keil 编写程序后，先 **Build（F7）**，确认没有编译错误。
4. 点击工具栏 **Download**，或菜单 **Flash → Download**。无需打开 PID 调试工作台界面。2026-10-22 起立即显示实时进度窗：当前阶段、真实百分比、活动动画和已用时间。
5. 下载会等待无线任务结束。成功后进度窗约 1.4 秒自动收起，Keil 保留成功输出：

   ```text
   [PID] Programming Done. Verify OK. Application running.
   ```

烧录路径：**Keil → 后台适配程序 → 主机 USB → ESP-NOW → 从机 SWD → STM32**。USB 自动识别主机序列号 `44:1B:F6:8D:10:20`，无需固定为 COM14。后台服务由适配程序自动启动。

本功能负责下载、完整读回校验、复位启动；Keil 的断点、单步、变量查看等在线调试尚未接入。它没有把 ESP32 主机伪装成 ST-Link 或 CMSIS-DAP。

## 已配置的 Utilities

`Project → Options for Target → Utilities`：

| 设置 | 内容 |
|---|---|
| Use External Tool for Flash Programming | 选中 |
| Command | `C:\Users\twice\Documents\Codex\2026-10-01\pid\outputs\PID_Workbench\integrations\keil\PidKeilDownload.exe` |
| Run Independent | **不勾选**，让 Keil 等待并接收结果 |

Arguments：

```text
"C:\Users\twice\.platformio\penv\Scripts\python.exe" "C:\Users\twice\Documents\Codex\2026-10-01\pid\outputs\PID_Workbench\integrations\keil\pid_keil.py" --firmware "#L" --project "#P" --chip STM32F407VET6 --interface swd --fromelf "C:\Keil_v5\ARM\ARMCC\bin\fromelf.exe"
```

`#L` 由 Keil 替换为本 Target 的链接产物，`#P` 替换为工程文件。路径应保留双引号。现有 `.uvoptx` 保留；Keil 5.39 的命令行验证也需要工程对应的 `.uvoptx`，否则可能在调用外部下载器前报 Internal DLL Error。

下载入口使用 Windows 启动器：后台启动 Python，持续转发 stdout/stderr，并返回原退出码。它不打开黑色 Windows Terminal 窗口、不等待键盘输入。完成后 Keil 输出区另显示 `Download tool finished (exit 0).`。失败则为非零退出码。第一次从 python.exe 入口切换到本启动器时需重新加载工程；后续原路径上的程序更新会在下一次 Download 自动使用。

### 实时进度窗（2026-10-22）

本机 Keil 会在外部工具结束后集中刷新输出区，所以仅靠日志看起来会停顿十几秒。启动器现在使用独立的实时窗口显示状态：

- 准备连接时显示活动动画；收到真实任务进度后显示检查、传输、擦除、写入、校验、启动阶段和百分比。
- 进度暂时不变时仍显示活动动画和累计耗时；不会按时间编造百分比，也不会因为读到 `done 100%` 就提前显示成功。
- 工具正常退出并确认结果后才显示完成；失败使用红色状态和错误内容，保留原失败退出码。
- 成功约 1.4 秒后自动收起；失败 8 秒后自动收起，也可直接关闭或打开日志。处理中点“收起”或关闭窗口只隐藏显示，烧录继续进行。
- 适配 PerMonitorV2 高 DPI；Keil 输出区、`events.log` 和任务 JSON 继续保留完整结果。只需再次点击 Download，不用重新配置工程。

命令行自动化或测试需要不显示窗口时，可在该进程环境设置 `PID_KEIL_NO_PROGRESS=1`，日志、任务校验和退出码保持一致。

### SWD 提速（2026-10-22）

当前从机 `slave_flash_psram` 已更新，SWD 时钟半周期由 5 µs 改为 2 µs；受 GPIO 和软件开销影响，实际频率低于理论 250 kHz。完整读回逐字节比较、SHA256、ACK/奇偶校验、设备和容量检查均保留。Keil 无需更改设置，下一次 Download 自动使用新速度。

同一份 8,708 字节 F407VET6 固件实测：从记录创建到最终确认由 **20.08 秒降至 12.60 秒**，耗时缩短约 **37%**；新任务 `6fc4bd9178dc915eabd15be0` 为 `succeeded`、`verified=true`、`start_ack=true`，传输重试为 0。SHA256 为 `318133abe09914026f41f6c9773390a7e341678a62a2d822953fa7e68f792cbd`。这是本次小镜像和当前接线下的结果，大镜像耗时不同。

编译参数位于从机 `platformio.ini` 的 `slave_flash`，由 `slave_flash_psram` 继承：`PID_FLASH_SWD_HALF_PERIOD_US=2`。若将来更换长线导致 SWD 错误，可改回 `5` 并重新编译、更新从机；不自动重放失败的擦写。验收记录在 `PID_Workbench/validation/speed_benchmark_20261022/`。

配置只修改所选 Target 的 Utilities，不改变芯片、链接地址、源代码或编译选项；原 `.uvprojx` 已备份为 `f407.uvprojx.before_pid_日期时间.bak`。恢复原下载设置可以从该备份取回 Utilities 节。

## 固件转换及成功判据

- 支持适配器输入 `.axf` / `.elf` / `.hex` / `.bin`；工作台 GUI/API 的直接固件输入仍为 `.bin`。
- AXF/ELF 通过本机 `fromelf --i32combined` 导出带地址 HEX，再验证记录校验和、地址和重叠内容。
- 固件必须从 `0x08000000` 开始，符合所选芯片容量，且初始 SP、Thumb 复位向量有效。不会把偏移固件或 RAM 镜像自动搬到 Flash 起始地址。
- HEX 空洞填 `0xFF`；超出 Flash、冲突重叠、损坏或缺少 EOF 均拒绝。
- 每次下载固定原始产物和 BIN 快照，记录 SHA256。提交请求前持久保存任务键。
- 只有任务 `state=succeeded`、`result.verified=true`，且 SWD 的 `result.start_ack=true` 时才报告下载并启动成功。下载失败、校验失败、超时等返回非零退出码。
- 每次 Download 是一个新的烧录意图；等待超时后的续查应使用原记录，避免再次擦写。

目前 F407VET6/VGT6 支持从机 SWD；实际接着的这颗是 **VET6**。不要因为工程名叫 f407 就改选 VGT6。UART 对应芯片目录中的已适配型号；更换为 UART 时需要相应 BOOT/NRST 接线。

## 日志与异常恢复

当前工程的记录目录：

```text
C:\Users\twice\Desktop\STM32PRO\f407\flash_records\keil\日期时间_随机编号\
```

| 文件 | 用途 |
|---|---|
| `attempt.json` | 本次意图、原始工程、固定固件、SHA256、任务键、任务 ID、状态 |
| `firmware.bin` | 本次实际提交的固件快照 |
| `source.axf` / `source.hex` 等 | 固定的输入与转换结果 |
| `fromelf.log` | AXF/ELF 转换日志 |
| `events.log` | 适配程序进度与完成信息 |
| `job.json` | 后台最近一次完整任务结果 |

如果 Keil 被关闭、等待超时或提交响应丢失，用原 `attempt.json` 继续查询；已有任务 ID 时只查询该任务，没有收到 ID 时以原请求和原任务键重新提交，服务端去重：

```powershell
& 'C:/Users/twice/.platformio/penv/Scripts/python.exe' `
  'C:/Users/twice/Documents/Codex/2026-10-01/pid/outputs/PID_Workbench/integrations/keil/pid_keil.py' `
  --resume 'C:/Users/twice/Desktop/STM32PRO/f407/flash_records/keil/实际目录/attempt.json'
```

任务若已失败，会返回该失败结果；不会自动生成新键重新擦写。应先确认原因，再决定是否重新 Download。无线未连接、硬件有其他任务、串口被占用时会明确报错。不要同时在 GUI 和 Keil 发起烧录。

### Keil 提示找不到 api_token.txt

2026-10-19 已修复 Codex/Keil 启动环境差异：Codex 属于 MSIX 应用，Windows 可能将服务看到的 `%LOCALAPPDATA%/PIDWorkbench` 重定向到 `%LOCALAPPDATA%/Packages/OpenAI.Codex_*/LocalCache/Local/PIDWorkbench`，而普通 Keil 进程仍查看未重定向的目录。

现在客户端先检查自己的数据目录，再查找同一 Windows 用户的 Codex 服务凭据；服务存在时复用该服务及原凭据，不生成新令牌、不迁移任务数据库、不额外启动服务。不要把 token 内容粘贴给 AI 或写进 Keil 参数。显式设置 `PID_WORKBENCH_DATA` 时遵循该目录，不自动切换到其他目录。

只有服务明确返回鉴权失败 `401` 时，才尝试另一个现有凭据；超时、断连、设备忙或其他错误不会自动重发烧录请求。仍应通过原任务键/记录恢复不确定的任务。此修复只涉及后台 Python 客户端，Keil 下一次 Download 即使用新代码。

仅转换及本地预检，不提交任务、不操作目标：

```powershell
& 'C:/Users/twice/.platformio/penv/Scripts/python.exe' `
  'C:/Users/twice/Documents/Codex/2026-10-01/pid/outputs/PID_Workbench/integrations/keil/pid_keil.py' `
  --firmware 'C:/Users/twice/Desktop/STM32PRO/f407/MDK-ARM/f407/f407.axf' `
  --project 'C:/Users/twice/Desktop/STM32PRO/f407/MDK-ARM/f407.uvprojx' --check
```

如果工程在其他目录，使用 `--records-dir` 指定记录保存位置。ARM Compiler 6 工程应通过 `--fromelf` 指定对应编译器的转换工具。

## 给 AI 的安装入口

CubeMX 重新生成工程或迁移工程后，如果 Utilities 设置被覆盖，可再次运行：

```powershell
& 'C:/Users/twice/.platformio/penv/Scripts/python.exe' `
  'C:/Users/twice/Documents/Codex/2026-10-01/pid/outputs/PID_Workbench/integrations/keil/configure_keil.py' `
  --project 'C:/Users/twice/Desktop/STM32PRO/f407/MDK-ARM/f407.uvprojx' --target f407 --chip STM32F407VET6
```

安装器检查 Target 与 Device，保留工程其余内容和换行；重复运行相同配置不会重复备份。它不会修改 `.uvoptx`、STM32 源码或重刷两块 ESP32。

启动器源码为 `integrations/keil/KeilDownloadLauncher.cs`，可运行同目录 `build_launcher.ps1` 用系统 .NET Framework C# 编译器重建。必须使用 Windows 子系统（`/target:winexe`）；命令行测试可用 `PidKeilDownload.exe <Python路径> <pid_keil.py路径> --resume <原记录路径>`，仍保留进度、日志和任务去重。

此工程当前启动会执行原来的电机/编码器应用。接入验收应烧录自己的 `f407.axf`；不要替换成手册里的 UART_ECHO 固件。PID 参数与 RUN/STOP 的从机 ACK 仍不能代表 STM32 已执行目标控制命令。

参考：[Keil 外部下载工具配置](https://www.keil.com/support/man/docs/uv4cl/uv4cl_dg_flashutil.htm)、[Keil 参数宏](https://www.keil.com/support/man/docs/uv4cl/uv4cl_ut_keysequence.htm)。

## 本次实板验收

2026-10-19 已通过 Keil 自身 `UV4.exe -f` 调用当前工程的外部下载工具，写入本工程 8,088 字节固件。任务 `743172b89f49a538c0df31c9` 为 `succeeded`，完整读回校验 `verified=true`、启动确认 `start_ack=true`。SHA256：`ab180c4e2ef870cf180fd842267c0d5fef8e588620e6c6422b0f633bef246d57`。原任务续查通过且没有创建第二个烧录任务；主机 P/I/D/STEP 与 USB 推送状态保留。

现有后端及接入测试共 34 项通过，包含地址越界、损坏 HEX、校验/启动失败拒绝成功、提交响应丢失后的同键续查等。验收记录：`PID_Workbench/validation/keil_integration/acceptance_20261019.json`。这不代替应用的电机运行或编码器转动验收。
