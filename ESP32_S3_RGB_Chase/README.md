# PID 主机本地功能、RGB 与 USB 控制

项目整体功能及进度见 [主机、从机与上位机功能总览](PROJECT_OVERVIEW.md)。

2026-10-19 主机面板新增：**长按 STEP 2 秒切换整圈 RGB 开关**。按住只切换一次，松手不会额外切色；STEP 短按在松手时切色。关灯后仍可调参、看 OLED、使用 USB 和无线通信，普通旋钮操作不会自动重新亮灯。再次长按恢复原灯效；开关仅保存在本次运行中，重新上电默认开启。使用当前完整功能环境 `host_flash` 编译/上传。USB `!rgb on/off/toggle` 可控制同一开关，`!status` 可查询 `rgb_enabled`、`step_hold_ms`、`step_holds`。

2026-10-18 当前主板已安装 `host_flash`，包含无线目标烧录和 F103 文本回传。重新上传这一版本请使用任务「主机：无线烧录版：烧录」；大文本回传及发送自己文字的入口见 [F103 回传测试说明](../STM32F103_UART_ECHO/README.md)。

工程默认环境及普通「主机：烧录」任务仍为基础 `host_wireless`：保留 RGB、旋钮、OLED、SAVE 和 USB 功能，加入与从机的 ESP-NOW 加密通信、参数快照确认、重发及失联检测。该基础环境不包含目标烧录/文本回传扩展，详见 [主从无线版说明](README_WIRELESS.md)。

关闭无线的 `host_local` 环境仍保留，任务「主机：烧录本地版（关闭无线）」可恢复，详见 [本地与 USB 使用说明](README_HOST_LOCAL.md)。

RGB 上电显示整圈渐变流水并同步呼吸，暗部约为峰值的 10%，最高亮度 30/255；旋钮操作进入共享彩虹拖影和互补色背景，停转 5 秒后平滑返回。原灯效验收环境 `host_panel` 通过「灯效验收版」任务保留，详见 [灯效交互说明](README_HOST_PANEL.md)。四旋钮、STEP 单旋钮及 20 种灯效独立环境也保留。下文主要介绍旧 `rgb_chase`，其亮度、命令及保存行为与当前主机版本不同。

OLED 单独显示 `OK` 的测试使用 `oled_ok` 环境及「OLED：烧录」任务，详见 [OLED 测试说明](README_OLED.md)。

2026-10-13 新增 STEP 编码器独立测试（当时默认环境为 `step_encoder`）。旋转移动带拖尾亮点、停转冻结、按下切色，详见 [STEP 使用说明](README_STEP.md)。下文的 20 种自动灯效对应保留的 `rgb_chase` 环境，可通过“RGB：烧录”恢复。

适用于ESP32-S3-WROOM-1-N16R8，GPIO47（模组第24脚）驱动40颗XL-5050RGBC-2812B-S。程序不初始化Wi-Fi或蓝牙，保留原生USB回显。

开机自动轮播20种灯效，每种8秒，切换时用1秒淡入过渡；刷新间隔16ms，目标约60帧/秒。默认亮度40/255，所有USB亮度设置都受40/255上限约束。调速只改变灯效运动速度，轮播间隔和淡入时间保持不变。

## 灯效编号

| 编号 | 名称 | USB名称 |
|---:|---|---|
| 1 | 彩虹流水 | rainbow_flow |
| 2 | 双彗星 | twin_comets |
| 3 | 极光 | aurora |
| 4 | 往返扫描 | scanner |
| 5 | 彩虹追逐 | rainbow_chase |
| 6 | 多点涟漪 | ripples |
| 7 | 火焰流 | fire |
| 8 | 星空闪烁 | starfield |
| 9 | 等离子 | plasma |
| 10 | 海浪 | ocean |
| 11 | 彩色火星 | confetti |
| 12 | 萤火虫 | fireflies |
| 13 | 代码雨 | matrix |
| 14 | 熔岩 | lava |
| 15 | 烟花 | fireworks |
| 16 | 心跳 | heartbeat |
| 17 | 糖果色 | candy |
| 18 | 彩虹拉链 | rainbow_zipper |
| 19 | 三色波 | color_waves |
| 20 | 日出 | sunrise |

这些效果按灯珠的数据串联顺序计算，不假设PCB上的灯珠排成直线或圆环。

## USB命令

打开VS Code的PlatformIO串口监视器，使用115200，发送行尾选择LF或CRLF。命令以英文感叹号开头，每条单独一行：

| 命令 | 功能 |
|---|---|
| !help | 查看帮助 |
| !list | 查看20种灯效名称与编号 |
| !effect 15 | 固定播放第15种：烟花 |
| !effect ocean | 按名称固定播放海浪 |
| !next / !prev | 下一种 / 上一种，并切换到固定播放 |
| !auto on / !auto off | 开启轮播 / 固定当前效果 |
| !speed 150 | 运动速度设为150%，范围25～300 |
| !brightness 20 | 亮度设为20/255，范围0～40 |
| !pause / !resume | 暂停 / 继续动画和轮播计时 |
| !off / !on | 熄灭 / 重新点亮灯光 |
| !status | 查询当前状态 |
| ? | 无需换行的状态查询 |

例如，依次发送：

    !effect fireworks
    !speed 150
    !brightness 30

选灯效、next、prev会退出轮播、恢复动画并打开灯光。auto on也会恢复动画和打开灯光；off只关闭输出，动画计时仍继续。brightness 0不修改动画或轮播状态。

设置只保存在RAM，断电或复位后恢复默认轮播、100%速度、40/255亮度。普通字符仍原样回显，例如发送1收到1；!和?是保留控制字符。!命令必须以换行结束，收取途中超过2秒没有新字节会报错并恢复普通回显；过长或非法文本会报错，不执行截断命令。

状态中的number为用户编号1～20，effect为程序索引0～19。保留原来的RGB_SHOW、pin、leds、brightness、name、frames、uptime_ms字段，并增加auto、speed、paused、enabled、effects。

## VS Code与硬件

打开 ESP32_S3_RGB_Chase.code-workspace。`Ctrl+Shift+B` 编译当前主机无线版；下文旧 20 灯效使用「RGB：编译」「RGB：烧录」「RGB：串口监视」。上传前退出串口监视器以释放端口。主机项目固定 COM14，从机独立项目固定 COM18；Windows 端口变化时需在对应 `platformio.ini` 更新。

数据从GPIO47经过现有5V电平转换器和470Ω电阻送入第一颗RGB，再按OUT→IN串联到第40颗。RGB由5V供电，使用GRB顺序、800kbit/s协议。独立指示灯不计入这40颗。

## 代码位置

- include/rgb_show.h：GPIO、灯数量、亮度上限和灯效控制接口。
- src/rgb_show.cpp：20种灯效、计时、切换、调速、暂停和命令解释。
- src/main.cpp：USB收发与NeoPixel输出，沿用“先完成USB回复、再发送灯光帧”的顺序。
- tests/native_test.cpp：不依赖硬件的灯效和控制行为测试。
- reference/rgb_show_8_effects_main.cpp：此前已验证的8种灯效版本。
- reference/all_on_main.cpp、reference/chase_main.cpp：更早的常亮、流水灯程序。

PlatformIO固定为Espressif32 7.0.1，NeoPixel固定为1.12.5，Flash配置16MB；该功能不使用PSRAM。

## 验证记录

2026-10-09：20种效果的本地测试及PlatformIO编译通过，经COM14烧录、写入校验成功。约40秒USB实测完成127项检查：逐一选中20种效果，验证调速、调亮度、暂停、熄灯、轮播、错误输入恢复、分段命令和1回显，未观察到重启。验证后恢复自动轮播、100%速度、40/255亮度；记录见rgb_show_validation_20261009.json。USB状态确认程序和控制接口正常响应，实际灯色需结合现场观察。

2026-10-07：旧8种灯效版本已通过约66秒观察，34次状态查询和1回显通过，详见rgb_show_validation_20261007.json。这份旧报告对应reference中的旧代码，不代表新增20种效果的验证结果。
