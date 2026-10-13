# Muse Gadgets 中文操作指南
### 目标硬件：微雪 Waveshare ESP32-S3-Touch-LCD-1.85B

> 本文件是仓库 [README.md](README.md) 与 [`esp32/README.md`](esp32/README.md) 的中文译述与操作指引，
> 并补充了**面向 Windows 用户**的分步说明（上游文档只写了 macOS / Linux）。
>
> **风险提示**：本项目由「黑客为黑客」而写，纯属好玩。刷入自定义固件可能导致开发板变砖、
> 保修失效。一切后果自负。请务必先完成[第 4 步：备份原厂固件](#第-4-步备份原厂固件)。

---

## 目录

1. [这是什么](#1-这是什么)
2. [你的开发板](#2-你的开发板)
3. [准备清单](#3-准备清单)
4. [分步操作](#4-分步操作)
   - [第 1 步：获取 SDK Token](#第-1-步获取-sdk-token)
   - [第 2 步：安装 ESP-IDF v6.0.1](#第-2-步安装-esp-idf-v601)
   - [第 3 步：填写 SDK Token 并构建](#第-3-步填写-sdk-token-并构建)
   - [第 4 步：备份原厂固件](#第-4-步备份原厂固件)
   - [第 5 步：擦除并刷入](#第-5-步擦除并刷入)
   - [第 6 步：在手机 Muse App 中配对](#第-6-步在手机-muse-app-中配对)
5. [日常使用](#5-日常使用)
6. [命令速查](#6-命令速查)
7. [故障排查](#7-故障排查)
8. [已知限制与注意事项](#8-已知限制与注意事项)
9. [恢复微雪原厂固件](#9-恢复微雪原厂固件)
10. [进阶：改固件、换头像、加指令](#10-进阶改固件换头像加指令)
11. [许可证](#11-许可证)

---

## 1. 这是什么

Muse Gadgets 是 Meta 开源的一套**自建设备（DIY Gadget）**方案：

- **ESP32 设备 SDK**（[`esp32/`](esp32)）——把一块现成的 ESP32 开发板变成能连上 Muse 的智能终端：
  屏幕显示、按键、麦克风与扬声器、以及传感器。
- **Linux 设备 SDK**（[`linux/`](linux)）——把闲置的树莓派 / Linux 主机变成 Muse 设备，
  可以替你跑系统运维任务或接管 Home Assistant。

刷入固件后，设备会通过网络与 **Muse**（Meta 的个人 AI 助理）建立**加密会话**。
带「家庭网络隧道」的板子，Muse 可以进一步访问你家里已有的设备，
以及任何暴露了本地 HTTP API 的东西。

**说人话**：刷完之后，你手上这块 1.85 寸圆屏手表/徽章就变成一个可以按住说话、
Muse 用文字（字幕）回答、还能显示图片的语音小助手。

本项目（`blacksamuraiiii/muse-esp32-s3-touch-lcd-1.85b`）在 Meta 官方 SDK 基础上，
**新增了对微雪 ESP32-S3-Touch-LCD-1.85B 这块板子的移植支持**。

---

## 2. 你的开发板

| 项目 | 参数 |
|---|---|
| 主控 | ESP32-S3R8，Xtensa 双核 LX7，最高 240 MHz |
| 内存 | 512 KB SRAM + **8 MB Octal PSRAM**（片上堆叠） |
| 存储 | **16 MB NOR Flash**（外挂） |
| 屏幕 | 1.85 英寸 **360 × 360** 圆屏 TFT，262K 色，亮度 450 cd/m²，对比度 1200:1 |
| 屏幕驱动 | **ST77916**，**QSPI** 接口 |
| 触摸 | **CST816S** 电容触摸，I2C（7 位地址 `0x15`） |
| 音频输出 | **ES8311** 编解码器 + 功放使能引脚（`PA_CTRL`） |
| 音频输入 | **ES7210** 双麦克风阵列 + 回声消除 |
| 姿态传感器 | QMI8658 六轴 IMU（加速度 + 陀螺仪） |
| 电量计 | BQ27220 |
| 按键 | **BOOT**（可编程）、**PWR**（纯硬件电源键，固件读不到） |
| 接口 | Type-C（含 USB Serial/JTAG）、SH1.0 4PIN 串口、SH1.0 4PIN GPIO、MX1.25 2PIN 锂电池 |
| 外壳 | CNC 铝合金 |

**引脚定义**（来自[微雪官方文档](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85B)，
已写入 `esp32/devices/sdkconfig.muse-waveshare-s3-touch-lcd-185b` 与
`esp32/components/muse/boards/board_waveshare_s3_touch_lcd_185b.c`）：

| 功能 | 引脚 |
|---|---|
| LCD QSPI | `CS=21`、`PCLK=40`、`D0=46`、`D1=45`、`D2=42`、`D3=41`、`RST=3`、`BL=5` |
| 触摸 CST816S | `SCL=10`、`SDA=11`、`RST=1`、`INT=4` |
| I2S 音频 | `MCLK=2`、`BCLK=48`、`LRCK=38`、`DOUT=47`、`DIN=39`、功放使能 `PA=9` |
| 按键 | `BOOT=GPIO0` |

> 注意：`GPIO45 / GPIO46` 同时是芯片的 strapping 引脚（`GPIO45` 在复位时决定 `VDD_SPI` 电压），
> 这是微雪原厂设计，属于已知取舍，无需处理。

---

## 3. 准备清单

必须准备好的东西：

- [ ] **一块 ESP32-S3-Touch-LCD-1.85B**（本仓库的移植目标）
- [ ] **一根能传数据的 USB-C 线**（不能是「只供电」的线，这是最常见的踩坑点）
- [ ] **一台电脑**（本指南针对 **Windows**；macOS / Linux 步骤见上游 README）
- [ ] **一个 SDK Token** —— 从 <https://gadgets.muse.ai/settings/sdk-tokens> 获取
      （Account > SDK tokens）。**每台设备都需要一个 Token 才能配对**，
      自己给自己做的设备也一样。使用前请先阅读
      [Gadget SDK Terms](https://gadgets.muse.ai/sdk-terms)
- [ ] **手机上的 Muse App**（iOS / Android），用于首次配置设备
- [ ] **网络环境**：构建时需要从 Espressif 组件仓库下载依赖

---

## 4. 分步操作

### 第 1 步：获取 SDK Token

1. 打开 <https://gadgets.muse.ai/settings/sdk-tokens> 并登录。
2. 生成一个 SDK Token。它的格式固定为：`mgst_` + 43 个 base64url 字符，**总长 48 字符**。
3. 把 Token 复制到安全的地方暂存，第 3 步要用。

> ⚠️ 构建脚本 `esp32/cmake/validate_config.cmake` 会**强校验 Token 格式**：
> 如果 Token 非空但长度不是 48 或前缀不对，**构建会直接报错终止**。
> 如果完全留空，构建只会**警告**，固件能刷进去，但**无法配对**。

---

### 第 2 步：安装 ESP-IDF v6.0.1

本固件**只支持 ESP-IDF v6.0.1**，其他版本不受支持。

#### 方式 A（推荐）：官方 Windows 安装器

1. 打开 <https://dl.espressif.com/dl/esp-idf/>，选择 **v6.0.1** 对应的 Windows 安装器。
2. 安装时勾选目标芯片 **esp32s3**。
3. 安装完成后，开始菜单里会出现 **「ESP-IDF v6.0.1 PowerShell」**（或 CMD），
   **以后每次构建都从这个快捷方式启动终端**。

#### 方式 B：Git 手动安装

前提：已安装 [Git for Windows](https://git-scm.com/download/win) 与 Python 3。

```sh
git clone -b v6.0.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6
cd ~/esp/esp-idf-v6
./install.bat esp32s3
./export.bat          # 每个新终端都要执行一次
```

> 在 Git Bash 中对应使用 `./install.sh esp32s3` 与 `. ./export.sh`。
> 本仓库的便利脚本 `esp32/tools/muse/board.sh` 会自动按以下路径寻找 ESP-IDF：
> `~/.espressif/esp-idf-v6.0.1` → 环境变量 `IDF_PATH` → `~/esp/esp-idf-v6.0.1` →
> `~/esp/esp-idf-v6` → `~/esp/esp-idf`。装在别处就设 `IDF_EXPORT=<export 脚本路径>`。

验证安装：

```sh
idf.py --version     # 应输出 ESP-IDF v6.0.1
```

---

### 第 3 步：填写 SDK Token 并构建

#### 3.1 填写 Token

```sh
cd esp32
idf.py menuconfig
```

在菜单中找到 **ESP32 Device SDK** → **Muse Gadgets SDK token**，把第 1 步的 Token 粘进去，
保存退出（`Q` → `Y`）。

> 也可以直接改对应构建目录下的 `sdkconfig` 里的 `CONFIG_GADGET_SDK_TOKEN`，
> 但**首次构建必须先生成 sdkconfig**，所以推荐用 menuconfig。

#### 3.2 构建

**方式 A：用便利脚本（推荐）**

```sh
cd esp32
tools/muse/board.sh build lcd185b
```

这个脚本会自动：
- 使用 `build-muse-waveshare-s3-touch-lcd-185b/` 作为独立构建目录；
- 按顺序加载三个配置文件 `sdkconfig.defaults` → `devices/sdkconfig.muse` → `devices/sdkconfig.muse-waveshare-s3-touch-lcd-185b`；
- 把日志写到 `/tmp/muse_build_lcd185b.log`；
- 构建前后清理 `managed_components/`，避免与其他板子的依赖冲突。

**方式 B：直接用 idf.py（便于排查问题）**

```sh
cd esp32
idf.py -B build-muse-waveshare-s3-touch-lcd-185b -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-waveshare-s3-touch-lcd-185b/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-touch-lcd-185b" \
  build
```

首次构建会从 Espressif 组件仓库下载 LVGL、`esp_codec_dev`、`esp_lvgl_adapter`、
`esp_lcd_st77916`、`esp_lcd_touch_cst816s` 等依赖到 `managed_components/`，需要联网。

> ### 🇨🇳 国内网络：组件仓库加速（重要）
>
> 组件包文件托管在 CloudFront（`components-file.espressif.com`），**国内网络下常常无法访问或极慢**，
> 典型报错：
>
> ```
> CMake Error ... ERROR: Cannot establish a connection to the component registry.
>   URL: https://components-file.espressif.com/components/espressif/cjson.json
> ```
>
> 解决办法：把文件源指向 Espressif 中国镜像。**在同一个终端里、执行 `idf.py` 之前**设置环境变量：
>
> - PowerShell：
>   ```powershell
>   $env:IDF_COMPONENT_STORAGE_URL = "https://components-file.espressif.cn/"
>   ```
> - CMD：
>   ```bat
>   set IDF_COMPONENT_STORAGE_URL=https://components-file.espressif.cn/
>   ```
> - Git Bash / macOS / Linux：
>   ```sh
>   export IDF_COMPONENT_STORAGE_URL=https://components-file.espressif.cn/
>   ```
>
> 注意这是**临时环境变量**，每次新开终端都要重新设置；想长期生效就写进系统环境变量。
> 该镜像只影响组件包下载，版本解析仍走官方 API（可正常访问）。

产出的固件位于 `build-muse-waveshare-s3-touch-lcd-185b/muse-gadget.bin`。

> 💡 **改了配置文件后必须删掉构建目录**：生成出来的 `sdkconfig` 会覆盖后来对
> `sdkconfig.defaults` 或 overlay 的修改。改完配置就 `rm -rf build-muse-waveshare-s3-touch-lcd-185b` 重新构建。

---

### 第 4 步：备份原厂固件 ⚠️ **务必执行**

这块板出厂预装的是微雪的 LVGL 演示固件。**在第一次刷入 Muse 之前必须完整备份**，
否则刷完就回不去了。

**先确认串口号**：插上 USB，在 Windows「设备管理器 → 端口 (COM 和 LPT)」里
找到形如 `USB 串行设备 (COMx)` 的条目，记下 `COMx`。

```sh
python -m esptool --chip esp32s3 -p COM5 read-flash 0 0x1000000 esp32-s3-touch-lcd-1.85b-backup.bin
```

- `0x1000000` = 16 MB，对应本板的 Flash 容量。
- 请把备份文件**保存到仓库之外**（仓库的 `.gitignore` 已排除构建产物，
  但备份属于个人数据，不要提交）。

---

### 第 5 步：擦除并刷入

#### 5.1 擦除 Flash（首次刷入必须做）

微雪原厂固件的 NVS / 数据分区布局与 Muse 不同，**不擦除会导致固件起不来**。

```sh
python -m esptool --chip esp32s3 -p COM5 erase-flash
```

#### 5.2 刷入 Muse 固件

**方式 A：便利脚本**

```sh
cd esp32
tools/muse/board.sh flash lcd185b COM5
```

> 不带串口参数时，脚本会通过 `tools/muse/ports.py` 按 USB 设备类型自动寻找端口。
> 如果同时插了多块同类板子，需要传入**设备 USB 序列号**（原生 USB 上是芯片 MAC）
> 来区分。用 `python tools/muse/ports.py --list` 列出当前所有板子。
>
> ⚠️ Windows 上若同时接了多块板，请传 MAC 序列号而**不要传 `COMx`**——
> 脚本对 `COM` 形式的参数不会做端口识别。

**方式 B：直接用 idf.py**

```sh
cd esp32
idf.py -B build-muse-waveshare-s3-touch-lcd-185b -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-waveshare-s3-touch-lcd-185b/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-touch-lcd-185b" \
  -p COM5 flash monitor
```

`monitor` 会打开串口日志，退出按 `Ctrl + ]`。

**如果连不上芯片**：按住 **BOOT** 键 → 重新插拔 USB（或按一下 **PWR** 开机）→ 松开 **BOOT**，
设备即进入下载模式，再重试刷写。

> 重新刷写会**保留**配对信息与 Wi-Fi 凭据。想完全从头开始，先执行 `erase-flash`。

---

### 第 6 步：在手机 Muse App 中配对

这块板**没有状态指示灯**，所有状态都显示在圆形屏幕上。

1. 打开手机 **Muse App** → **Settings（设置）> Devices（设备）**，打开 **Developer mode（开发者模式）**。
2. 点击右上角 **+** → **Add Device**。
3. 设备名会以 `MuseGadget-XXXXXX` 开头，选中它。
4. 屏幕提示需要确认时，**按一下板子上的 BOOT 键**确认。
5. 配对成功后设备会加入你的 Wi-Fi 并连上 Muse。

**状态对照表**（屏幕显示）：

| 状态 | 含义 |
|---|---|
| 呼吸橙色 | 已就绪，等待配置 |
| 呼吸蓝色 | 请按 BOOT 键确认配对 |
| 蓝色 | 正在加入 Wi-Fi 并连接 Muse |
| 绿色 | 已连接 |
| 黄色闪烁 | 正在重连 |
| 紫色 | 尚未配对 |
| 红色闪烁 | 出错了，去看串口日志 |

**重新配对**：长按 BOOT 键 **5 秒**即可重置设备并重新配置。

> 每次配置都会建立一条全新的加密会话。由于这是社区设备，
> **配对没有厂商验证，无法防御主动中间人攻击**——请在可信的网络上配置。

---

## 5. 日常使用

| 操作 | 方式 |
|---|---|
| **按住说话（PTT）** | 按住 **BOOT** 键说话，松开结束 |
| **确认配对** | 按一下 **BOOT** 键 |
| **菜单 / 设置** | 在**触摸屏**上操作（从 Muse 主界面左滑进入设置） |
| **息屏 / 亮屏** | 由固件根据电源状态与电量自动管理 |
| **关机** | 菜单里选择关机 → 进入 **deep sleep**；按 **BOOT** 键唤醒 |
| **硬件断电** | 按 **PWR** 键（约 2 秒），这是纯硬件电源开关，固件读不到它 |

**关于语音交互的一个重要设计**：

本固件**没有常开语音检测（no always-on VAD），也不支持全双工**。
Muse **只在你按住 BOOT 键时才录音**，所以扬声器的声音永远不会像
「常开监听」类固件那样把录音提前打断。

如果**在 Muse 还在说话时**按下 BOOT（打断式插话 / barge-in），
固件会先丢弃 `CONFIG_MUSE_BARGE_IN_SETTLE_MS`（**默认 200 ms**）的采集音频，
这段正是扬声器自己的尾音。如果发现插话录到的开头是 Muse 自己的声音，
就在 `menuconfig` 的 **Muse** 菜单下把这个值调大；如果发现开头被削掉了，就调小。
设为 `0` 表示不丢弃。

**电池与功耗**：电量由 BQ27220 电量计读出。屏幕关闭时，
CPU 会降频并在按键轮询之间进入 light sleep；插着 USB 时芯片保持全速运行。

---

## 6. 命令速查

所有命令都从 `esp32/` 目录、在已加载 ESP-IDF 环境的终端中执行。

| 目的 | 命令 |
|---|---|
| 配置（填写 Token、调 barge-in 等） | `idf.py menuconfig` |
| 构建 | `tools/muse/board.sh build lcd185b` |
| 刷写 | `tools/muse/board.sh flash lcd185b COM5` |
| 刷写 + 看日志 | `idf.py -B build-muse-waveshare-s3-touch-lcd-185b -p COM5 flash monitor` |
| 备份原厂固件 | `python -m esptool --chip esp32s3 -p COM5 read-flash 0 0x1000000 backup.bin` |
| 擦除 Flash | `python -m esptool --chip esp32s3 -p COM5 erase-flash` |
| 列出已接入的板子 | `python tools/muse/ports.py --list` |
| 跑主机端单元测试 | `python -m unittest discover -s tests -p 'test_*.py'` |
| 更换屏幕头像 | `python tools/muse/avatar.py` |

---

## 7. 故障排查

| 现象 | 排查方向 |
|---|---|
| 构建报错 `No SDK token` / Token 非法 | Token 必须为空（仅警告）或严格为 `mgst_` + 43 个 base64url 字符 |
| 构建报错 `Cannot establish a connection to the component registry`（URL 指向 `components-file.espressif.com`） | 国内网络访问不了 CloudFront 文件源。设置 `IDF_COMPONENT_STORAGE_URL=https://components-file.espressif.cn/` 后重新构建，详见[第 3 步的国内加速说明](#32-构建) |
| 构建报错 TCP buffer / cJSON nesting | 生成的 `sdkconfig` 过期，删除整个构建目录后重新构建 |
| `esptool` 找不到串口 | 换一根**能传数据**的 USB 线；检查设备管理器里的 COM 口；按住 BOOT 再重插 USB 进入下载模式 |
| 刷完起不来 / 反复重启 | 首次刷入前**没有执行 `erase-flash`**，执行后重刷 |
| 屏幕有竖条纹 / 边缘色带 | ST77916 面板初始化表选错。参照 `board_waveshare_s3_touch_lcd_185b.c` 顶部的说明，通过 `st77916_vendor_config_t.init_cmds` 传入自定义初始化表 |
| 屏幕红蓝反了 | 把板级文件里 `rgb_ele_order` 从 `LCD_RGB_ELEMENT_ORDER_RGB` 改成 `BGR` |
| 触摸无反应 | overlay 已设 `CONFIG_ESP_LCD_TOUCH_CST816S_DISABLE_READ_ID=y`（部分 CST816S 变体读 ID 会失败）；若仍无反应，检查触摸排线与 `TP_RST=GPIO1 / TP_INT=GPIO4` |
| 一直显示「紫色（未配对）」 | SDK Token 未填或已失效，去 gadgets.muse.ai 重新签发并重新构建 |
| 日志里报 `BQ27220 not answering` | 未接电池时属正常现象；接了电池仍报错则检查 I2C |
| 连不上 Wi-Fi | 确认路由器是 **2.4 GHz**（ESP32-S3 不支持 5 GHz） |

---

## 8. 已知限制与注意事项

- **无常开语音唤醒**：必须按住 BOOT 说话，不支持「小爱同学式」的随时唤醒。
- **无全双工**：Muse 说话时无法同时录音（有 barge-in 机制，见第 5 节）。
- **PWR 键固件不可读**：菜单关机走的是 deep sleep，靠 BOOT 唤醒。
- **IMU / RTC 未接入**：QMI8658 六轴传感器与板载 RTC 当前固件尚未使用。
- **电量计充电状态未解码**：只上报电量百分比与电压，`charging` 恒为 `false`（USB 在线状态可读）。
- **无板载状态指示灯**：状态信息全部通过圆屏呈现。
- **安全建议**：强烈建议开启 NVS 加密——NVS 里存着 Wi-Fi 凭据和设备 Token，
  不加密时任何拿到板子实物的人都能读出来。在 `idf.py menuconfig` 的
  **ESP32 Device SDK** 下打开 `CONFIG_HOMEHUB_NVS_ENCRYPTION` 即可，
  密钥由 eFuse 中的 HMAC 密钥派生，首次启动时自动生成。
- **SDK Token 会被编进固件**：请把它当作**标识符**而非密码。一旦泄露，
  到 gadgets.muse.ai 吊销并重新签发，然后重新构建。
- **OTA 更新默认开启**（全 UI 板卡），版本号为 `999.0.0`；可用
  `idf.py -DPROJECT_VER=1.0.0 build` 指定版本。
- **构建使用仓库内的开发密钥签名，不启用 Secure Boot**，因此可以随意重复刷写。

---

## 9. 恢复微雪原厂固件

如需回退，把第 4 步的备份写回即可：

```sh
python -m esptool --chip esp32s3 -p COM5 write-flash 0 esp32-s3-touch-lcd-1.85b-backup.bin
```

---

## 10. 进阶：改固件、换头像、加指令

- **换成你自己的头像**：插上板子后运行 `python tools/muse/avatar.py`。
  它会请你的 Muse 把头像重绘为该板子的像素版本、校验结果，然后自动构建并刷入。
  自定义头像不会进 Git。详见
  [`tools/muse/AVATAR_RECIPE.md`](esp32/tools/muse/AVATAR_RECIPE.md)。
- **给 Muse 加一条自定义指令**（例如读传感器、开关继电器）：
  在 `link.register` 里声明，并在 `esp32/main/app.c` 中处理。
  详见 [`esp32/AGENTS.md`](esp32/AGENTS.md)。
- **不想接板子就调 UI**：用
  [`esp32/simulator/`](esp32/simulator/README.md) 的桌面预览，
  它在 412 × 412 的 SenseCAP Watcher 窗口中运行**生产环境的 UI 与头像渲染器**，
  支持鼠标键盘输入，也能在无显示服务器的情况下渲染脚本化截图。
- **让 Muse 说中文**：本板 overlay 已启用 `CONFIG_MUSE_CJK_FONT=y`，
  字幕会回退到 GNU Unifont 16×16 位图来显示中日文字（约占 850 KB Flash，不占 RAM）。
- **想自己移植新板子**：见 [`esp32/devices/README.md`](esp32/devices/README.md#add-a-board)
  与 [`esp32/AGENTS.md`](esp32/AGENTS.md)。
- **功耗分析**：起点是 [`esp32/docs/power/README.md`](esp32/docs/power/README.md)，
  使用 PPK2 前请先读 [`ppk2.md`](esp32/docs/power/ppk2.md)。

**给 AI 编码助手用的说明**：每个目录下的 `AGENTS.md` 是给
[Muse Code](https://developer.meta.com/ai/lp/muse-code/) 这类编码代理读的，
里面包含了完整的环境搭建、构建、刷写与读日志流程。

---

## 11. 许可证

本项目采用 **Apache License 2.0**（见 [`LICENSE`](LICENSE)），但以下第三方文件保留其上游许可证：

| 路径 | 上游 | 许可证 |
|---|---|---|
| `esp32/components/minimp3/include/minimp3.h` | [lieff/minimp3](https://github.com/lieff/minimp3) | CC0-1.0 |
| `esp32/main/pixel_font.c` | Adafruit GFX `glcdfont.c` | BSD-2-Clause |

构建时下载的依赖（ESP-IDF 组件、LVGL、SDL 等）遵循各自的许可证。
Apache 许可证**不覆盖** [Jollybot 头像](esp32/avatar)。

**社区**：欢迎加入 [Discord](https://discord.gg/3bhjCkZdd6) 与其他玩家交流。

---

## 附：本次移植的验证状态

本仓库的 1.85B 移植已通过**真实编译验证**（ESP-IDF v6.0.1，xtensa-esp32s3-elf-gcc 15.2.0）：

| 项目 | 结果 |
|---|---|
| 编译 | ✅ 通过，无错误无警告 |
| 固件体积 | `muse-gadget.bin` = **2.88 MB**，4 MB 应用分区剩余 **28%** |
| 生效板卡 | `CONFIG_MUSE_BOARD_ID="waveshare_s3_touch_lcd_185b"` |
| 内存配置 | `CONFIG_SPIRAM=y` + `SPIRAM_MODE_OCT` + 80 MHz（8 MB Octal PSRAM 已启用） |
| 分区表 | `partitions_muse.csv`（16 MB 双 OTA 布局，`otadata`@0x1d000） |
| 中文字体 | `CONFIG_MUSE_CJK_FONT=y` |
| 依赖解析 | `esp_lcd_st77916` 2.0.2、`esp_lcd_touch_cst816s` 1.1.2、`esp_lvgl_adapter` 0.6.4、`esp_codec_dev` ✅ |
| 刷入真机 | ⏳ **未进行**（需要你的板子 + SDK Token） |

### 验证中发现并已修复的问题

**`board_waveshare_s3_touch_lcd_185b.c` 编译不过**：BQ27220 电量计的读寄存器代码用了
**旧版 I2C 主站 API 的参数形式**，把总线句柄当成设备句柄、把地址当成写缓冲区传了进去，
在 ESP-IDF v6 上直接编译失败：

```
error: passing argument 1 of 'i2c_master_transmit_receive' from incompatible pointer type
error: too many arguments to function 'i2c_master_transmit_receive'; expected 6, have 7
```

已按 IDF 新 API 修正：在 `init()` 中先用 `i2c_master_bus_add_device()` 把 BQ27220 注册为
该总线上的一个从设备（句柄 `s_bq`），再由 `i2c_master_transmit_receive(s_bq, ...)` 读取。

### 仍待真机验证的部分（编译无法覆盖）

1. **ST77916 面板初始化表**：组件会探测寄存器 `0x04` 在两张表之间二选一，某些面板会选错，
   出现竖条纹 / 边缘色带。若出现，按板级文件顶部说明传入自定义 `init_cmds`。
2. **RGB / BGR 顺序**：首启若红蓝颠倒，把 `rgb_ele_order` 改成 `LCD_RGB_ELEMENT_ORDER_BGR`。
3. **触摸方向与阈值**：CST816S 的 `swap_xy` / `mirror_x` / `mirror_y` 可能需要按实机微调。
4. **绘制缓冲放在 PSRAM**：本板是全仓库唯一把 LVGL 绘制缓冲放进 PSRAM 的板子
   （2 × 360×32×2 B = 46 KB 位于 PSRAM）。这是刻意的内存优化，但需在 Wi-Fi/BLE 起来之后
   确认刷新无异常；若出现花屏，可改为 `use_psram = false` 并把 `DRAW_BUF_LINES` 降到 16。
5. **barge-in 时长**：默认 200 ms，按第 5 节的说明在实机上微调。
6. **电量计充电状态**：当前 `charging` 恒为 `false`（未解码 BQ27220 充电状态寄存器）。

