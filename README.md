<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Muse Gadgets

> **中文完整操作指南（本板 1.85B 全程实操）：[`README_zh.md`](README_zh.md)** —— 含分步刷机、
> 语音播报服务端、故障排查、已知限制与**更新记录（v1.0 → v1.1）**。本节以下的英文是上游原文。

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset=".github/images/muse-gadgets-dark.png">
    <img src=".github/images/muse-gadgets-light.png" width="900" alt="Muse gadgets: a Waveshare round AMOLED, an M5Stack StickS3, Muse Home Link, a Raspberry Pi and a Seeed reTerminal e-ink display">
  </picture>
</p>

Muse gadgets are open source devices you build yourself. Program an
off-the-shelf ESP32 board or set up a Raspberry Pi with our device SDKs, then
connect Muse to your displays, buttons, sensors, actuators, and whatever else
you've got lying on your workbench.

We open sourced the SDKs and firmware here. It's built by hackers, for hackers,
just for fun. Side effects of tinkering may include bricked boards, voided
warranties, brownouts, or bankruptcies. Proceed at your own risk!

| | |
|---|---|
| [**ESP32 Device SDK**](esp32) | Connect your ESP32 board to Muse through our open source SDK. Throw in a screen to show images, add audio in and out, or wire up other sensors. |
| [**Linux Device SDK**](linux) | Turn that spare Raspberry Pi or Linux box into a Muse gadget. Hack in your own commands to let Muse handle sysadmin chores or your Home Assistant setup. |

Before you flash or pair a gadget, get an
[SDK token](https://gadgets.muse.ai/settings/sdk-tokens) and review the
[Gadget SDK Terms](https://gadgets.muse.ai/sdk-terms). Every gadget needs a
token to pair.

---

## 本地分支：微雪 ESP32-S3-Touch-LCD-1.85B 移植与调试记录

> 本仓库在 `facebookincubator/muse-gadget-sdk` 基础上增加了
> **Waveshare ESP32-S3-Touch-LCD-1.85B** 板级移植，并记录 2026-10-15 一天的真机调试。
> **完整的分步操作指南在 [`README_zh.md`](README_zh.md)**，本节只是要点索引。

### 屏幕：两个坑，都踩过

1. **屏幕不亮** —— 绘制缓冲放在了 PSRAM。ESP-IDF 的 SPI 驱动**不能通过 DMA 直接读
   PSRAM**（`esp_lcd_panel_io_spi` 会给 PSRAM 缓冲打上 `SPI_TRANS_DMA_USE_PSRAM`，
   ESP32-S3 上供不上 QSPI 速率就报 `DMA TX underflow`，屏幕完全没有像素）。
   修法：改用本仓库既有的 **`muse_lcd_bands` 分带渲染**——1.75C / 2.16 / Watcher /
   Stopwatch 都这么做，1.85B 是唯一漏掉的。
2. **花屏横条纹** —— 这块板**混装两个 ST77916 面板修订版**，靠面板**寄存器 0x04**
   区分（`00 7F 7F 7F` = v1，`00 02 7F 7F` = v2）。`espressif/esp_lcd_st77916` 2.x
   只剩一张内置默认表，**两张都对不上**。已内置官方双表 + 0x04 探测自动选表，
   本机这块是 **v2**，换另一块不用改代码。

详见 [`README_zh.md` 第 11.1 / 11.2 节](README_zh.md#111-屏幕不亮--psram-绘制缓冲改分带渲染)。

### 构建环境（Windows）

- **组件仓库走中国镜像**：`$env:IDF_COMPONENT_STORAGE_URL = "https://components-file.espressif.cn/"`。
- **在 AI 代理里构建会被"删除保护"打断**：代理工具经 `PYTHONPATH` 注入的
  safe-delete 钩子会拒绝组件管理器的批量删除，构建 FATAL_ERROR。
- **⚠️ `dependencies.lock` 会被写坏成半截**（`dependencies:` 映射只剩 4 条），
  于是每次构建都认为 16 个组件"未使用"要删 → 再次被拦 → **死循环**。
  **破解：`managed_components/` 与 `dependencies.lock` 一起清再建**
  （就是仓库 `tools/muse/board.sh` 每次构建做的 `clean()`）。
  全量 2166 步约 18～20 分钟。

详见 [`README_zh.md` 第 11.3 节](README_zh.md#113-构建环境坑windows--本机)。

### 静态 IP（新增）

原来只有 SSID/密码的预置通道，**静态 IP 是新加的**。Kconfig 新增
`HOMEHUB_STATIC_IP` / `_NETMASK` / `_GATEWAY` / `_DNS`，实现在 `main/wifi_mgr.c`
的 `apply_static_ip()`，挂在 `WIFI_EVENT_STA_START`（必须在这里：IDF 的 Wi-Fi glue
注册的 STA_START 处理函数在前，那一刻 netif 与 DHCP 客户端都已起来，
正是 `esp_netif_dhcpc_stop()` 需要的状态）。解析失败会自动退回 DHCP。
本机配置：SSID `MyHome_5G`、`192.168.1.50/24`、gw/dns `192.168.1.1`。
⚠️ 把密码写进 `sdkconfig` 等于写进固件镜像，拿到板子就能读出来。

详见 [`README_zh.md` 第 3.3 节](README_zh.md#33-预置-wi-fi-与静态-ip可选)。

### 语音播报 / TTS（新增，**尚未跑通**）

**Muse SDK 默认不出声是设计**：端侧不跑模型，回复只回文字，官方留了
`start_tts()` 这个接口让你自己接。本仓库补上了这条链路
（`components/muse/muse_tts.{c,h}`）：设备侧只做「把回复文本填进
`CONFIG_MUSE_TTS_URL` 的 `{text}` 占位符 → GET 回 MP3 → 交给 SDK 自带的
minimp3 + 重采样器播放」。**换语音服务 = 改一行 URL，不用碰固件**：

```
CONFIG_MUSE_TTS_URL="https://fanyi.baidu.com/gettts?lan=zh&text={text}&spd=5&source=web"
CONFIG_MUSE_TTS_URL="http://192.168.1.1:5050/tts?voice=zh-CN-XiaoxiaoNeural&text={text}"
```

**为什么默认不用 Edge TTS**：Edge TTS 的 `Sec-MS-GEC` 令牌已升级 **v2**
（`<16位CS64>-<64位SHA256>`，内含刻意混淆的自定义 SHA-256，约 200 行密集位运算），
算错即 403，且微软随 Chromium 版本推进会改。要 Edge 音色就在局域网跑官方
[`edge-tts`](https://github.com/rany2/edge-tts) 包个 HTTP 服务，把 URL 指过去即可
——令牌算法由上游维护。

> ⚠️ **当前状态：已接线、已编译刷入，但真机实测仍未出声，尚未定位。**
> 另外只有**语音回合**（按住按钮说话）会朗读，打字回合按设计只显示文字，
> 所以无法用串口触发朗读来做验证。排查清单见
> [`README_zh.md` 第 3.4 节](README_zh.md#34-让-muse-说话语音播报可选)。

---



ESP32 and Linux gadgets pair with the Muse app on iOS and Android, via
Settings > Devices. Turn on Developer mode there first, then look for devices
prefixed with "MuseGadget".
Each directory has a `README.md` to get started and an `AGENTS.md` for coding
agents like [Muse Code](https://developer.meta.com/ai/lp/muse-code/).

For battery-input power profiling, see the [porting recipe](esp32/docs/power/README.md).
The opt-in diagnostic core is board-neutral; boards supply its hooks and state
matrices, and [power_sweep.py](esp32/tools/power/power_sweep.py) drives the
USB-free sweep. The isolated diagnostic does not pair or need an SDK token;
fixture safety and explicit hardware authorization still come first, as the
[PPK2 rules](esp32/docs/power/ppk2.md) describe.

## Community

Meet other hackers who are building and customizing Muse gadgets in our
community [Discord](https://discord.gg/3bhjCkZdd6). Get inspired, support each
other, and share what you make.

## License

Muse Gadgets is licensed under the Apache License, Version 2.0, found in
[`LICENSE`](LICENSE), except for these third-party files, which keep their
upstream licenses:

| Path | Upstream | License |
|---|---|---|
| [`esp32/components/minimp3/include/minimp3.h`](esp32/components/minimp3) | [lieff/minimp3](https://github.com/lieff/minimp3) | CC0-1.0, see [`LICENSE`](esp32/components/minimp3/LICENSE) |
| [`esp32/main/pixel_font.c`](esp32/main/pixel_font.c) | Adafruit GFX `glcdfont.c` | BSD-2-Clause, in the file header |

Dependencies fetched at build time are under their own licenses: ESP-IDF
components (into `esp32/managed_components/`), and the simulator's LVGL and
SDL (listed in [`esp32/simulator/THIRD_PARTY.md`](esp32/simulator/THIRD_PARTY.md)).

The Apache License does not cover the [Jollybot avatar](esp32/avatar).
