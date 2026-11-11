# 单词练习第二期（分支 `feat/wordcards-phase2-20261111`）

> 基点 main `869d7c0`（1.5.2c）。第一期见 `wordcards-tts-server.md` 与分类页 v2。
> 本目录随仓库公开：内网地址、口令、密钥一律用占位符。

## 改了什么

| 项 | 内容 |
|---|---|
| 新组 ① | **动物2 · Animal 2**：bear 熊、duck 鸭子、frog 青蛙、fox 狐狸（练习册照片） |
| 新组 ② | **物品 · Things**：balloon 气球、box 盒子、bag 包、robot 机器人（课本 Unit 4/6/7） |
| 改名 | 原动物组改名 **动物1 · Animal 1**（cat/dog/bird/fish 不变）；组名即卡片页标题，改一处即全改 |
| 分类页 | 4 行：水果 / 动物1 / 动物2 / 物品；行从固定位置改成**可上下滚**（与小游戏列表同一做法，flex 纵列、只竖滚、关滚动条、底垫 110 px，最后一行能滚出圆屏下沿）；提示行「免费 · 每类限时 2 分钟」已删（4 行时第 3 行 y=220 与提示行 y=232 重叠，放不下） |
| 玩法 | 与第一期一致：免费学习档、每组限时 2 分钟、进组即玩（无开始页/暂停页、长按不暂停、返回钮常驻）、翻面自动出声、点发声 logo 重播、点卡片其他区域翻回 |
| 图标 | 全部复用 game-icons（CC BY 3.0，须署名；与剪刀石头布、第一期同一管线：单色 SVG master → 固件内 A8 128×128 master 各 16 KB → 运行时重着色）。8 张新 art 对齐度（IoU，对 240 px 彩色渲染稿）：bear 0.9854、duck 0.9990、frog 0.9995、fox 0.9993、balloon 0.9977、box 1.0000、bag 0.9986、robot 0.9988。bear 低于 0.99 是阈值测量问题（细毛笔画在 >127 阈值下被磨掉），包围盒与原图一致，240 px 下 IoU 0.9957，不是图做坏了 |

图标来源（已核实为真实文件，凭记忆猜作者/文件名会大面积 404，核实方法见下）：

| 词 | game-icons 文件 | 颜色 |
|---|---|---|
| bear | `delapouite/bear-head.svg` | #a8763e |
| duck | `delapouite/duck.svg` | #f6cb4b |
| frog | `lorc/frog.svg` | #6ff0bf |
| fox | `lorc/fox-head.svg` | #ff8c42 |
| balloon | `delapouite/air-balloon.svg` | #ff5c5c |
| box | `delapouite/cardboard-box.svg` | #b07a4a |
| bag | `delapouite/hand-bag.svg` | #a77dff |
| robot | `lorc/robot-golem.svg` | #7dadcb |

## 词表与发音

- 固件词表 `muse_wordcards.c`：两组新表追加在索引 8（动物2）、9（物品），`MUSE_WORDCARDS_SETS` 8→10；其余 6 组（颜色/身体/数字/学校/食物/家庭）仍是 ART_NONE、不上线。**颜色组是用户明确否掉的（太简单），不要激活。**
- 预生成词是 **32→39 个不同词**（40 张卡）：`gen_word_audio.py` 只追加 7 个（bear、duck、frog、fox、balloon、box、robot）；**bag 不重复生成**——学校组里已有 bag（书包），物品组的 bag（包）是同一个英文词，共用一个 MP3。服务端 `word=1` 分支是通用的（按文本查磁盘缓存、缺失再合成），**不需要改服务端逻辑**。
- 效果图：卡片页 v3 `wordcards-phase2-v3.png`、分类页 v3 `wordcards-category-v3.png`（都在助手侧工作目录，不在仓库里）。

## 验证（助手侧，未上真机）

- 主机测试：改前 590 个（27 败 + 10 错 + 10 跳过），改后 590 个，失败清单逐行相同，**零新增**。10 个错是 `setUpClass` 找不到 `managed_components/.../cJSON.c`（本机没跑过 `idf.py build`），与改动无关。
- 桩语法检查（`cc -fsyntax-only` + goldrain-stubs）：`muse_wordcards_ui.c`、`muse_wordcards.c`、`muse_wordcards_art.c` 及 8 个新 art 文件 0 报错。顺带补了一个本地桩缺口：`lv_pct` 声明（`build_minigame` 早就在用，桩头里漏了；已按 LVGL 9.5 `src/misc/lv_area.h:246` 核对），桩文件只在助手本机，不进仓库。
- 旧 8 张 art 用新生成器重跑过：像素数据字节一致，只变了生成器文件名那行注释。
- **未做**：无 ESP-IDF 本机编译、无实机验收（助手侧无工具链）；主页面「小游戏 ｜ 每种 -10金币」文案未改（对免费行不准确，用户定）；relay/TTS 低时延代码未动。

## 合入后用户侧步骤

1. 主机上跑预生成（新 7 个词）：拷 `tts-server/gen_word_audio.py` 到桥主机 `/opt/muse-tts/`，按现有容器方式跑一遍，确认 7 个新 MP3 已生成（bag 不用）。
2. 本机编译（`tools/muse/board.sh build lcd185b`），确认无 `-Werror`。
3. 实机验收：分类页 4 行可上下滚、最后一行（物品）能滚到可点位置；动物1/动物2/物品三组进组即玩、翻面出声、2 分钟到时结算「本次学了 X 张」；新 7 个词翻面立即出声（预取命中，不应等现调现等）。
