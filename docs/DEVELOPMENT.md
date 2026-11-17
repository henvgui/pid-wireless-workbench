# 首次配置与源码管理

## 本机工程

仓库根目录就是包含 `ESP32_S3_RGB_Chase`、`ESP32_S3_Slave_Diagnostic`、`PID_Shared` 和 `PID_Workbench` 的目录。保持这些相对位置，PlatformIO 的共享库及 STM32 示例才能找到协议代码。

在原电脑上，Git 管理源码不会改变已烧录程序，也不会更改本机已存在的配对密钥、主机保存的参数或上位机凭据。

## 配对和端口

现有已配对的套件可直接换电脑使用。工作台通过原生 USB 和主机协议识别 ESP32-S3，COM 会随电脑和端口变化。

给新套件初始化时，使用与项目原理图接线和 Flash/PSRAM 规格相符的主从 ESP32-S3。先在两块板的原生 USB 口使用数据线连接电脑，记下真实序列号和主机 COM 口。使用 PlatformIO 的终端在仓库根目录运行。下面的 PowerShell 变量由你在运行时输入。

```powershell
pio device list
$pidHostMac = Read-Host '主机 USB 序列号，例如 AA:BB:CC:DD:EE:02'
$pidSlaveMac = Read-Host '从机 USB 序列号'
$pidHostCom = Read-Host '主机实际 COM 口，例如 COM14'
python PID_Shared/PIDLink/prepare_keys.py --host $pidHostMac --slave $pidSlaveMac
pio run -d ESP32_S3_RGB_Chase -e host_flash -t upload --upload-port $pidHostCom
$env:PID_SLAVE_USB_SERIAL = $pidSlaveMac
pio run -d ESP32_S3_Slave_Diagnostic -e slave_flash_psram -t upload
Remove-Item Env:PID_SLAVE_USB_SERIAL
```

- 上面两个 upload 命令分别写入 ESP32 主机和从机。若其中一块之前刷过本项目，主机 NVS 可能仍保存旧的从机名单，可用 [从机指南](../PID_Workbench/docs/PEER_SELECTION.md) 的 `!peer add GREEN <从机 MAC>` 和 `!peer select GREEN` 更新。新增从机用同一克隆的密钥及主机地址编译，上传时把 `PID_SLAVE_USB_SERIAL` 设置为新从机的序列号，再设置身份并登记。
- 新套件的地址文件和随机配对密钥由脚本写到 `PID_Shared/PIDLink/private/`。同一套件使用同一个克隆构建，两端共用这份配置。已有配置会保留。为不同套件准备新克隆。
- 沿用原开发板和已有本地密钥时，`prepare_keys.py` 可以不带参数。未生成自定义地址配置时编译沿用原开发板地址。
- 配对密钥与本地 API token 由 Git 忽略。备份原密钥使用单独的本机安全备份。

## 上位机构建

```powershell
python -m pip install -r PID_Workbench/requirements.txt
$env:PID_WORKBENCH_PYTHON = (Get-Command python).Source
& .\PID_Workbench\build.ps1
```

`build.ps1` 会先把共享协议工具同步到 `PID_Workbench/backend/protocol`，再用 Windows C# 编译器生成 EXE。EXE 运行时应与 `backend`、`integrations`、配置和文档目录放在一起。只复制 EXE 不构成完整安装。

`PID_WORKBENCH_PYTHON` 可覆盖默认 PlatformIO Python 路径。本地后台由 GUI 或 CLI 自动启动，监听 `127.0.0.1:18765`；认证文件由本机创建。`pidctl.cmd service` 返回经验证的凭据文件路径，不返回 token 内容。

项目中一些维护脚本和旧手册保留原电脑的完整路径。换电脑时以当前仓库路径、芯片目录和实际接线为准。实板维护脚本需要原目标镜像及原始验证资料，不能把历史验收记录当作本次设备状态。

用于发布 Windows 便携包时运行 `& .\PID_Workbench\package-windows.ps1`。输出位于 `PID_Workbench/output/release/`，包含 ZIP 与 SHA256 清单。

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
