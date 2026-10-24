# PID 主从共享通信代码

`PIDLink` 被主机 `host_wireless` 和从机 `slave_wireless` 共用。使用说明见 [主从无线版](../ESP32_S3_RGB_Chase/README_WIRELESS.md)。

- `src/pid_link_core.*`：48 字节显式编码、CRC、快照、序号、启动会话、重发和超时；不依赖 Arduino，可在 PC 测试。
- `src/PIDLink.*`：ESP-NOW 加密配对、固定信道、回调收件队列和主循环收发。
- `prepare_keys.py`：首次生成并保留两端共享的本机密钥；不打印密钥。
- `tests/core_test.cpp`：损坏包拒绝、旧 ACK 拒绝、重复包、会话变化、重发、超时及时间回绕。
- `validation/check_pair_20261017.py`：真实 COM14/COM18 联调，包含无线关闭/恢复和从机复位；最后恢复 RAM 参数并保持 STOP。

共享密钥保存在 `PIDLink/private/pair_keys.h`。两个工程必须使用同一份文件。无需安装第三方无线库，使用固定 Arduino-ESP32 版本自带的 Wi-Fi / ESP-NOW API。

新增 [PIDFlash](PIDFlash/README.md)：F103C8/G431 完整镜像分块传输、应用确认重传、CRC32/SHA256、ROM UART 擦写及完整读回。通过独立 `host_flash` / `slave_flash` 环境启用；日常默认及目标接口关闭策略保留。

2026-10-18 当前目标为 STM32F103C8T6，使用 `PIDFlash/tools/flash_stm32.py`；清单与确认均携带目标标识，旧 G431 入口保留。
