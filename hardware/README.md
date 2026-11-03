# PID 主从机硬件工程

2026-11-03 归档用户提供的嘉立创EDA专业版本地工程：[pid旋钮.eprj2](pid旋钮.eprj2)。

一个工程内包含两块板：

| 板子 | 原理图 | PCB |
|---|---|---|
| 主机 | Schematic1（P1） | PCB1 |
| 从机 | Schematic2（P1） | PCB2 |

使用支持 `.eprj2` 格式的嘉立创EDA专业版客户端打开文件。GitHub 中提供文件下载，图形编辑需在 EDA 客户端进行。客户端本地工程和在线导入包的区别见 [官方客户端说明](https://prodocs.lceda.cn/cn/faq/client/)；`.eprj2` 支持情况见 [官方 v3 更新记录](https://pro.lceda.cn/page/update-record)。

本次归档的是可编辑工程，没有另外生成 PDF、Gerber 或 BOM。主机固件位于 [ESP32_S3_RGB_Chase](../ESP32_S3_RGB_Chase)，从机固件位于 [ESP32_S3_Slave_Diagnostic](../ESP32_S3_Slave_Diagnostic)。

## 文件校验与账户字段处理

提供的文件为 SQLite 工程数据库，原文件大小 6,672,384 B。上传副本清除了 `users.password` 中的 1 条本地账户密码，并执行数据库整理，避免旧值残留；桌面原文件保持不变。

已比较所有表的全部逻辑数据，除该密码字段外完全一致，包括原理图、PCB、历史数据与工程结构；数据库完整性检查为 `ok`。本次没有改动电路或布线。

上传副本大小 **6,668,288 B**，SHA256：

```text
e9b2b879a0b1cba82de6c4f920b2448c22ae3adef32c97b2f83daac7a54f9030
```

原文件与上传副本的校验值、处理记录及工程清单见 [manifest.json](manifest.json)。
