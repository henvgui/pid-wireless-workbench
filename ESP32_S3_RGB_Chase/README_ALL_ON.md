# 40 颗 RGB 全灯常亮

环境 `rgb_all_on`：GPIO47，40颗灯全部白色常亮，亮度16/255，每250ms重发同一帧。使用固定RMT通道驱动和独立的器件手册时序。白色同时点亮三个通道，沿用历史全白程序的亮度16；“40”是灯的数量。

USB直接供电；无需上位机或串口监视器保持连接。程序不读取旋钮，不启动Wi-Fi或蓝牙。

在本工程目录运行：

```powershell
platformio run --environment rgb_all_on --target upload --upload-port COM14
```

USB串口115200，DTR=1、RTS=0。发送 `?` 查看 `RGB_ALL_ON` 状态，其他字符原样回显。`rgb_error=0` 表示驱动返回成功；实际灯光是否正常仍以实物观察为准。

原四旋钮源码和默认环境保留。该全亮程序是独立环境。

## GPIO47 输出回读（2026-10-14）

当前程序增加了 PCNT 内部脉冲计数。开机先发送16个慢速校准脉冲，然后由RMT发送全白数据；每帧应有40×24=960个上升沿。实机连续三次读到 `calibration_edges=16 gpio47_edges=960 expected_edges=960 out_sel=81 pcnt_in=47 pcnt_error=0`，发送计数与运行时间正常增长。

这确认GPIO47输入回读路径能看到输出脉冲；不能据此认定U2输出、首颗灯DI、供电或完整脉冲时序都正常，也不能代替实际灯光确认。

早期诊断版本误在RMT初始化后调用 `gpio_set_direction(...INPUT_OUTPUT)`，把输出映射覆盖成普通GPIO，造成零脉冲计数。该版本的 `rgb_all_on_pulses_20261014.json` 不是原故障证据。修正版只启用输入缓冲，保留RMT映射；结果见 `validation/rgb_all_on_pulses_fixed_20261014.json`。此行为核对了[ESP-IDF 4.4.7 的 GPIO 实现](https://github.com/espressif/esp-idf/blob/v4.4.7/components/driver/gpio.c)。

## 完整数据和时序回读

当前诊断同时用RMT RX通道4回读GPIO47，发送 `C` 可导出最近一帧的原始接收记录。该回读位置在ESP32引脚处，位于U2之前；它不能验证U2输出、首颗灯DI或5V供电。

器件资料 `work/rgb_chase/XL-5050RGBC-2812B-S.pdf` 第7页规定T0H最大400ns、T1L最大400ns。旧通用时序的T0H为400ns，T1L为450ns，后者超过该资料上限。`rgb_all_on` 通过宏 `PID_RGB_DATASHEET_TIMING=1` 使用以下参数；其他环境的时序没有随之改变。

| 参数 | 全亮测试当前值 |
|---|---|
| T0H / T0L | 300ns / 950ns |
| T1H / T1L | 900ns / 350ns |
| 每bit周期 | 1250ns |
| 帧尾低电平 | 至少300us |

软件接收三帧完整数据，均解出120个字节且全部为16；实测0/1高电平分别300/900ns、1码低电平350ns，帧内周期1250ns。记录见 `validation/rgb_all_on_datasheet_waveform_20261014.json`，上传记录见 `validation/rgb_all_on_datasheet_upload_20261014.txt`。截至该次记录，新的器件手册时序是否恢复点亮仍待使用者确认，不能认定它已解决原全灭问题。
