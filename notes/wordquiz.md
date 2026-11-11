# 英语口语（听力练习，1.5.3a B2，分支 `feat/1.5.3a`）

> 基点 main `b923afd`（1.5.2 正式版）。单词卡片见 `wordcards-tts-server.md` 与 `wordcards-phase2.md`，食物组见 `wordcards-food.md`。
> 本目录随仓库公开：内网地址、口令、密钥一律用占位符。

## 玩法（用户 2026-11-11 晚定，不要再问）

- 测试页标题「英语口语」。题库从已上线词组选：水果 4 + 动物 8 + 物品 4 = 16 词。
- 每轮 5 题随机、题内不重复（5 个答案互不相同）；每题 3 个候选项，干扰项从同一 16 词池随机抽 2 个不等于答案的词，三选项洗牌。
- 限时 60 秒（用户从 120 秒改的）。大发声按钮读英文，下面三个候选项；发声钮只留白色发声 logo，无圆底、无边框（与单词卡片上的同款）。
- 提示照数学练习：verdict 行平时显示「第 n/5 题」，反馈 600ms 内显示「✓ 答对了，+1 分」（绿）/「✗ 答错了，不得分」（红）；计分行「本局 n 分 · 剩余 s 秒」，OVER 时变金「本局 n 分，+m 金币」。
- 发音进组预取本轮 5 词，复用单词卡片管线；点发声钮从缓存重播。音效复用 v4 的好/坏。
- 经济：免费进场，答对 1 题 +1 分，结束按 1:1 折算金币，一局最多 +5。

## 版式（效果图 `~/workspace/wordquiz-mockup-v6.png`，脚本 `~/workspace/wordquiz-mockup-v6.py`）

- 标题 16px 紫 y=38；返回钮常驻（无 START/暂停页，进组即玩，照单词卡片）。
- 发声钮 84×84 透明，中心 (180,122)，内含 Montserrat 28 的 `LV_SYMBOL_VOLUME_MAX`，transform_scale 约 1.5x。
- 选项卡 96×96 一排 y=176，x=28/132/236（底角距圆心 177.7px，在圆内）；图标 60×60 在 (x+18,y+6)，中文在 y+70。
- 选项内图标是 A8 master scale 120，对象定位 (-16,-28)；答对/答错时被点卡 CARD_PRESSED + 3px GOLD/DANGER 边（只圈被点的卡，不圈正确答案）。
- verdict 行 y=280，HUD y=306。OVER overlay：dim + 「再来一局」钮（98×66，无价格行——免费）。

## 架构

- 三件套：逻辑 `muse_quiz.{c,h}`（无 LVGL、无 ESP-IDF，时间只经 tick(ms) 推进）+ UI `muse_quiz_ui.{c,h}`（自建 50ms lv_timer）+ 无 art（复用单词卡片的 art 表）。
- 逻辑：池 16 词（水果/动物/物品，`(set,idx)` 表）；5 题部分 Fisher-Yates 发牌；阶段 START/PLAYING/OVER（无 PAUSED——离页停 UI 计时器即停钟，回来续跑）；答对 +1、答错 +0、反馈窗内不接答案、反馈结束发下一题、最后一题反馈结束或 60 秒到进 OVER（即使在反馈中）；发音待播标志 `audio_pending`；每题一槽的预取缓存 bookkeeping（复用 `muse_wordcards_cache_t`，generation 规则与单词卡片一致）；`muse_quiz_restart` 换新题故清缓存标志并 bump `quiz_gen`（与单词卡片同组 restart 保留缓存不同——题变了，字节不能留）。
- 音频模块 `muse_wordcards_audio.{c,h}` 改成列表驱动（`muse_wc_fetch_list_t`）：取词与播放共用一套 fetch task / play task / TLS client，单词卡片与 quiz 各有自己的 PSRAM 槽池（8 槽 vs 5 槽，槽池分离——两页交错进出时 READY 标志不能指向对方的字节）。原有 wordcards API 不变。
- 接线：小游戏列表「英语口语」行在「单词练习」之后第 3 位，NULL label（免费不 dim）。**这是 B 流临时入口，A 流将其移入学习页**：A 流删除小游戏列表此行，学习页「英语口语」行复用同一 `muse_quiz_ui_build`/page，只改入口，不动 quiz 模块。

## 文件清单

- `esp32/components/muse/muse_quiz.{c,h}`（新，逻辑）
- `esp32/components/muse/muse_quiz_ui.{c,h}`（新，UI）
- `esp32/components/muse/muse_wordcards_audio.{c,h}`（列表驱动：`muse_wc_fetch_list_t` + `muse_wordcards_audio_prefetch_list` / `prefetch_quiz` / `play_quiz`；wordcards 行为不变）
- `esp32/components/muse/muse_interact_ui.c`（`IC_PAGE_QUIZ` + `s_quiz_page` + `on_open_quiz` + 列表行 + build/hide 接线）
- `esp32/components/muse/CMakeLists.txt`（加 `muse_quiz.c`、`muse_quiz_ui.c`）
- `esp32/tests/quiz_harness.c` + `esp32/tests/test_quiz.py`（新，池断言、200 轮发牌不变量、答对/反馈/时钟、待播音频、缓存 bookkeeping）

## 验收

- `python3 -m unittest tests.test_quiz` 一次通过；`tests.test_wordcards` 通过。
- 全部 UI/逻辑文件桩语法检查通过（`muse_quiz_ui.c` 仅剩桩缺口 `lv_obj_set_style_transform_scale_y`——桩头缺该函数，main 里的数学练习 UI 同样报它；`muse_interact_ui.c` 与基线一致，16 个桩缺口，零新增）。
- 全量主机测试与干净基线（590 个、失败 27 + 错误 10 + 跳过 10）逐行比对、零新增。

## 未做（留给用户 / A 流）

- 未编译验证、未实机验收（本机无工具链，主机测试通过 ≠ 固件编译通过）。
- A 流：学习板块 2×2 网格 + 改名（数学练习/英语单词/英语口语）+ 英语口语入口从小游戏列表移入学习页。
