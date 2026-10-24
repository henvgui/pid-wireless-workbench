# PID 主机本地与 USB 功能版

更新：2026-10-16。本说明对应保留的 `host_local` 环境，适用于现有 ESP32-S3-WROOM-1-N16R8 主机。只需原生 USB 连接电脑，不启用 Wi-Fi、ESP-NOW 或蓝牙。2026-10-17 默认已切换到 [主从无线版](README_WIRELESS.md)；任务「主机：烧录本地版（关闭无线）」可恢复本文版本。

## 直接使用

1. 在 VS Code 打开 `ESP32_S3_RGB_Chase.code-workspace`。
2. 本地版使用 `platformio run -e host_local` 编译，任务「主机：烧录本地版（关闭无线）」上传。`Ctrl+Shift+B` 和通常的「主机：烧录」现对应 `host_wireless`。
3. 转 P / I / D 调参数，转 STEP 选择步长。OLED 显示四个数值并高亮最近操作的一行。
4. 按 SAVE 保存 P / I / D / STEP，断电后自动恢复。没有有效保存记录时，P/I/D 为 0.00，STEP 为 1.00。
5. 按 RUN/STOP 切换启停请求；OLED 显示 `RUN?` / `STOP?`。当前未接入从机和目标板，实际电机等执行器不会因此受控。

RGB 沿用已确认的效果：整圈渐变流水与同步呼吸；转动任意旋钮进入共享拖影，互补色背景、颜色缓慢渐变，停转 5 秒后平滑恢复呼吸。全局亮度上限 30/255，暗部约为峰值的 10%。细节见 [灯效与旋钮说明](README_HOST_PANEL.md)。

## 参数与保存

| 项目 | 范围或行为 |
|---|---|
| P、I、D | 0.00～9999.99，最多两位小数 |
| STEP | 0.01、0.10、1.00、10.00；旋钮到边界停止 |
| 编码器短按 | 切换自己的色板；STEP 在松手时生效，不保存参数 |
| STEP 长按 2 秒 | 关闭 / 开启整圈 RGB；按住只触发一次，松手不切色 |
| SAVE | 保存当前 P/I/D/STEP 到主机 NVS |
| RUN/STOP | 切换本地启停请求，带独立请求序号 |
| 上电 | 恢复有效保存记录；启停请求重新回到未发出状态 |

只有显式 SAVE 才写 Flash；旋钮调节不会逐格写入。当前支持一组参数，记录带版本号和 CRC 校验。亮度、颜色、方向、编码器跳变设置和 RUN 状态不写入该记录。

RGB 开关仅保存在本次运行中，重新上电默认开启；关灯不影响参数、OLED、USB 或无线功能，转动/短按不会自动开灯。USB 可用 `!rgb on`、`!rgb off`、`!rgb toggle`，`!status` 的 `rgb_enabled` 表示实际开关状态，`step_holds` 统计物理 STEP 长按次数。

屏幕顶部的 `*` 表示当前参数未保存；`SAVED` 表示参数与保存记录一致。`LOCAL` 表示本地模式尚未发出启停请求；`RUN?` / `STOP?` 表示等待确认，`RUN ACK` / `STOP ACK` 表示收到匹配当前请求的 USB 确认。保存或加载后会短暂显示提示，约 1.5 秒后恢复状态。

USB 确认只表示电脑发来了对应序号和状态，不能据此认定目标板已执行；实际执行结果需后续目标板协议提供。

## USB 命令

使用「主机：串口监视」或 Windows 上位机的手动命令输入框，115200、LF 或 CRLF。每条命令以英文 `!` 开头、以换行结束，命令名为小写。使用一个程序占用串口，烧录前关闭监视器。

| 命令 | 用途 |
|---|---|
| `!get` | 读取 P/I/D/STEP、保存状态和启停请求 |
| `!pid 1.25 0.10 2.50` | 一次设置 P、I、D；任一值非法则全部保持原值 |
| `!step 0.10` | 设置调节步长 |
| `!save` | 保存本机参数 |
| `!load` | 从保存记录恢复参数；没有有效记录则返回错误 |
| `!forget` | 删除保存记录，保留当前 RAM 数值；下一次重启使用默认值 |
| `!run` / `!stop` | 发出启停请求；重复同一状态保持同一个序号 |
| `!ackrun 1 1` | 确认请求 1 的 RUN；STOP 确认末项为 0 |
| `!stream on` / `!stream off` | 开启 / 关闭自动 JSON 事件，默认开启 |
| `!feedback 2 100 90.5 -0.2` | 向通道 2 注入目标值、测量值和输出，测试曲线数据接口 |
| `!status` 或 `?` | 完整的硬件、灯效、旋钮诊断文本 |
| `!help` | 查看命令帮助 |

例如逐行发送：

```text
!pid 1.25 0.10 2.50
!step 0.10
!save
!get
```

`!get` 等本地功能命令返回一行 JSON：

```json
{"type":"state","p":1.25,"i":0.10,"d":2.50,"step":0.10,"revision":2,"dirty":false,"saved":true,"storage":true,"run_request":false,"request_id":0,"ack":false,"stream":true}
```

`revision` 在本次启动中参数变化时递增，重启归零；`saved` 表示存在有效保存记录，`dirty` 表示当前值与记录不同或尚未保存，`storage` 表示 NVS 可用。`request_id=0` 表示本次启动尚未发出启停请求。

启停流程：先发 `!run`，读取返回的 `request_id`，再发 `!ackrun 对应序号 1`；`!stop` 会产生新序号，确认末项用 `0`。旧序号或不匹配的状态返回 `stale_or_mismatched_ack`，不会确认新的请求。主机重启不自动继续 RUN。

原有 `!knob P/I/D/STEP`、`!move cw/ccw`、`!color next`、`!brightness 0..30`、`!reverse on/off`、`!edges 2/4`、`!rgbtest on/off` 仍可用。`!reset` 将 RAM 参数、色板和位置恢复默认，但不删除保存记录；随后重启仍恢复已保存参数。删除记录使用 `!forget`。

普通字符保留回显，`!` 和 `?` 为控制字符。诊断及旧灯效命令返回文本，新本地命令返回 JSON；接收程序应按行解析、忽略空行，并区分两种格式。

## 自动事件与曲线接口

默认自动发送 JSON。参数变化最多每 100ms 合并发送一次最新值，不要求电脑持续轮询。

| `type` | 内容 |
|---|---|
| `params` | 与 `state` 相同字段，表示最新本地参数状态 |
| `run_request` | `request_id`、`run`、`ack`；表示启停请求 |
| `saved` | `ok`、`revision`；表示最近一次保存结果 |
| `telemetry` | `source`、`channel`、`time_ms`、`target`、`measured`、`error`、`output` |
| `error` | `code`，例如 `invalid_arguments`、`no_valid_profile` |

自动事件合并为最新状态，不作为每次按键或每个采样的完整历史。`!stream off` 仅关闭自动事件，主动查询和参数设置仍正常回复。

目前 `!feedback` 是电脑经 USB 注入的测试数据，通道 0～3，每个数值须为有限数且绝对值不超过 1,000,000。打开 stream 后，例如：

```text
!feedback 2 100 90.5 -0.2
```

返回命令确认，随后自动回报：

```json
{"type":"telemetry","source":"usb","channel":2,"time_ms":12345,"target":100,"measured":90.5,"error":9.5,"output":-0.2}
```

`time_ms` 为主机收到数据时的启动毫秒数，32 位计时约 49.7 天回绕；`error = target - measured`。这些字段为后续曲线页面预留，并不代表已连接传感器。Windows 上位机现有输入框可发命令，PID 专用控件、曲线页面及目标板实时数据尚未接入。

## 引脚与代码

| 功能 | GPIO |
|---|---|
| P A / B / 按键 | 4 / 5 / 6 |
| I A / B / 按键 | 7 / 10 / 11 |
| D A / B / 按键 | 12 / 13 / 14 |
| STEP A / B / 按键 | 15 / 16 / 17 |
| RUN/STOP / SAVE | 18 / 21，低电平按下，25ms 去抖 |
| OLED SDA / SCL | 8 / 9，当前可用地址 0x3C |
| RGB | 47，40 颗，沿用 RMT 时序 |

OLED 仍由独立任务处理 I²C，主循环负责旋钮、按钮、USB 和 RGB。主要新增文件：

- `include/host_local_core.h`、`src/host_local_core.cpp`：参数校验、保存记录编码、命令解析、启停请求状态。
- `include/host_local_app.h`、`src/host_local_app.cpp`：NVS、按钮、JSON 事件及 OLED 状态。
- `tests/host_local_test.cpp`：不依赖硬件的核心测试。
- `validation/check_host_local_20261016.py`：实际 USB 验证脚本，会临时写入测试参数并复位，结束时恢复先前参数及保存记录状态。

「灯效验收版：编译 / 烧录」对应保留的 `host_panel`，仅含此前的灯效、旋钮及四参数显示。它的参数只保存在 RAM，不含本版 SAVE 和 RUN/STOP 功能。其他独立测试环境也保留。

## 本次验证

- `host_local` 编译、上传成功；核心测试通过，覆盖数值边界、整组校验、保存记录损坏、请求序号及旧确认拒绝。
- 31 项真实 USB 检查通过：参数读写、错误输入不改值、保存/加载、启停确认、反馈 JSON、连续 12 次查询、RGB/OLED 刷新，以及实际复位后的参数和外设恢复。
- 复位后启停请求归零；测试结束恢复原先参数和保存记录状态。
- RUN/STOP、SAVE 的实物操作及新增 OLED 顶部布局仍需现场观察确认，USB 测试不代替实物按键验收。

记录：`validation/host_local_native_20261016.txt`、`host_local_build_20261016.txt`、`host_local_final_upload_20261016.txt`、`host_local_usb_20261016.json`。旧灯效环境回归编译及灯效逻辑测试也通过，见 `host_panel_regression_build_20261016.txt`、`host_ui_regression_20261016.txt`。
