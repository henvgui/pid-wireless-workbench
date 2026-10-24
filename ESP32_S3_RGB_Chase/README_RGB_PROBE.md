# RGB 不亮：供电与数据通路检查

2026-10-14 在 COM14 上读到了 STEP 固件状态和 `1` 回显，已确认 ESP32 在运行。亮度为 40；重新发送亮点、切换青色并移动到第 40 颗后，使用者仍观察到全部不亮。当前尚未确认故障点。

先保持原 STEP 固件，黑表笔接主板 GND，检查：

| 测量点 | 预期 |
|---|---|
| 第 1 颗 RGB 的 VDD / 1 脚 | 约 5V |
| 第 1 颗 RGB 的 GND / 3 脚 | 约 0V |
| U2 的 VCC / 5 脚 | 约 5V |

以上数值必须实测。ESP32 的 3.3V 正常不代表 RGB 的 5V 和地连接也正常。

## 供电确认后的静态信号检查

`rgb_probe` 是独立诊断环境，默认编译/烧录环境仍是 `step_encoder`。诊断程序只控制 GPIO47 的静态电平，不产生 RGB 颜色数据；因此烧录该程序后灯光不亮是正常现象。

烧录命令（先释放串口）：

    platformio run --environment rgb_probe --target upload --upload-port COM14

USB 发单个 `H`：GPIO47 保持高电平；发单个 `L`：保持低电平；发 `?`：查询状态；其他字符回显。每次切换后读回 `RGB_PROBE` 状态，保持电平不变后用万用表测量。

| 测量点 | H 时预期 | L 时预期 |
|---|---|---|
| 模组第 24 脚 GPIO47 / RGB_IO | 约 3.3V | 接近 0V |
| U2 第 2 脚 A 输入 | 约 3.3V | 接近 0V |
| U2 第 4 脚 Y 输出 | 接近 U2 的 5V 供电 | 接近 0V |
| R22 两端、第 1 颗 RGB 第 4 脚 DI | 接近 U2 的 5V 供电 | 接近 0V |

U2 第 1 脚 /OE 应始终接近 0V。数据路径按原主机 PCB 核对为：GPIO47 → U2 输入2脚 → U2 输出4脚 → R22（470Ω）→ 第1颗 RGB 的 DI（4脚）。这些读数用于找出哪一段没有传递高/低电平，不能代替 800kbit/s 波形检查或直接证明灯珠损坏。

检查结束恢复 STEP 程序：

    platformio run --environment step_encoder --target upload --upload-port COM14

2026-10-14 后续记录：使用者确认 RGB 的 VDD、GND 和 U2 的 VCC 正常（未提供具体数值）。静态诊断程序已通过 COM14 烧录并校验，已发送 `H` 并读回 `RGB_PROBE gpio=47 level=1`。当前保持高电平，等待 U2 输入/输出和第一颗 RGB DI 的实测电压；程序读回的状态不是物理电压测量。烧录记录见 `validation/rgb_probe_upload_20261014.txt`，USB 记录见 `validation/rgb_probe_20261014.json`。

后续测量确认：输入端为3.3V，输出和首颗 DI 约4.87～5V；切换为低电平后，使用者确认首颗 DI 接近0V。随后烧录 `rgb_diagnostic`，让全部40颗灯每2秒轮换红/绿/蓝、每250ms重发颜色数据；使用者确认40颗都亮并能切色。此时已离开静态探测模式。原全灭根因仍未完全确定，记录见 `validation/rgb_diagnostic_20261014.json`。
