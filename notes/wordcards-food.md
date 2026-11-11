# 单词练习第三期：食物组（1.5.3a B1，分支 `feat/1.5.3a`）

> 基点 main `b923afd`（1.5.2 正式版）。第一、二期见 `wordcards-tts-server.md` 与 `wordcards-phase2.md`。
> 本目录随仓库公开：内网地址、口令、密钥一律用占位符。

## 改了什么

| 项 | 内容 |
|---|---|
| 新组 | **食物 · Food**：cake 蛋糕、milk 牛奶、bread 面包、egg 鸡蛋（词表第 6 组，本来就在表里，只把 ART_NONE 换成 art 16..19） |
| 分类页 | 5 行：水果 / 动物 / 食物 / 物品（表顺序），可滚列表自动多出一行，不用改布局；提示行「限时免费」仍是列表最后一项 |
| 玩法 | 与之前各组一致：免费学习档、不限时、进组即玩、翻面自动出声、点发声 logo 重播、点卡片其他区域翻回 |

图标来源（已核实为真实文件，game-icons，CC BY 3.0，须署名；与之前各组同一管线：单色 SVG master → 固件内 A8 128×128 master 各 16 KB → 运行时重着色）：

| 词 | game-icons 文件 | 颜色（用户 2026-11-11 晚确认） | IoU（对 240 px 彩色渲染稿） |
|---|---|---|---|
| cake | `lorc/cake-slice.svg` | #ff8fb3 粉 | 0.9984 |
| milk | `rihlsul/milk-carton.svg` | #8ec9ff 浅蓝 | 0.9991 |
| bread | `delapouite/bread.svg` | #e0a458 面包金 | 0.9952 |
| egg | `delapouite/egg-eye.svg` | #ffd23e 蛋黄 | 1.0000 |

预览图 `~/workspace/wordcards-food-v1.png`（用户已过目）；颜色按此表写进词表，预览图与固件 art 颜色保持一致。

## 词表与发音

- **发音零新增**：cake/milk/bread/egg 4 个词已在 39 词预生成表里（第一期 32 词就含食物组），`tts-server/gen_word_audio.py` 不用改，桥不用动。
- art 表从 16 扩到 20（`MUSE_WORDCARDS_ART[20]` / `MUSE_WORDCARDS_ART_COLOR[20]`），新 art 用生成器 `~/workspace/wordcards-art-gen-food.py`（只写 4 个 `.c` 文件，不碰表，表是手改的）。

## 文件清单

- `esp32/components/muse/muse_wordcards_art_{cake,milk,bread,egg}.c`（新，A8 master 各 16 KB）
- `esp32/components/muse/muse_wordcards_art.{c,h}`（表 16→20 + 颜色）
- `esp32/components/muse/muse_wordcards.c`（食物组 art 16..19 + 颜色）
- `esp32/components/muse/muse_wordcards.h`（注释：食物组已上线）
- `esp32/components/muse/muse_wordcards_ui.h`（注释：分类页 4 组名）
- `esp32/components/muse/CMakeLists.txt`（4 个新 `.c`）
- `esp32/tests/test_wordcards.py`（art 字节和：cake 1772340、milk 1164517、bread 531518、egg 1497558）
- `esp32/tests/wordcards_harness.c`（食物组表断言，`offered_sets() == 4`）

## 验收

- 主机测试：`python3 -m unittest tests.test_wordcards` 通过；全量与 1.5.2 基线（590 个、失败 27 + 错误 10 + 跳过 10）逐行相同、零新增。
- 未编译验证、未实机验收：本机无工具链，固件编译与真机（分类页 5 行、食物组翻面出声）由用户在自己机器上做。
