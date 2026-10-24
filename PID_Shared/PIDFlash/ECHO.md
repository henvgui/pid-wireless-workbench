# F103 / F407 端到端回传协议

更新：2026-10-18。复用已有固定对端加密 ESP-NOW、PF 帧和 USB 文件环境，增加 `!echo` 会话；不改变固件烧录命令。使用前需在 F103 运行 `../../STM32F103_UART_ECHO/uart_echo.bin`。

2026-10-19 增加 F407 应用 [STM32F407_UART_ECHO](../../STM32F407_UART_ECHO/README.md)：通过 SWD 烧录，USART1 PA9/PA10 回传，握手返回 `F407VE01`（VET6）；VGT6 保留 `F407ECHO`。新版从机接受 CRC 正确的 `F103ECHO` / `F407ECHO` / `F407VE01`，电脑工具自动识别并记录 `target_part`、`target_marker`，可加 `--part STM32F407VET6` 限定期望应用。帧格式、长度、CRC、去重及重试逻辑保持兼容；下文 F103 验收记录仍为历史真实结果，不代表 F407 已通过。

## USB 命令

```text
!echo open ID
!echo open ID RADIO_DROP_EVERY UART_CORRUPT_EVERY
!echo data ID OFFSET HEX_DATA
!echo close ID TOTAL_BYTES
```

ID 为非零十六进制 32 位数；偏移/长度/故障注入间隔为十进制。故障注入间隔为 0 时关闭。成功回复为 `type:"echo"` JSON：`id`、`op`、`offset`、`ok`、`error`、返回数据 `hex`、应用层重试数，以及主机主动丢弃计数。

- `open` 的操作码为 8。F103 返回 8 字节 ASCII `F103ECHO`，才确认 UART 应用握手成功。
- `data` 的操作码为 9。最多 128 字节，必须等匹配的返回数据后才发送下一块。回复中的 `hex` 来自经过 CRC 验证的 F103 原文回传。
- `close` 的操作码为 10。F103 返回 LE32 总字节数、流 CRC32 和唯一数据块数；从机追加 LE32 串口重试数、主动无线丢弃数和主动串口损坏数，共 24 字节。返回串口及转换器到关闭状态。

原 `!fw` 的操作码 1..7 保留，新增目标回包操作码 11。帧仍为 `PF` + 版本 1 + 操作码 + sender/peer 启动会话 + transfer ID + 请求序号 + offset + length + reserved + payload + CRC32；总长 32+payload。F103 将请求的 sender/peer 交换后回包，保留 transfer、请求序号和 offset，重新计算 CRC。无线和 UART 都使用同一帧。

## 并发与重试

USB 每次只允许一个待确认请求。Echo 会话与目标烧录互斥：Echo 活动时不接受新的固件 Begin/Flash；固件任务占用时不允许 Open；RUN 在 Echo/文件会话期间拒绝。STOP、已有显示/灯效和链路心跳继续工作。

从机收到相同已完成请求，重发上次经 F103 确认的缓存回复。UART 处理中收到相同请求，不重复开启传输。UART 超时后最多重发 3 次，F103 对完全相同请求重发缓存；若电脑重试产生新请求序号，但仍是上一块相同 offset 和内容，F103 返回该块而不重复累计。旧序号、错 offset、冲突内容和已关闭会话的数据均不推进状态。

F103 的流式解析器限制帧长 160 字节，丢弃 CRC 损坏或超长帧，并扫描恢复帧边界。协议只用于测试回传，不提供可重复执行控制命令的通用恰好一次语义。尚未实现跨掉电持久化续传；断电后重新开始文件。

电脑入口为 `tools/echo_stm32.py`，可选 `--text`、`--file`、`--bytes`，`--journal` 保存结果；默认生成 128 KiB 文本。完整使用示例见 [F103 测试项目](../../STM32F103_UART_ECHO/README.md)。

## 验证记录

- `validation/echo_native_20261018.txt`：独立模拟 131091 字节二进制回传，丢失、损坏、重复/新序号重试、流统计、错误会话及串口解析恢复。
- `validation/echo_tools_20261018.txt`：电脑工具及原烧录工具测试，损坏数据、错误汇总和超时均不能显示成功。
- `validation/echo_app_program_20261018.json`：实板目标程序完整写入、读回一致及 GO 确认。
- `validation/echo_large_20261018.json`：131091 字节 UTF-8 文本实板全链路往返，完整比较/CRC32/SHA256 一致，56.954 秒，应用层无线/串口重试均 0。
- `validation/echo_faults_20261018.json`：同样 131091 字节，每第 5 个 Echo 无线发送被丢弃、每第 7 个数据块首次 UART 发送 CRC 被破坏。最终原文/返回数据一致，CRC32/SHA256/1025 个唯一数据块一致；无线重试 714 次、UART 重试 146 次，158.172 秒。
- `validation/echo_outage_20261018.json`：每个 Echo 无线发送都被丢弃，工具在有界重试后明确失败，`passed:false`、返回 0 字节；没有将超时当成成功。
- `validation/echo_interlock_20261018.json`：会话活动期间，第二次 Open、RUN 和新固件 Begin 均被拒绝，原会话仍可正常关闭。

这些是当前接线、距离和干扰环境下的实测，不能推导为任意距离/负载都零丢包。正常测试的有效单方向文本吞吐约 2.3 KB/s（包含整个双向回传的等待时间），本版优先验证完整性，尚未做滑动窗口或高波特率优化。

应用层确认和序号重试的依据：[Espressif ESP-NOW 指南](https://docs.espressif.com/projects/esp-idf/en/release-v5.0/esp32s3/api-reference/network/esp_now.html)。F103 UART 配置依据：[ST RM0008](https://www.st.com/resource/en/reference_manual/rm0008-stm32f103xx-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)。

2026-10-19 F407VET6 实板验收：无线 SWD 烧录/完整读回/启动通过。131091 字节正常回传 53.546 秒；主动故障轮次 155.954 秒，无线重试 714 次、UART 重试 146 次，全文、CRC32、SHA256 和 1025 个唯一数据块一致。持续丢弃全部无线测试包明确失败，不报告成功。报告见 `../../PID_Workbench/validation/f407ve_echo_acceptance_20261019.json`。
