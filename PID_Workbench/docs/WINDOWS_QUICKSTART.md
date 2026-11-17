# Windows 上位机快速开始

适用于 Windows 10/11 x64，压缩包内含 Python 运行时和 pyserial。电脑连接已配对的 ESP32-S3 **主机**的原生 USB 口。从机与 STM32 正常供电。首次使用空白 ESP32-S3 时，先按文末的硬件准备文档给两块板写入各自固件。

1. 从 GitHub Releases 下载 `PID-Workbench-Windows-x64.zip` 并完整解压。双击 `PID_Workbench.exe`。
2. 选择「自动识别 ESP32 主机」，在软件中查看当前从机和无线连接状态。如果有多个主机，刷新列表后选择本次使用的 COM 端口；主机 OLED 显示当前选择的从机名称。
3. **读取串口**。从机 H2 的 TX 接 STM32 RX、RX 接 STM32 TX，按板型接 GND 和 3.3 V 参考。在「03 无线串口」中选择 H2，填写目标应用的波特率、数据位、校验位和停止位，点击「连接串口」。目标应用打印后就能看到数据，也可以按应用协议发送文本或 HEX。
4. **远程烧录**。在「固件烧录」选择 STM32 完整型号和已接线的接口，选择链接到 `0x08000000` 的 BIN，先点「检查镜像」，再点「开始烧录」。任务会写入并完整读回校验。需要启动应用时勾选界面的启动选项。F103C8T6 的 UART ROM 和 F407VET6 的 SWD 有实板验收记录。
5. **从 Keil 下载**。有 Keil 的电脑可为 F407 的 Target 配置下载器。运行压缩包内 `runtime/python.exe` 和 `integrations/keil/configure_keil.py`，传入自己的工程、Target、完整料号和 `fromelf.exe` 路径。配置后重开工程，Build 后点 Download。具体参数参见下方 Keil 指南。

诊断可运行 `pidctl.cmd ports` 和 `pidctl.cmd device --port auto`。如果找不到主机，核对数据线、原生 USB 口和 `host_flash` 固件，并关闭占用同一 COM 口的串口工具。升级时若旧版后台还在运行，等烧录任务结束，关闭旧工作台，再重启电脑或退出旧后台进程后启动新版。

[GitHub Releases](https://github.com/henvgui/pid-wireless-workbench/releases/latest) · [空白 ESP32 硬件准备](https://github.com/henvgui/pid-wireless-workbench/blob/main/docs/DEVELOPMENT.md#配对和端口) · [H2 串口指南](https://github.com/henvgui/pid-wireless-workbench/blob/main/PID_Workbench/docs/WIRELESS_SERIAL.md) · [Keil 接入](https://github.com/henvgui/pid-wireless-workbench/blob/main/PID_Workbench/docs/KEIL_DOWNLOAD.md) · [STM32 型号及接线入口](https://github.com/henvgui/pid-wireless-workbench/blob/main/PID_Workbench/docs/STM32_FLASH_TARGETS.md)
