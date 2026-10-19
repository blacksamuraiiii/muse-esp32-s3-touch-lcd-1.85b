# Muse Gadgets 中文操作指南（本仓库主 README）
### 目标硬件：微雪 Waveshare ESP32-S3-Touch-LCD-1.85B

> 本文件是本仓库的主 README（中文），由原 `README_zh.md` 更名而来；上游英文原文见
> [facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)，
> 板级文档见 [`esp32/README.md`](esp32/README.md) 与 `esp32/AGENTS.md`。
> 内容以**面向 Windows 用户**的分步操作为主（上游文档只写了 macOS / Linux）。
>
> 当前版本 **1.3**（`esp32/version.txt`），发布件与 OTA 清单见
> [Releases](https://github.com/blacksamuraiiii/muse-esp32-s3-touch-lcd-1.85b/releases)。
>
> **风险提示**：本项目由「黑客为黑客」而写，纯属好玩。刷入自定义固件可能导致开发板变砖、
> 保修失效。一切后果自负。请务必先完成[第 4 步：备份原厂固件](#第-4-步备份原厂固件)。

---

## 目录

- [更新记录（v1.0 → v1.3）](#更新记录)

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
11. [本次移植踩过的坑（屏幕 / 构建环境）](#11-本次移植踩过的坑屏幕--构建环境)
12. [TTS 服务端：Azure Speech 注册 + Docker 搭建](#12-tts-服务端azure-speech-注册--docker-搭建)
13. [许可证](#13-许可证)

---

## 更新记录

### v1.3（2026-10-19）

真机（电池态）验证 T0–T4 全通过：自动熄屏能保持、敲击亮屏、按住亮屏、按键亮屏、亮屏后触摸 UI 正常。

- 修复 **电池下敲击亮屏**。四条接缝叠加，每条都单独实测过：
  ① 唤醒线极性要与器件有效电平一致——CST816S 的 INT 低有效，此前按高电平武装，IDF 直接拒绝 light sleep；
  ② 只武装唤醒源不够，**该脚中断必须使能**——LVGL 适配器在暂停触摸输入时会把这一脚的中断关掉，于是敲击一次也唤不醒；
  ③ 借来的中断要「使能进、使能出」并恢复驱动原来的下降沿触发——关着不恢复会让**触摸输入在第一次休眠后彻底失效**；
  ④ 控制器保持型 IRQ 要在睡前读走（drain）、只认「睡前该线为高」的下降沿，且 `last_slept_ms≈0` 的返回是「睡眠被拒绝」不是唤醒——
  熄屏瞬间控制器会自己抖一个脉冲，否则屏幕每 30 秒亮一次、永远睡不着。
  另按 CST816S 手册把 `0xED IrqPluseWidth` 由默认 `0x01` 放宽到 `0x14`，让短脉冲能被低功耗侧采样到。
- 修复 **1.2 引入的按键唤醒回归**。根因不在按键：电池态首次真睡/醒后 I2C 总线卡死（现场实测 `probe device timeout`
  每 2 秒、`BQ27220 not answering`、IMU 每次读失败），LVGL 任务在触摸读上等 IDF 的无限超时 → UI 冻住 → 按键看似无效。
  新增总线自愈：连续 2 次失败即 `i2c_master_bus_reset()` 并重设上拉（10 秒节流），失败日志带上 SDA/SCL 电平；`>i2c` 查询、`>i2c.reset` 强制。
- 修复 **电量恒显 33%**：电量计的电池模型被重置（`DesignCapacity`/`FullChargeCapacity` 都读 3000 mAh，而实际电池是 500 mAh），
  SoC 一直按错误容量算。现改用官方组件 `espressif/bq27220`（含 `espressif/i2c_bus`）：开机时把这块电池的 CEDV 模型写进电量计的数据内存
  （幂等，写入后回读校验；实测 `DesignCapacity`/`FCC` 均 500 mAh、`SoC 100%` 与 4159 mV 自洽）。电量百分比**以电量计为准**，
  电压折算曲线仅保留为电量计读数不可用时的最后兜底（不再用它去推翻电量计）。
- 新增诊断：`>tp`（触摸线电平/武装状态/drain 结果）、`>i2c`，`@imu.wakes` 增加 `touch_armed`，RTC 日记加 `TAPH` 魔数与触摸/按键位。
- 界面：把**麦克风图标**从屏幕底部中间挪到右上贴边（BOOT 键旁），并**新增电源图标**（`LV_SYMBOL_POWER`）放在右下贴边（PWR 键旁）——两个图标用来指示两个物理按键的位置。
  `muse_button_hint_t` 增加可选 `scale`（0=不缩放），电源图标按 192/256 绘制；`align_on_bar()` 为 `LV_ALIGN_TOP_RIGHT` 补分支，菜单底部提示仍留在中间（其它板不受影响）。
- 结论订正：这块板上「敲屏幕亮屏」来自**触摸控制器**（GPIO4），不是 IMU 敲击引擎；1.2 里的 `>knock` 软件敲击检测本轮未启用。
- 按两轮代码审查逐条修复（P0/P1）：
  **面板休眠**改为发原始 QSPI 命令 `0x10 SLPIN` / `0x11 SLPOUT` 并等 120 ms——`esp_lcd_st77916` 2.0.2 只注册了 `del/reset/init/draw_bitmap/disp_on_off`，
  没有 `base.disp_sleep`，原先的 `esp_lcd_panel_disp_sleep()` 是空操作，息屏后面板电荷泵仍在跑（与官方 1.75C 同一坑，它也是这么绕的）；
  关机路径同时补 **PA_EN 拉低**与 **BOOT 唤醒脚 RTC 上拉**（ext0 走 RTC 域，数字域上拉已掉）。
  **局部刷新**补偶数对齐回调（`LV_EVENT_INVALIDATE_AREA`）：ST77916 的 QSPI 每拍两个像素，奇数 x 起始的窗口会整行偏移一像素。
  **上电时按住的 BOOT** 不再算作一次按下（原 `pressed` 初值为 false，刷机时按住会被判成长按，5 秒即走到「重置配对」）。
  **显示链路等待全部加超时**：`wait_band` 上限 5 秒、chunk 信号量 500 ms（原来是 `while` 死等与 `portMAX_DELAY`，一次 DMA 回调丢失即永久冻屏）。
  **IMU 加速度平方改 `int64_t`**：±2 g 满量程三轴可达 3×32767²，溢出 INT32 后最强的一下反被算成 0。
  **息屏暂停等待 1 秒 → 10 秒**：三条唤醒线都是中断，1 秒轮询在默认配置下没有服务对象（新增板级 `wake_lines_reliable`）。
  **屏 QSPI 时钟 40 → 80 MHz**（对齐厂商 BSP）。
- 已知限制：电量计 CEDV 模型用的是参考电芯参数、未做 GPCCEDV 标定（容量项已按本板 500 mAh 写回）；
  I2C 自愈尚未在真实故障上复现验证；上述 P0 级修复已完成代码修改，待电池态复测。

### v1.2（2026-10-18）

固件：

- 修复 **敲击唤醒在电池下失效**。根因三条：I2C 事务被 light sleep 截断、`STATUS1` 锁存没人清、
  引擎配置完好但不检测。对策：每个 I2C 事务持 `ESP_PM_NO_LIGHT_SLEEP` 锁；亮屏也读 `STATUS1`；
  每秒比对 `CTRL7/CTRL8`、每分钟比对 `CAL1..CAL4` 并自愈重配；睡着时每 60 s 重臂一次。
  另在 `muse_imu_tap_init()` 里先软复位 QMI8658A（`0x60=0xB0`，等 20 ms），敲击阈值降到 0.4 g²，
  并把每次敲击的加速度脉冲写进日志（`imu bump`）。
- 修复 **静态 IP 在 OTA 后丢失**。地址改存 NVS（`net_ip` / `net_mask` / `net_gw` / `net_dns`），
  发布件里 `CONFIG_HOMEHUB_STATIC_*` 保持为空，设备升级后仍用自己的地址；两者都没有才退回 DHCP。
  新增控制台命令 `>net`（打印实际地址、DHCP 状态与来源）、`>net.set ip=… mask=… gw=… [dns=…]`、
  `>net.clear`（交回 DHCP）。地址丢失时表现为「Wi-Fi 已连、只有连 Muse 失败」：Muse 的
  `api.muse.ai` 在直连路线上不通，静态网关指向旁路由时正常。
- 修复 **屏幕长亮约 20 分钟后冻住**：LVGL 等 band 发送完成用的是空转自旋，会把发送任务饿死；
  改成二值信号量 + `lv_display_set_flush_wait_cb()`（上游同款修复）。
- 修复 **电量恒显 100%**：读 BQ27220 `StateOfCharge` 的寄存器是 `0x2C`（此前误读 `0x1C`）；
  另加电压折算兜底与 `>batt` 原始寄存器转储。
- 改进 **TTS 首音时延**：服务端换 waitress + Azure 连接池与启动预热，首音 1.2–1.8 s → 0.47–0.58 s；
  设备端加 MP3 载荷闸门（只放行 `0xFF` 或 `ID3` 开头），请求补 `User-Agent`。
- 改进 **OTA 健壮性**：下载任务栈从内部 RAM 逐级降档申请，manifest 检查任务改用 PSRAM 栈；
  新镜像不再因云端控制 WS 未连上被回滚；宽限路径只在镜像确为 OTA 安装时才标记有效。
- 头像换成自绘的 **毛毛**（64×64 程序化像素稿：公主切刘海、两侧上翘发束；V8 起头发改为从参考照片抠出的 64×64 点阵，无描边、无眉、无睫毛线）。
- 新增调试命令：`>imu` / `>imu.check` / `>imu.rearm`（tap 寄存器与自愈计数）、
  `>pmlow=0|1`（带 USB 也强制进 light sleep，用来复现电池态）。

服务端（不在固件里）：

- `tts-server/` 固定 waitress；Azure Speech（eastasia）加连接池与预热；`deploy.py` 一键部署。

### v1.1（2026-10-17）

固件：

- 新增 **设置页 Update**：拉 GitHub `releases/latest/download/latest.json`，数值比较版本后交给既有
  OTA 流程（镜像签名校验、重启自动）；不再广告 `device.ota` 命令（云端曾推来别的芯片的固件）。
- 新增 **敲击唤醒**：QMI8658A tap 引擎（加速度 500 Hz，INT1 → GPIO8，经 R51 0R）；睡着时靠
  GPIO 电平唤醒，另每 200 ms 读 STATUS1 兜底。
  ⚠️ **已知问题**：长时间睡眠（约 2 分钟以上）后敲击失效；按键点亮→30 秒自动息屏后再敲则正常。
  留待下次修。
- 改进 **音量按钮位置**：`y = -ring_in/3`（原 `-5/8·ring_in`）。距 Muse 净空 15 px → 33 px，
  与圆环内缘仍留 5 px；回复页字格数不变（17×3 / 14×7）。
- 改进 **TTS 载荷校验**：前 8 字节须是 `0xFF`（MPEG 同步字）或 `ID3`，否则整段丢弃、只留字幕；
  请求带 `User-Agent`。
- 首次升级：版本门禁是**数值比较**，跑 1.0 的板子必须先用 USB 刷一次 v1.1，之后才能在设置页 OTA。

服务端（不在固件里）：

- TTS 从 edge-tts 切到 **Azure Speech（eastasia）**，默认音色晓伊（Xiaoyi）；`tts-server/` 含
  Dockerfile 与 `deploy.py`（paramiko 一键部署）。
- **waitress** 替代 Flask 开发服务器——后者会把给板子的长响应截断在约 1440 字节（只出字幕不出声）。

### v1.0（2026-10-16）

- 首次在本板（Waveshare ESP32-S3-Touch-LCD-1.85B）跑通 Muse：显示（ST77916 读寄存器自动选表）、
  电容触摸、音频编解码、Wi-Fi 与配对、头像与 UI、语音播报（TTS）链路。

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

#### 3.3 预置 Wi-Fi 与静态 IP（可选）

如果不想每次刷完都走蓝牙配网，可以把 Wi-Fi 凭据直接编进固件。在
`idf.py menuconfig` 的 **Home Hub** 菜单里填（或直接改构建目录下的 `sdkconfig`）：

| 配置项 | 作用 |
|---|---|
| `CONFIG_HOMEHUB_WIFI_SSID` | 非空即启用覆盖：开机直接连这个网络，不等蓝牙配网 |
| `CONFIG_HOMEHUB_WIFI_PASSWORD` | 上面那个网络的密码 |
| `CONFIG_HOMEHUB_STATIC_IP` | 静态 IPv4 地址；留空则走 DHCP |
| `CONFIG_HOMEHUB_STATIC_NETMASK` | 子网掩码 |
| `CONFIG_HOMEHUB_STATIC_GATEWAY` | 网关 |
| `CONFIG_HOMEHUB_STATIC_DNS` | 主 DNS（旁路由场景填旁路由地址） |

静态地址是**新增支持**（`main/wifi_mgr.c` 的 `apply_static_ip()`）：在
`WIFI_EVENT_STA_START` 里停掉 DHCP 客户端再套用地址——这个时机是刻意的，IDF 的
Wi-Fi glue 注册的 `STA_START` 处理函数在前，此时 netif 和它的 DHCP 客户端都已经起来，
正是 `esp_netif_dhcpc_stop()` 需要的状态；之后也不会有别的路径把 DHCP 重新拉起来。
地址解析失败会自动退回 DHCP，不会把设备卡死在没网的状态。

实测（`backups/boot-log-lcd185b-2026-10-15-wifi-online.txt`）：

```
I (1307) link.wifi: static IP 192.168.1.50 mask 255.255.255.0 gw 192.168.1.1 dns 192.168.1.1
I (3513) link.app: wifi override active, connecting to MyHome_5G
I (3678) link.wifi: got IP 192.168.1.50
...
I (10926) link.noise_ctrl: TLS connected to hatch.metaaivm.com:443
I (12397) link.noise_ctrl: handshake msg3 sent (64 bytes), session established
I (14373) link.noise_tun: tunnel stream open (stream 2)
I (18901) link.heartbeat: hb t=17s setup=done wifi=up ws=up raw=up ble=off
```

> ⚠️ 把密码写进 `sdkconfig` 就等于写进固件镜像，任何人拿到板子都能读出来。
> 仅适合自己调试用的板子。

#### 3.4 让 Muse 说话（语音播报，可选）

**Muse SDK 默认不会出声。** 这不是故障：它是"瘦客户端"设计，录音上传云端转文字，
回复只回文字（`voice.h` / `muse_chat.h` 都写明了），官方留了一个 TTS 接口让你自己接。
所以默认行为是**用静音按阅读速度走字幕**——你看到字、听不到声，属正常。

本仓库补上了这条链路（`esp32/components/muse/muse_tts.c`）。设备侧只做一件事：
把回复文本填进一个 URL 模板、GET 回 MP3、交给 SDK 自带的 minimp3 + 重采样器播放。
换个语音服务＝**改一行 URL**，不用碰固件：

```
CONFIG_MUSE_TTS_URL="https://fanyi.baidu.com/gettts?lan=zh&text={text}&spd=5&source=web"
CONFIG_MUSE_TTS_URL="http://192.168.1.1:5050/tts?voice=zh-CN-XiaoxiaoNeural&text={text}"
```

留空则回到"只显示字幕"的老行为。长回复会按标点切成 180 字节分多次请求，
所以 URL 长度可控、播放也能早于全部合成完成就开始。

**为什么默认用百度而不是 Edge TTS**：Edge TTS（`speech.platform.bing.com` 的
readaloud 端点）现在的 `Sec-MS-GEC` 令牌已经是 **v2**（`<16位CS64>-<64位SHA256>`，
内含一套刻意混淆的自定义 SHA-256），要在设备上移植约 200 行密集位运算，算错即 403，
且微软随 Chromium 版本推进会改。百度翻译的朗读端点免令牌、免 key、直连可用、
返回标准 MP3，是更稳的默认值。

想用 Edge TTS 的音色，在局域网任意一台常开的 Linux 上跑
[`edge-tts`](https://github.com/rany2/edge-tts) 包一个小 HTTP 服务（收到 `text` 返回
MP3），再把上面第二行填进去即可——令牌算法由上游维护，设备端不用管。

> **注意**：目前只有**语音回合**（按住按钮说话）会朗读回复。串口/App 里**打字**的回合
> 是"只显示文字"的设计（`muse_chat_session.cpp` 的 `message_done()` 在文字分支提前返回），
> 所以**没法用串口命令触发一次朗读来做验证**，只能按住按钮说话。

> ✅ **实现状态：2026-10-17 已切到 Azure Speech，真机出声。** 服务端从 edge-tts 换成
> **Azure Speech（eastasia）**，音色默认 **晓伊 zh-CN-XiaoyiNeural**（`TTS_VOICE`，可用
> `TTS_FORCE_VOICE` 覆盖板子 URL 里带的音色，改音色不必重刷固件）；并叠加
> `mstts:express-as style='friendly' styledegree='1.2'` 与 `pitch=+4%`。
>
> 同一次换血里踩到一个**服务端坑**：Flask 自带的开发服务器（werkzeug）会把给板子的长响应
> **截断在约 1440 字节**，板子串口表现为
> `E HTTP_CLIENT: Incomlete data received, ret=0, 1440/51264 bytes` → 只显示字幕、没有声音。
> 换成 **waitress** 一次通过（镜像 `muse-tts:1.4` 起；`TTS_SERVER=werkzeug` 只留给本地 curl）。
> 排查手法：容器日志显示服务端发满 51264 字节 → 同一 URL 从 WSL 取亦是完整 → 用**裸 socket 探针**
> 占同一端口回放同一份载荷（绕开 werkzeug），板子完整播出 → 责任锁定在 HTTP 服务器层。
>
> 2026-10-16 打通时的两个真因（仍记在案）：
>
> | 现象 | 根因 | 修法 |
> |---|---|---|
> | 只出字幕、`muse.tts: no task for TTS` | 内部 RAM 最大连续块只剩 9 KB，而 TTS 任务要 12 KB 连续栈 → `xTaskCreate` 失败，回合退回静音 | TTS 任务栈与 16 KB 环形缓冲都改从 PSRAM 分配（`xTaskCreateWithCaps` / `xStreamBufferCreateWithCaps`）。任务改为常驻 worker，不再自我删除 |
> | 回复根本不入队 | `SUB_LINE_MAX` 只有 256 KB，长 `message_done` 事件整行被丢 | `muse_chat_session.cpp:94` → 1 MB |
>
> 成功时的串口标志（可作以后回归的验收行）：
>
> ```
> I muse_chat_session: speaking message assistant-msg-… (N chars)
> I muse_chat_session: reply audio: 24000 Hz, 1 ch, 48 kbps
> I muse.tts: TTS got NNNNN bytes
> I muse_voice: muse reply: X.XXs of audio, N.NNs total
> ```
>
> 另注：edge-tts 固定回 24 kHz MP3，固件自带重采样器会降到 codec 的 16 kHz，服务端不用管采样率。
> 端到端首音约 6–9 秒，其中 **Muse 服务端吐文本占 4–7.6 秒**（改不了），本机合成约 1.3–3 秒。
> 服务端已改流式返回（`Transfer-Encoding: chunked`），首字节由 3.15 s 降到 **1.29 s**；
> 串口 `muse.tts` 行会打 `first byte N ms` 便于回归对比。换后端仍只需改 `CONFIG_MUSE_TTS_URL` 一行；
> 局域网服务见 `tts-server/README.md`。

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

板子自己的控制台命令（串口连上后输入，前面带 `>`）：

| 命令 | 作用 |
|---|---|
| `>net` | 打印实际 IP/掩码/网关/DNS、DHCP 状态与地址来源（nvs / build / dhcp） |
| `>net.set ip=10.0.0.5 mask=255.255.255.0 gw=10.0.0.1 dns=10.0.0.1` | 写入 NVS 并重启；发布件里不内置地址，靠它给板子定地址 |
| `>net.clear` | 删除 NVS 地址，重启后回到 DHCP |
| `>imu` / `>imu.check` / `>imu.rearm` | 打印 / 校验 / 重臂 QMI8658A tap 引擎 |
| `>batt` | BQ27220 标准寄存器全量转储 |
| `>pmlow=1` / `>pmlow=0` | 带 USB 也强制进 / 退出 light sleep（复现电池态用；进入后 USB 串口会断） |
| `>status` | 一行 JSON：版本、电量、Wi-Fi、Muse 会话状态 |

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
- **OTA 更新默认开启**（全 UI 板卡）；本分支版本号现为 `1.3`（`esp32/version.txt`）。
  ⚠️ 版本门禁认数值：上游占位版本是 `999.0.0`，`1.x` **不比**它新，所以**从原厂固件过来的第一次必须 USB 刷入**
  （或走一次 `force` 的 OTA），此后同一分支内的 `1.0` → `1.3` 才能正常自更新。
  也可用 `idf.py -DPROJECT_VER=1.3.1 build` 覆盖本次构建的版本。
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

## 11. 本次移植踩过的坑（屏幕 / 构建环境）

这一节把 2026-10-15 一天真机调试里踩到的坑集中记下来。前面各步散落着同样的内容，
这里是可以直接跳查的索引。

### 11.1 屏幕不亮 → PSRAM 绘制缓冲，改分带渲染

**现象**：刷完屏幕全黑。串口四条错误，只在第一次绘制时出现一次：

```
E spi_master: DMA TX underflow detected
E lcd_panel.io.spi: panel_io_spi_tx_param(221): recycle spi transactions failed
E st77916_spi: panel_st77916_draw_bitmap(485): send command failed
E esp_lvgl:bridge_v9: Draw bitmap failed: ESP_ERR_INVALID_STATE
```

**根因（有 IDF 源码级证据，不是猜的）**：ESP-IDF 的 SPI 驱动**不能通过 DMA 直接读 PSRAM**。

- `esp_lcd_panel_io_spi.c` 判断 `esp_ptr_external_ram(color)`，是 PSRAM 就给事务打上
  `SPI_TRANS_DMA_USE_PSRAM`；
- `esp_driver_spi/src/gpspi/spi_master.c` 里那句 `DMA TX underflow` 的检测**本身被
  `#if SOC_PSRAM_DMA_CAPABLE && CONFIG_SPIRAM` 门控**，且要求发送缓冲在外部 RAM
  —— 也就是说**这条错误只可能因缓冲在 PSRAM 而触发**；
- 失败的事务在下一次 `spi_device_get_trans_result()` 回收时报 `recycle spi transactions failed`。

即：`use_psram = true` 的 LVGL 缓冲经 DMA 发 QSPI 时饿死 → 第一次 flush 失败 →
屏幕从头到尾没拿到有效像素。

**修法**：改用本仓库既有的 **`muse_lcd_bands`（分带渲染）**——1.75C / 2.16 / Watcher /
Stopwatch 四块板都这么做，**1.85B 是唯一漏掉的**。LVGL 在 PSRAM 画整条 90 行的高带，
专用任务切成 8 行的块拷进两块固定的内部 DMA 缓冲交替发出。

加新板务必：`DRAW_BUF_LINES = LCD_V_RES / 4`、`LCD_CHUNK_BYTES = LCD_H_RES * 8 * 2`、
`max_transfer_sz = LCD_CHUNK_BYTES`，并在 `components/muse/CMakeLists.txt` 该板分支
**同时登记** `boards/<board>.c` 和 `boards/muse_lcd_bands.c`。
`muse_lcd_bands_register()` 之后，面板与面板 IO 的一切访问都要经 `muse_lcd_bands_run()`。

### 11.2 屏幕花屏横条纹 → 面板混装两个修订版

**现象**：绘制通路修好后屏幕点亮，但显示彩色横条纹。

**根因**：微雪这块板**出厂混装两个 ST77916 面板修订版**，靠**面板寄存器 0x04** 区分：

| 0x04 返回值 | 修订 | 需要的初始化表 |
|---|---|---|
| `00 7F 7F 7F` | version 1 | `vendor_specific_init_version_1`（182 条） |
| `00 02 7F 7F` | version 2 | `vendor_specific_init_version_2`（185 条） |

`espressif/esp_lcd_st77916` **2.x 只剩一张内置默认表**（`F0=0x08, F2=0x08` 开头），
**两张修订都对不上** → 无论拿到哪块板都是花屏。探测逻辑在**厂商 BSP**里，不在组件里：
`Examples/ESP-IDF-V5.5.3/*/components/waveshare__esp32_s3_touch_lcd_1_85B/esp32_s3_touch_lcd_1_85B.c`

**修法**（已内置在板级文件，照搬官方流程）：

1. 先手工拉一次面板复位（驱动的复位要等面板创建后，晚于探测）；
2. 以 **3 MHz** 建 panel IO，用读操作码 `0x0B` 读寄存器 `0x04`；
3. 按返回值选表；
4. `esp_lcd_panel_io_del()` 后以 **40 MHz** 重建 IO 交给绘制路径；
5. 经 `st77916_vendor_config_t.init_cmds` 传入。

**本机这块板是 v2**（启动日志会打印 `ST77916 reg 0x04 = 00 02 7F 7F ... -> init table v2`）。
换到另一块 1.85B（v1）**不用改任何代码**，探测会自动选对。

> 经验：这类"亮但花"的问题，**优先去厂商 BSP 拿权威初始化序列**，别对着组件默认表猜。
> 微雪的例程在 `Examples/ESP-IDF-V5.5.3/*/components/waveshare__<board>/`；
> 用 GitHub API 拉 `git/trees?recursive=1` 找文件最快（网页列表会漏文件名）。

### 11.3 构建环境坑（Windows / 本机）

#### （1）组件仓库走中国镜像

`components-file.espressif.com`（CloudFront）本机不可达，构建报
`Cannot establish a connection to the component registry`。构建前设：

```powershell
$env:IDF_COMPONENT_STORAGE_URL = "https://components-file.espressif.cn/"
```

#### （2）在 AI 代理里构建时，删除保护会打断 ESP-IDF

WorkBuddy（以及同类的代理工具）会通过 `PYTHONPATH` 给所有 Python 进程注入一个
`sitecustomize.py`，把删除操作改成"移入回收站"。ESP-IDF 的组件管理器清理未使用组件时
一次删掉整个目录，于是：

- `[safe-delete][SAFE_DELETE_BULK_CONFIRM_REQUIRED]` —— 单次删除超过阈值（默认 50）就拒绝；
- `[safe-delete][SAFE_DELETE_FAIL_CLOSED]` + `SHFileOperationW ... 0x2` —— 长路径进回收站失败后"失败关闭"。

**这两条都会让 CMake 以 FATAL_ERROR 终止，构建直接失败。**

#### （3）⚠️ 依赖锁会被写坏，形成死循环（本机反复踩到）

组件管理器的流程是「**先写 `dependencies.lock`，再清理未使用组件**」。清理被上面的保护
打断 → **锁就停在半截** → 下一次构建读到半截锁，认为其余组件都"未使用"要删 → 再次被拦。

判断方法：锁里 `dependencies:` **映射**条目应约 19 条（对应 19 个组件目录）。
**只剩 4 条就说明被截断了**，日志会反复出现 `Direct dependencies have changed` 和
`Deleting N unused components`。

**唯一的破解：两个一起清，再重新构建。**

```powershell
# 走 PowerShell 原生删除通道，不经 Python shim
Remove-Item -LiteralPath "esp32\managed_components" -Recurse -Force
Remove-Item -LiteralPath "esp32\dependencies.lock" -Force
# 然后正常 idf.py build
```

这正是仓库 `tools/muse/board.sh` 每次构建前做的 `clean()` —— 不同板卡的依赖集不同，
组件管理器本来就要清理上一套，所以**养成"先清再建"的习惯最省事**。
全量重建 2166 步、约 18～20 分钟（ccache 热）。

#### （4）构建耗时数字会被机器休眠污染

CMake configure 日志里的 `Configuring done (82001.7s)` 是**墙钟时间**，
机器休眠会被算进去，别据此判断环境异常。

### 11.4 小结：今天改了什么

| 文件 | 改动 |
|---|---|
| `components/muse/boards/board_waveshare_s3_touch_lcd_185b.c` | 分带渲染；内置官方两张初始化表 + 0x04 探测选表；背光日志；面板 IO 访问经 `muse_lcd_bands_run()` |
| `components/muse/CMakeLists.txt` | 185B 分支补登 `boards/muse_lcd_bands.c`；源列表加 `muse_tts.c` |
| `components/muse/muse_tts.{c,h}` | 新增：URL 模板 → MP3 的语音播报 |
| `components/muse/muse_chat_session.cpp` | `start_tts()` 发声分支、主循环 `pump_tts()`、`turn_finish()` 取消防护 |
| `components/muse/Kconfig` | 新增 `MUSE_TTS_URL` |
| `main/Kconfig.projbuild` | 新增 `HOMEHUB_STATIC_IP` / `_NETMASK` / `_GATEWAY` / `_DNS` |
| `main/wifi_mgr.c` | 新增 `apply_static_ip()`，挂在 `WIFI_EVENT_STA_START` |

---

## 12. TTS 服务端：Azure Speech 注册 + Docker 搭建

### 12.1 注册 Azure Speech（拿 key 与 region）

1. 打开 <https://portal.azure.com> 注册并登录（个人免费账号可用；F0 层不产生费用）。
2. 搜索 **Speech services（语音服务）** → **创建**：
   - **区域**选 **East Asia（`eastasia`）**——与本固件已验通的调用一致；
   - **定价层**选 **Free F0**：语音合成每月 50 万字符（以[定价页](https://azure.microsoft.com/zh-cn/pricing/details/speech/)为准），
     单板日常够用；超额会返回 429，届时在定价页升级到 S0。
3. 资源创建完成后进入该资源 → **密钥和终结点（Keys and Endpoint）**：
   - 复制 **KEY 1** → 这就是 `AZURE_SPEECH_KEY`；
   - 记下 **位置/区域**（如 `eastasia`）→ 这就是 `AZURE_SPEECH_REGION`。
4. 选音色（可选）：音色列表见
   <https://learn.microsoft.com/azure/ai-services/speech-service/language-support>。
   服务端默认 `zh-CN-XiaoxiaoNeural`，也可用 `zh-CN-XiaoyiNeural` 等；
   改音色**不必重编固件**（服务端 `TTS_FORCE_VOICE` 覆盖板子 URL 里的 `voice=`）。
5. 看用量：资源页的「指标（Metrics）」里可查当月合成字符数。

> 服务端封装的请求形态：`POST https://<region>.tts.speech.microsoft.com/cognitiveservices/v1`，
> 头带 `Ocp-Apim-Subscription-Key` 与 `X-Microsoft-OutputFormat: audio-24khz-48kbitrate-mono-mp3`
> （与固件验通的 `reply audio: 24000 Hz, 1 ch, 48 kbps` 一致），正文是 SSML。

### 12.2 用 Docker 跑 tts-server

```sh
cd tts-server
cat > .env <<'EOF'
AZURE_SPEECH_KEY=<KEY 1>
AZURE_SPEECH_REGION=eastasia
TTS_VOICE=zh-CN-XiaoyiNeural
EOF
docker build -t muse-tts:1.5 .
docker run -d --name muse-tts --restart unless-stopped -p 5050:5050 --env-file .env muse-tts:1.5
curl -s http://127.0.0.1:5050/health
curl -s -o /tmp/t.mp3 "http://127.0.0.1:5050/tts?text=你好&voice=zh-CN-XiaoyiNeural"
head -c 4 /tmp/t.mp3 | od -An -tx1     # ff f3 或 49 44 33 = MP3
```

- 一键部署到局域网主机（上传 → 写 `.env` → 建镜像 → 重启容器 → 主机自测）：
  `~/.venv/bin/python3 deploy.py <host>`。默认用 SSH key `~/.ssh/muse-16827_rsa`，
  没有 key 时才回退到环境变量 `MUSE_HOST_PASSWORD`（口令走环境变量，不进 `argv`）。
- 密钥只放宿主机 `/opt/muse-tts/.env`（权限 600）：**不进镜像、不进 `docker run -e`**，用 `--env-file` 传。
- 关键环境变量：

| 变量 | 默认 | 作用 |
|---|---|---|
| `AZURE_SPEECH_KEY` / `AZURE_SPEECH_REGION` | 空 / `eastasia` | Azure 凭据 |
| `TTS_BACKEND` | `azure` | 后端选择（`edge` 是回退路线，实测已 403，别指望） |
| `TTS_VOICE` / `TTS_FORCE_VOICE` | `zh-CN-XiaoxiaoNeural` / 空 | 默认音色 / 强制压掉板子 URL 里的音色 |
| `TTS_RATE` / `TTS_PITCH` | `+8%` / `+4%` | 语速、音高（Azure 用百分比；edge-tts 只认 `Hz`） |
| `TTS_AZURE_POOL` | `6` | Azure 连接池大小，复用 TLS 是首音 1.2–1.8 s → 0.47–0.58 s 的关键 |
| `TTS_SERVER` | `waitress` | **必须 waitress**：Flask 开发服务器会把给板子的长响应截断在约 1440 字节（只出字幕不出声） |
| `TTS_PORT` | `5050` | 监听端口 |
| `TTS_STREAM_HEAD` / `TTS_SEND_BYTES` | `512` / `512` | 首包阈值与分片字节数 |
| `TTS_MAX_CHARS` | `180` | 单次请求最大字符数（固件按标点分块） |
| `TTS_TEST_HOOKS` / `TTS_FORCE_BAD` | 关 | 坏包丢弃的测试钩子，验完关掉 |

- 板子侧只改一行 URL（写进构建目录的 `sdkconfig`，不入库）：

```
CONFIG_MUSE_TTS_URL="http://<tts-host>:5050/tts?voice=zh-CN-XiaoyiNeural&text={text}"
```

- 验收：`/health` 里 `azure_pool_idle=1`、`stream_head=512`；板子串口出现
  `muse_chat_session: reply audio: 24000 Hz, 1 ch, 48 kbps` 与 `muse.tts: TTS got N bytes`。
  排查见 [`tts-server/README.md`](tts-server/README.md) 与本文第 11 节。

---

## 13. 许可证

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
| 刷入真机 | ✅ **已完成**（2026-10-15，真机 MAC `44:BD:8D:xx:xx:xx`） |
| 原厂固件备份 | ✅ 16 MB 全片镜像，MD5 `7655af5517e5667b2c35b1298482c0fc` |
| SDK Token | ✅ 已写入并嵌入固件（真机日志 `link.app: SDK token: mgst_…`） |
| 真机启动 | ✅ 无 panic / 无重启循环，Wi-Fi 扫描、BLE 广播、心跳均正常 |
| 屏幕点亮 | ✅ 绘制通路已修复（分带渲染），串口零绘制错误；画面需肉眼确认 |

### 真机实测结果（2026-10-15）

**环境**：ESP-IDF v6.0.1，`idf.py build` + `esptool` erase/write，串口 COM3（USB-Serial/JTAG，921600 读 / 460800 写）。

**原厂固件备份**（刷写前）：

| 项 | 值 |
|---|---|
| 文件 | `backups/esp32-s3-touch-lcd-1.85b-backup.bin` |
| 大小 | 16,777,216 字节（0x1000000，整片） |
| MD5 | `7655af5517e5667b2c35b1298482c0fc` |
| 原厂工程名 | `01_factory`，构建于 2026-03-23，ESP-IDF v5.5.3 |
| 原厂分区 | `factory`@0x20000 (6.5 MB) + `ota_0`@0x6c0000 + `assets`@0xac0000 (4 MB 字体/图片资源) |

> 注意：Muse 分区表**没有 `factory` 分区**（只有 `ota_0` / `ota_1`），所以 `erase-flash` 之后原厂固件在片上就没了。上表这份备份是唯一的回退途径，别弄丢。

**Muse 固件刷入结果**：四个镜像全部写入并通过哈希校验。

```
bootloader.bin          22608 B  @0x0       Hash of data verified
partition-table.bin      3072 B  @0x10000   Hash of data verified
ota_data_initial.bin     8192 B  @0x1d000   Hash of data verified
muse-gadget.bin       3018752 B  @0x20000   Hash of data verified
```

**真机启动日志要点**：

- 引导器识别分区表为 Muse 双 OTA 布局（`ota_0`@0x20000、`ota_1`@0x420000、`prod_data`/`prod_bak`@0x820000）。
- Octal PSRAM 正常：`vendor id 0x0d (AP)`、`density 0x03 (64 Mbit)`、`Speed: 80MHz`，向堆加入 5287 KB。
- 板卡识别正确：`muse: board: Waveshare ESP32-S3-Touch-LCD-1.85B`。
- Token 生效：`link.app: SDK token: mgst_…`（对比：token 留空时会打印 `muse no token`）。
- 节点身份：`node_id=homelink-780c1c`、`ble_name=MuseGadget-780C1C`，处于未配对广播状态。
- 触摸控制器 CST816S 注册成功；Wi-Fi 扫描到 AP；`link.heartbeat` 每 5 s 一次，堆稳定在 int 70 K / psram 5165 K，无下降趋势。

**发现的问题：屏幕点不亮（已定位并修复）**

首刷之后屏幕无画面，串口日志里是这四条：

```
E spi_master: DMA TX underflow detected
E lcd_panel.io.spi: panel_io_spi_tx_param(221): recycle spi transactions failed
E st77916_spi: panel_st77916_draw_bitmap(485): send command failed
E esp_lvgl:bridge_v9: Draw bitmap failed: ESP_ERR_INVALID_STATE
```

根因**不是驱动型号选错**——微雪官方文档与板级文件逐条核对过：驱动 IC `ST77916`、
QSPI 接口、360×360、CS=21 / PCLK=40 / D0=46 / D1=45 / D2=42 / D3=41 / RST=3 / BL=5，
全部一致。

真正的原因是**绘制缓冲放在了 PSRAM**：ESP-IDF 的 SPI 驱动不能通过 DMA 直接读 PSRAM
（`esp_lcd_panel_io_spi` 检测到 `esp_ptr_external_ram(color)` 就打上 `SPI_TRANS_DMA_USE_PSRAM`），
ESP32-S3 上 DMA 从 PSRAM 取数供不上 40 MHz QSPI 的消耗，于是 `spi_master` 报
`DMA TX underflow`（这个检测本身就是 `#if SOC_PSRAM_DMA_CAPABLE && CONFIG_SPIRAM` 门控的，
只有 TX 缓冲在外部 RAM 时才可能触发）；下一条命令在
`spi_device_get_trans_result()` 回收事务时失败，即 `recycle spi transactions failed`，
LVGL 的第一次 flush 就此失败，屏幕始终没有有效像素。

修法沿用本仓库既有的 **`muse_lcd_bands`（分带渲染）**——1.75C、2.16、Watcher、Stopwatch
四块板都这么做的，1.85B 是唯一漏掉的：

- LVGL 仍把每个 90 行的整条带画在 PSRAM（360 ÷ 4 = 4 条带）；
- 专用发送任务把每带切成 8 行的块，拷进**两块固定的内部 DMA 缓冲**（各 5760 B）交替发出，
  交给 DMA 的永远是可访问的内部 RAM；
- `max_transfer_sz` 设为单块上限 `LCD_CHUNK_BYTES`。

**修复后实测**（`backups/boot-log-lcd185b-2026-10-15-bands-fix.txt`）：

| 指标 | 修复前 | 修复后 |
|---|---|---|
| `DMA TX underflow` | 1 | **0** |
| `recycle spi transactions failed` | 1 | **0** |
| `Draw bitmap failed` | 1 | **0** |
| panic / 重启 | 0 | 0 |

同时新增一行确认背光的日志（`board: backlight 100%`），说明 LEDC 确实把背光拉到了满亮。
堆余量：`int 46 K / dma 43 K / psram 4024 K`，无下降趋势。

**第二个问题：屏幕亮起后花屏条纹（已定位并修复）**

修好绘制通路后屏幕亮了，但显示的是彩色横条纹。原因**不在数据通路，而在面板初始化表**：
微雪这块板出厂时**混装两个 ST77916 面板修订版**，靠面板的**寄存器 0x04**区分
（QSPI 读操作码 `0x0B`）：

| 0x04 返回值 | 面板修订 | 需要的初始化表 |
|---|---|---|
| `00 7F 7F 7F` | version 1 | `vendor_specific_init_version_1`（182 条） |
| `00 02 7F 7F` | version 2 | `vendor_specific_init_version_2`（185 条） |

`espressif/esp_lcd_st77916` **2.x 只有一张内置默认表**（以 `F0=0x08, F2=0x08` 开头），
它**两张都对不上**——所以无论拿到哪个修订版都是花屏。（1.x 时代据称有探测逻辑，
2.0 起改由厂商 BSP 自己实现。板级文件顶部那句"探测 0x04 二选一"的注释说的正是这件事，
之前误以为已过时，实际它一直是对的。）

修法：把微雪官方 BSP
（`Examples/ESP-IDF-V5.5.3/01_comprehensive_example/components/waveshare__esp32_s3_touch_lcd_1_85B/esp32_s3_touch_lcd_1_85B.c`）
的两张表**原样搬进板级文件**（`s_init_version_1` / `s_init_version_2`），并复刻它的流程：

1. 先手工拉一次面板复位（驱动的复位要等面板创建后才会发生，晚于探测）；
2. 以 **3 MHz** 建 panel IO，读寄存器 `0x04`；
3. 按返回值选表；
4. `esp_lcd_panel_io_del()` 后以 **40 MHz** 重建 IO，交给绘制路径；
5. 把选中的表经 `st77916_vendor_config_t.init_cmds` 传入。

本机的实测结果（`backups/boot-log-lcd185b-2026-10-15-init-v2.txt`）：

```
I (1114) board: ST77916 reg 0x04 = 00 02 7F 7F (ESP_OK) -> init table v2
I (1132) st77916_spi: LCD panel create success, version: 2.0.2
I (1940) board: backlight 100%
```

即此板是 **version 2** 修订。绘制错误仍为 0（`DMA TX underflow` / `Draw bitmap failed` / panic 全 0）。

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

**`board_waveshare_s3_touch_lcd_185b.c` 显示点不亮**：本板是全仓库唯一把 LVGL 绘制缓冲
放进 PSRAM、且没有走 `muse_lcd_bands` 分带渲染的板子（2 × 360×32×2 B = 46 KB 在 PSRAM），
导致 SPI DMA 从 PSRAM 取数饿死（详见上一节的完整分析）。已改为分带渲染：

- `components/muse/boards/board_waveshare_s3_touch_lcd_185b.c`：
  `DRAW_BUF_LINES` 由 32 改为 `LCD_V_RES / 4`（= 90，四条带），新增
  `LCD_CHUNK_BYTES (LCD_H_RES * 8 * 2)`；`spi_bus_config_t.max_transfer_sz` 由整带大小
  改为 `LCD_CHUNK_BYTES`；`esp_lv_adapter_register_display()` 换成
  `muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES)`，
  并把 `profile` 里的 `buffer_height` / `use_psram` / `require_double_buffer` 交给分带模块自己设。
- 分带模块注册后，面板与其 IO 必须经它自己的任务访问，因此 `panel_sleep()` 与
  `power_off()` 里的 `esp_lcd_panel_disp_sleep()` / `esp_lcd_panel_disp_on_off()`
  都改由 `muse_lcd_bands_run()` 包一层（背光是 LEDC，不经面板 IO，无需包装）。
- `components/muse/CMakeLists.txt`：把 `boards/muse_lcd_bands.c` 加进
  `CONFIG_MUSE_BOARD_WAVESHARE_S3_TOUCH_LCD_185B` 分支的源列表（此前只有板级文件本身）。

**`board_waveshare_s3_touch_lcd_185b.c` 花屏条纹**：面板初始化表不匹配（组件 2.x 只剩一张
默认表，而这块板混装两个面板修订）。已把微雪官方 BSP 的两张表原样引入，并在
`display_start()` 里复刻它的"手工复位 → 3 MHz 下读 `0x04` → 选表 → 40 MHz 重建 IO"流程。
详见上一节的对照表。

### 仍待真机验证的部分（编译无法覆盖）

1. **ST77916 面板初始化表**：✅ 已解决——组件 2.x 只有一张默认表，两块面板修订都不是，
   已改为**读寄存器 0x04 自动选表**（见上）。此板实测为 **version 2**。
   若换到另一块 1.85B（version 1），代码会自动选中 v1 表，无需改动。
2. **RGB / BGR 顺序**：首启若红蓝颠倒，把 `rgb_ele_order` 改成 `LCD_RGB_ELEMENT_ORDER_BGR`。
   微雪官方 BSP 用的是 `LCD_RGB_ELEMENT_ORDER_RGB` + 16 bpp，与此处一致。
3. **触摸方向与阈值**：CST816S 的 `swap_xy` / `mirror_x` / `mirror_y` 可能需要按实机微调。
4. **绘制缓冲**：✅ 已解决——改为分带渲染，见上（原 `use_psram = true` 是屏幕不亮的根因）。
5. **barge-in 时长**：默认 200 ms，按第 5 节的说明在实机上微调。
6. **电量计充电状态**：当前 `charging` 恒为 `false`（未解码 BQ27220 充电状态寄存器）。

