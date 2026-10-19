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

- [更新记录](#更新记录)
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
10. [TTS 服务端：Azure Speech 注册 + Docker 搭建](#10-tts-服务端azure-speech-注册--docker-搭建)
11. [许可证](#11-许可证)

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
  另按《官方 1.75C 与 1.85B 差异对比》补：触摸控制器**关掉自动休眠**（`0xFE=0x01`，否则长间隔后第一下触摸只唤醒、不出坐标）、
  面板修订探测失败改为**显式 WARN + 重试一次**（不再静默当 v2）、`0xED` 写入移到 Kconfig 开关
  （`MUSE_TOUCH_IRQ_PULSE_WIDEN`，默认开）、麦克风增益按 ES7210 的 3 dB 档对齐
  （33 dB 档实际下发 34.5，因为 `esp_codec_dev` 会把 33 归到 30）、补**按住头像 0.5 s 说话**
  （`TOUCH_HOLD_MS`，只有从脸页起按才算）、`aux_button` 改 `NULL`（设置页不再提示「按 boot 关屏」）。
  **按住说话与脸页控件互斥**：脸页上的音量圆钮与电源图标自己拥有按压（命中测试含 12 px 外扩，与点击区一致），
  按住它们不再被当成「按住头像说话」；电源图标同时改为可点——点一下即关机，走菜单/设置页同一条 `muse_input_request_power_off()`。
- 已知限制：电量计 CEDV 模型用的是参考电芯参数、未做 GPCCEDV 标定（容量项已按本板 500 mAh 写回）；
  I2C 自愈已在电池态 T3 上验证生效，但「真实总线卡死」这一触发条件难以主动复现，属于**有备无患**的代码路径。

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
  ⚠️ 当时的已知问题（v1.2 / v1.3 已分别修掉硬件引擎的配置漂移与 I2C 截断，详见上方记录）：
  长时间睡眠后敲击失效；按键点亮→自动息屏后再敲则正常。
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
| 屏幕驱动 | **ST77916**，**QSPI**（80 MHz），出厂混装两个面板修订版（靠寄存器 `0x04` 区分） |
| 触摸 | **CST816S** 电容触摸，I2C（7 位地址 `0x15`） |
| 音频输出 | **ES8311** 编解码器（`0x18`）+ 功放使能引脚（`PA_CTRL`） |
| 音频输入 | **ES7210** 双麦克风阵列（`0x40`）+ 回声消除 |
| 姿态传感器 | **QMI8658A** 六轴 IMU（`0x6B`），已接入：tap 引擎 + 敲击唤醒 |
| 电量计 | **BQ27220**（`0x55`） |
| RTC | **PCF85063**（`0x51`），固件暂未使用 |
| 按键 | **BOOT**（可编程，按住说话）、**PWR**（纯硬件电源键，固件读不到） |
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
| 板载 I2C 总线（共用） | `SCL=10`、`SDA=11`；挂 CST816S `0x15`、ES8311 `0x18`、ES7210 `0x40`、BQ27220 `0x55`、QMI8658A `0x6B`、PCF85063 `0x51` |
| IMU 中断 | `QMI8658A INT1 = GPIO8`（经 R51 0R）；`INT2` 未接 |
| 按键 | `BOOT=GPIO0` |

> 注意：`GPIO45 / GPIO46` 同时是芯片的 strapping 引脚（`GPIO45` 在复位时决定 `VDD_SPI` 电压），
> 它们同时是 LCD 的 `DATA1 / DATA0`，这是微雪原厂设计，属于已知取舍，无需处理。

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
`esp_lcd_st77916`、`esp_lcd_touch_cst816s`、`espressif/bq27220` 等依赖到 `managed_components/`，需要联网。

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
>
> 💡 **本机私有配置放 `esp32/devices/sdkconfig.local`**（已 gitignore）：TTS 地址、Wi-Fi 凭据、
> 静态 IP 这类「只属于这台机器」的东西写这里，`tools/muse/board.sh` 会最后加载它。
> 好处是删构建目录不会丢，也不会被提交上去——只写进 `build-…/sdkconfig` 的话，删一次就没了。

#### 3.3 预置 Wi-Fi 与静态 IP（可选）

如果不想每次刷完都走蓝牙配网，可以把 Wi-Fi 凭据直接编进固件。在
`idf.py menuconfig` 的 **Home Hub** 菜单里填，或写进 `esp32/devices/sdkconfig.local`
（推荐，已 gitignore，删构建目录不丢）：

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

> 运行期改地址用 `>net.set ip=… mask=… gw=… [dns=…]`（写 NVS 后重启），`>net` 查当前生效值
> 与来源（nvs / build / dhcp），`>net.clear` 交回 DHCP。地址存 NVS 是为了**OTA 升级后不丢**。

> ⚠️ 把密码写进 `sdkconfig` 就等于写进固件镜像，任何人拿到板子都能读出来。
> 仅适合自己调试用的板子。

#### 3.4 让 Muse 说话（语音播报）

**Muse SDK 默认不会出声。** 这不是故障：它是"瘦客户端"设计，录音上传云端转文字，
回复只回文字（`voice.h` / `muse_chat.h` 都写明了），官方留了一个 TTS 接口让你自己接。
所以默认行为是**用静音按阅读速度走字幕**——你看到字、听不到声，属正常。

本仓库补上了这条链路（`esp32/components/muse/muse_tts.c`）。设备侧只做一件事：
把回复文本填进一个 URL 模板、GET 回 MP3、交给 SDK 自带的 minimp3 + 重采样器播放。
换个语音服务＝**改一行 URL**，不用碰固件：

```
CONFIG_MUSE_TTS_URL="http://<tts-host>:5050/tts?voice=zh-CN-XiaoxiaoNeural&text={text}"
```

留空（默认值）就只显示字幕。长回复会按标点切成 **180 字符**一段、分多次请求，
所以 URL 长度可控、播放也能早于全部合成完成就开始。

> **本仓库的实际部署**是局域网 `tts-server/` + **Azure Speech（eastasia）**，
> 音色 `zh-CN-XiaoxiaoNeural`，首音 0.47–0.58 s；服务端注册与 Docker 搭建见
> [第 10 节](#10-tts-服务端azure-speech-注册--docker-搭建)。
>
> `CONFIG_MUSE_TTS_URL` **默认留空**：固件里不烧任何厂商端点，新克隆的仓库构建出来
> 就是"只有字幕、没有声音"——这是设计，不是 bug。
>
> **填在哪里**：写进 `esp32/devices/sdkconfig.local`（已 gitignore，`tools/muse/board.sh`
> 最后加载它，优先级最高），而不是只写进 `build-…/sdkconfig`——后者删构建目录就没了：
>
> ```
> CONFIG_MUSE_TTS_URL="http://<tts-host>:5050/tts?voice=zh-CN-XiaoxiaoNeural&text={text}"
> ```

**不要在设备端直连 Edge TTS**：`speech.platform.bing.com` 的 readaloud 端点现在要
`Sec-MS-GEC` v2 令牌（内含一套刻意混淆的自定义 SHA-256），设备端算不动、算错即 403，
且微软随 Chromium 版本推进会改。令牌算法交给服务端的 `edge-tts` 包维护，设备只收 MP3。

> **注意**：目前只有**语音回合**（按住按钮说话）会朗读回复。串口/App 里**打字**的回合
> 是"只显示文字"的设计（`muse_chat_session.cpp` 的 `message_done()` 在文字分支提前返回），
> 所以**没法用串口命令触发一次朗读来做验证**，只能按住按钮说话。

成功时的串口标志（可作回归的验收行）：

```
I muse_chat_session: speaking message assistant-msg-… (N chars)
I muse_chat_session: reply audio: 24000 Hz, 1 ch, 48 kbps
I muse.tts: TTS got NNNNN bytes
I muse_voice: muse reply: X.XXs of audio, N.NNs total
```

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
Muse 分区表没有 `factory` 分区（只有 `ota_0` / `ota_1`），所以 `erase-flash` 之后
**原厂固件在片上就没了**——第 4 步那份备份是唯一的回退途径。

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

**重新配对**：触摸屏进 **设置页 → Reset pairing**（要连点两次确认），确认后设备会
忘掉 Wi-Fi 与 Muse 配对并重启。想只换 Wi-Fi 就在设置页的 Wi-Fi 列表里双击那条记录（"Tap to forget"）。

> 每次配置都会建立一条全新的加密会话。由于这是社区设备，
> **配对没有厂商验证，无法主动防御中间人攻击**——请在可信的网络上配置。

---

## 5. 日常使用

| 操作 | 方式 |
|---|---|
| **按住说话（PTT）** | 按住 **BOOT** 键说话，松开结束；亮屏时也可以**按住头像 0.5 秒**说话 |
| **确认配对** | 按一下 **BOOT** 键 |
| **亮屏** | **敲一下屏幕**、按一下 **BOOT**，或点设置页的电源图标 |
| **熄屏** | 无操作自动熄屏（设置页 **AUTO-SLEEP**：从不 / 30 秒 / 1 分钟 / 2 分钟 / 5 分钟 / 10 分钟，出厂默认 **2 分钟**）；也可在设置页点 **Sleep now** 立刻熄 |
| **菜单 / 设置** | 在**触摸屏**上操作（主界面左滑进入设置） |
| **关机** | 点设置页的**电源图标**（或菜单里的关机）→ 进入 **deep sleep**；按 **BOOT** 键唤醒 |
| **硬件断电** | 按 **PWR** 键（约 2 秒），这是纯硬件电源开关，固件读不到它 |

> 这块板上「敲屏幕亮屏」走的是**触摸控制器 CST816S**（`INT=GPIO4`），不是 IMU 敲击引擎——
> 见[第 8 节](#8-已知限制与注意事项)。

### 熄屏（sleep）与关机的区别

三档状态容易混，区分清楚：

| 状态 | 屏幕 | CPU / 无线 | 怎么回来 | 日志 |
|---|---|---|---|---|
| **亮屏** | 亮 | 全速 | — | `full power` |
| **熄屏（asleep）** | 背光 0 + 面板 `SLPIN` + 黑遮罩 | **电池供电**：CPU 降到晶振时钟、在按键/触摸轮询之间 light sleep，两分钟无网后 Wi-Fi 也 nap；**插 USB**：CPU 仍全速，只关显示 | 敲屏 / 按 BOOT / 点电源图标 | `sleeping (auto-sleep)` |
| **关机（deep sleep）** | 灭 | 仅 RTC 域 | 按 **BOOT** | `power off` |

所以「自动熄屏」= `muse_state_set_asleep(true)`，**不是** deep sleep，也**不等于**一定进 light sleep：
`update_power()` 里进低功耗的前提是 `muse_state_on_battery()`——插着 USB 调试时只会关屏，
CPU 不会降频（日志会写 `display paused (USB host: CPU at full speed)`）。想在 USB 上复现电池态的低功耗，
用 `>pmlow=1`。

自动熄屏只发生在**空闲（`MUSE_MODE_IDLE`）**且无操作超时的时刻；配对提示（passkey / 等待确认）期间
会强制保持亮屏，说话与思考过程中也不会熄。设置 30 秒后，日志应出现 `sleeping (auto-sleep)`。

**关于语音交互的一个重要设计**：

本固件**没有常开语音检测（no always-on VAD），也不支持全双工**。
Muse **只在你按住 BOOT 键时才录音**，所以扬声器的声音永远不会像
「常开监听」类固件那样把录音提前打断。

如果**在 Muse 还在说话时**按下 BOOT（打断式插话 / barge-in），
固件会先丢弃 `CONFIG_MUSE_BARGE_IN_SETTLE_MS`（**默认 200 ms**）的采集音频，
这段正是扬声器自己的尾音。如果发现插话录到的开头是 Muse 自己的声音，
就在 `menuconfig` 的 **Muse** 菜单下把这个值调大；如果发现开头被削掉了，就调小。
设为 `0` 表示不丢弃。

**电池与功耗**：电量由 BQ27220 电量计读出（CEDV 模型按本板 500 mAh 电池写回，
百分比以电量计为准）。屏幕关闭时，CPU 会降频并在按键/触摸轮询之间进入 light sleep；
插着 USB 时芯片保持全速运行。接上 3.7 V 锂电池后电量与充电状态才有真实数据。

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

板子自己的控制台命令（串口连上后输入，前面带 `>`）：

| 命令 | 作用 |
|---|---|
| `>status` | 一行 JSON：版本、电量、Wi-Fi、Muse 会话状态 |
| `>net` | 打印实际 IP/掩码/网关/DNS、DHCP 状态与地址来源（nvs / build / dhcp） |
| `>net.set ip=10.0.0.5 mask=255.255.255.0 gw=10.0.0.1 dns=10.0.0.1` | 写入 NVS 并重启；发布件里不内置地址，靠它给板子定地址 |
| `>net.clear` | 删除 NVS 地址，重启后回到 DHCP |
| `>tp` | 触摸线电平、唤醒武装状态、睡前 drain 的结果 |
| `>i2c` / `>i2c.reset` | 总线失败计数与 SDA/SCL 电平 / 立刻复位总线并重设上拉 |
| `>imu` / `>imu.check` / `>imu.rearm` | 打印 / 校验 / 重臂 QMI8658A tap 引擎 |
| `>batt` | BQ27220 标准寄存器全量转储（`Voltage`/`Capacity`/`Current`…） |
| `>power` / `>power.reset` | 电池计时统计（睡眠占比等）/ 归零重新计 |
| `>pmlow=1` / `>pmlow=0` | 带 USB 也强制进 / 退出 light sleep（复现电池态用；进入后 USB 串口会断） |
| `>ota` | 手动触发一次 GitHub OTA |

---

## 7. 故障排查

| 现象 | 排查方向 |
|---|---|
| 构建报错 `No SDK token` / Token 非法 | Token 必须为空（仅警告）或严格为 `mgst_` + 43 个 base64url 字符 |
| 构建报错 `Cannot establish a connection to the component registry`（URL 指向 `components-file.espressif.com`） | 国内网络访问不了 CloudFront 文件源。设置 `IDF_COMPONENT_STORAGE_URL=https://components-file.espressif.cn/` 后重新构建，详见[第 3 步的国内加速说明](#32-构建) |
| 构建报错 TCP buffer / cJSON nesting | 生成的 `sdkconfig` 过期，删除整个构建目录后重新构建 |
| 构建反复出现 `Direct dependencies have changed` / `Deleting N unused components` 后失败 | `dependencies.lock` 被截断（正常应有约 19 条 `dependencies:` 映射，只剩 4 条即异常）。把 `managed_components/` 与 `dependencies.lock` **一起删掉**再重建（`tools/muse/board.sh` 每次构建前就会这么做） |
| `esptool` 找不到串口 | 换一根**能传数据**的 USB 线；检查设备管理器里的 COM 口；按住 BOOT 再重插 USB 进入下载模式 |
| 刷完起不来 / 反复重启 | 首次刷入前**没有执行 `erase-flash`**，执行后重刷 |
| 屏幕有竖条纹 / 边缘色带 | ST77916 面板初始化表选错。板级文件已按寄存器 `0x04` 自动选表（v1 / v2 两张），日志应打印 `ST77916 reg 0x04 = … -> init table vN`；探测失败会 WARN 并重试一次 |
| 屏幕红蓝反了 | 把板级文件里 `rgb_ele_order` 从 `LCD_RGB_ELEMENT_ORDER_RGB` 改成 `BGR` |
| 触摸无反应 | overlay 已设 `CONFIG_ESP_LCD_TOUCH_CST816S_DISABLE_READ_ID=y`（部分 CST816S 变体读 ID 会失败）；若仍无反应，用 `>tp` 看 INT 电平与武装状态，并检查触摸排线与 `TP_RST=GPIO1 / TP_INT=GPIO4` |
| 亮屏后 UI 冻住、触摸和按键都像失效 | 电池态首次睡醒后 I2C 总线可能卡死（日志 `probe device timeout`、`BQ27220 not answering`）。1.3 已加自愈：连续 2 次失败自动 `i2c_master_bus_reset()`；手动用 `>i2c` 看计数、`>i2c.reset` 立刻复位 |
| 熄屏后屏幕每隔半分钟左右自己亮一下 | 熄屏瞬间触摸控制器自抖的中断被当成触摸。1.3 已修（睡前 drain + `last_slept_ms≈0` 判为"睡眠被拒绝"）；仍复现就贴 `>tp` 与 `@imu.wakes` 的输出 |
| 敲屏幕唤不醒 | 这块板的敲醒来自 CST816S 触摸（`GPIO4`），不是 IMU。先 `>tp` 看 INT 是否被武装；电池态才需要关注唤醒线，USB 供电下不会进 light sleep |
| 电量显示明显不对（如恒 33% / 0%） | 电量计电池模型不对：`>batt` 看 `DesignCapacity` / `FullChargeCapacity`（本板电池是 **500 mAh**）；读回 0 mV / 0% 会被判为总线故障并保留上次值。`>power.reset` 归零统计后重启 |
| 一直显示「紫色（未配对）」 | SDK Token 未填或已失效，去 gadgets.muse.ai 重新签发并重新构建 |
| 日志里报 `BQ27220 not answering` | 未接电池时属正常现象；接了电池仍报错则检查 I2C（`>i2c`） |
| 只有字幕、没有声音 | 先看 `CONFIG_MUSE_TTS_URL` 是否还停在占位端点（[3.4](#34-让-muse-说话语音播报)）；再确认服务端 `/health` 正常、`reply audio: 24000 Hz` 这行是否出现 |
| 连不上 Wi-Fi | 确认路由器是 **2.4 GHz**（ESP32-S3 不支持 5 GHz） |

---

## 8. 已知限制与注意事项

- **无常开语音唤醒**：必须按住 BOOT（或按住头像）说话，不支持「小爱同学式」的随时唤醒。
- **无全双工**：Muse 说话时无法同时录音（有 barge-in 机制，见第 5 节）。
- **PWR 键固件不可读**：纯硬件电源开关；菜单/设置里的关机走 deep sleep，靠 BOOT 唤醒。
- **IMU 只用于敲击唤醒，且电池态引擎不出事件**：QMI8658A 已接入（tap 引擎、`>imu` 诊断、
  睡着时作为一条唤醒线），但电池态下引擎配置全对却零事件（`taps=0`，多次复现）。
  实际能用的是**触摸控制器**那条路——敲屏幕亮屏走 CST816S（`GPIO4`）。
- **RTC 未接入**：板载 PCF85063（`0x51`）在 I2C 总线上，但固件没有读它；日志里的「RTC 日记」
  指的是芯片的 RTC 慢速内存（掉电不丢），与 PCF85063 无关。
- **电量计未做 GPCCEDV 标定**：CEDV 模型用的是参考电芯参数，容量项已按本板 500 mAh 写回；
  充电状态由 `BatteryStatus` 的 `DSG` 位反推，没有做充放电电流积分。
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

## 10. TTS 服务端：Azure Speech 注册 + Docker 搭建

### 10.1 注册 Azure Speech（拿 key 与 region）

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

### 10.2 用 Docker 跑 tts-server

```sh
cd tts-server
cat > .env <<'EOF'
AZURE_SPEECH_KEY=<KEY 1>
AZURE_SPEECH_REGION=eastasia
TTS_VOICE=zh-CN-XiaoxiaoNeural
EOF
docker build -t muse-tts:1.5 .
docker run -d --name muse-tts --restart unless-stopped -p 5050:5050 --env-file .env muse-tts:1.5
curl -s http://127.0.0.1:5050/health
curl -s -o /tmp/t.mp3 "http://127.0.0.1:5050/tts?text=你好&voice=zh-CN-XiaoxiaoNeural"
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
CONFIG_MUSE_TTS_URL="http://<tts-host>:5050/tts?voice=zh-CN-XiaoxiaoNeural&text={text}"
```

- 验收：`/health` 里 `azure_pool_idle=1`、`stream_head=512`；板子串口出现
  `muse_chat_session: reply audio: 24000 Hz, 1 ch, 48 kbps` 与 `muse.tts: TTS got N bytes`。
  排查见 [`tts-server/README.md`](tts-server/README.md)。

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
