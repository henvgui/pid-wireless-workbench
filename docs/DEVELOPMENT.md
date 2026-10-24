# 首次配置与源码管理

## 本机工程

仓库根目录就是包含 `ESP32_S3_RGB_Chase`、`ESP32_S3_Slave_Diagnostic`、`PID_Shared` 和 `PID_Workbench` 的目录。保持这些相对位置，PlatformIO 的共享库及 STM32 示例才能找到协议代码。

在原电脑上，Git 管理源码不会改变已烧录程序，也不会更改本机已存在的配对密钥、主机保存的参数或上位机凭据。

## 配对和端口

- ESP-NOW 主从 MAC 当前写在 `PID_Shared/PIDLink/src/PIDLink.cpp`；更换硬件需要同时核对两端配置。
- 主机的 PlatformIO 下载端口目前为 COM14；其他电脑应改为实际主机端口。
- 从机按 `platformio.ini` 的 `custom_usb_serial` 识别 USB 身份；更换从机需更新该值。
- 在同一克隆中只生成一次配对密钥，两端编译使用同一文件。生成工具遇到已有文件会保留它。
- 配对密钥与本地 API token 被 Git 忽略，不要用 `git add -f` 强制提交。备份原密钥应使用单独的本机安全备份。

## 上位机构建

```powershell
python -m pip install -r PID_Workbench/requirements.txt
$env:PID_WORKBENCH_PYTHON = (Get-Command python).Source
& .\PID_Workbench\build.ps1
```

`build.ps1` 会先把共享协议工具同步到 `PID_Workbench/backend/protocol`，再用 Windows C# 编译器生成 EXE。EXE 运行时应与 `backend`、`integrations`、配置和文档目录放在一起。只复制 EXE 不构成完整安装。

`PID_WORKBENCH_PYTHON` 可覆盖默认 PlatformIO Python 路径。本地后台由 GUI 或 CLI 自动启动，监听 `127.0.0.1:18765`；认证文件由本机创建。`pidctl.cmd service` 返回经验证的凭据文件路径，不返回 token 内容。

项目中一些维护脚本和旧手册保留原电脑的完整路径。换电脑时以当前仓库路径、芯片目录和实际接线为准。实板维护脚本需要原目标镜像及原始验证资料，不能把历史验收记录当作本次设备状态。

## STM32 示例

`STM32F103_UART_ECHO` 使用相邻 `STM32F103_PA3_HIGH` 的启动文件；`STM32F407_UART_ECHO` 包含自身启动文件和链接脚本。它们的 `build.ps1` 使用 PATH 中的 `arm-none-eabi-gcc`、`arm-none-eabi-objcopy` 和 `arm-none-eabi-size`。

回传示例是专用测试应用。烧录它会替换目标现有应用，不应为了查看串口日志而自动刷入。用户车辆应用不随本仓库上传。

## 日常提交

```powershell
git status
git add README.md PID_Workbench ESP32_S3_RGB_Chase ESP32_S3_Slave_Diagnostic PID_Shared
git diff --cached --stat
git commit -m "Describe the change"
git push
```

`.gitignore` 限定了当前产品与可构建的 STM32 示例；历史实验、验证输出、缓存、私有密钥和可执行文件留在本机。以后新增一级目录时，需明确加入跟踪范围。原始实板证据和桌面 ZIP 属于本机备份，不依赖 Git 提交保存。
