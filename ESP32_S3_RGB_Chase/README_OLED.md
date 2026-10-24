# OLED 显示 OK 测试

主机 OLED 接线：SDA 为 GPIO8、SCL 为 GPIO9，供电 3.3V。GPIO 编号与模组焊盘编号不同。

在本工程执行 VS Code 任务「OLED：烧录」或：

```powershell
platformio run --environment oled_ok --target upload
```

默认按 SSD1306 128×64 显示屏，在中央静态显示 `OK`。自动尝试 7 位地址 0x3C 和 0x3D，I2C 速度 100kHz。不等待电脑串口连接，不启用 Wi-Fi、蓝牙或旋钮/RGB 控制。

USB 串口波特率 115200，发送 `?` 查询驱动、地址、I2C 应答、总线电平及运行时间；发送大写 `S` 扫描 I2C 的 0x01～0x7E 地址，其他字符回显。`oled_ack=1` 仅表示 I2C 设备应答，屏幕文字是否正常需现场查看，地址也不能确定驱动型号。

OLED 操作在独立任务中执行，USB 每两秒报告一次状态；`stage` 表示显示任务当前阶段，正常刷新后为 `idle`，方便定位初始化卡住的问题。

如果有应答但屏幕不显示或显示异常，使用「OLED：烧录 SH1106」任务，或将命令中的环境改为 `oled_ok_sh1106`。其他分辨率或控制器需按实物调整。

四旋钮独立测试通过「旋钮：烧录」恢复；当前默认编译环境为包含旋钮、RGB 与 OLED 的主机整合固件 `host_panel`，使用「主机：烧录」上传，详见 [主机说明](README_HOST_PANEL.md)。

## 2026-10-15 实测通过

- 使用者确认 OLED 正常显示 `OK`。目前 SSD1306 128×64 配置可用；尚未确认屏幕控制器的实际型号。
- OLED VCC、SDA、SCL 对模块 GND 的静态电压，使用者均报告约 3.3V。
- COM14 上传成功；USB 状态持续返回 `oled_ack=1`、`address=0x3C`、`stage=idle`，刷新计数增长。全地址扫描只发现 0x3C，没有超时或其他错误。
- 已修正初始化冲突：先由 `Wire.begin(8, 9, 100000)` 配置引脚，U8g2 硬件 I2C 构造函数不再重复指定 SDA/SCL。此前重复指定会使 U8g2 的 GPIO 初始化在 Wire 启动后重新调用 `pinMode(OUTPUT)`，覆盖引脚的 I2C 输出配置；现场日志曾持续停在 `oled_init`。
- 原始烧录与修正验证记录见 `validation/oled_pin_routing_fix_upload_20261015.txt`、`validation/oled_pin_routing_fix_20261015.json`。
