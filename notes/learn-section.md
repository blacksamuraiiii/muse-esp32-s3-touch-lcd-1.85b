# 学习板块（1.5.3a A，分支 `feat/1.5.3a`）

> 基点 main `b923afd`（1.5.2 正式版）。食物组与听力练习见 `wordcards-food.md`、`wordquiz.md`。
> 本目录随仓库公开：内网地址、口令、密钥一律用占位符。

## 结构（用户 2026-11-11 晚定，不要再问）

- 互动主页改 2×2 网格：四张卡 140×95（x=35/185、y=80/185、间距 10），
  顺序今日运势 → 喂饭 → 学习 → 小游戏；每张卡图标 + 标题，**无副行**
  （未抽 / 免费 / 每种 +10金币 / 每种 -10金币全删；费用只在各游戏页内提示）。
  金币行 y=302 不动；四角距圆心 176.1px，在圆内。
  效果图 `~/workspace/main-grid-mockup-v3.png`。
- 学习是子页，页标题「学习」，三张卡片（2026-11-12 用户拍板 v6，
  见下方“## 三张卡片改版”一节；三行纵列已删）：数学练习 / 英语单词 /
  英语口语。标语「好好学习，天天向上」y=84（DIM），底行金币数 y=302
  （TEXT）。效果图 `~/workspace/learn-page-mockup-v6.png`。
- 改名只改 UI 文案（列表名 + 页标题），代码名 `muse_mathpractice` /
  `muse_wordcards` 不动。学习里三项全部免费（用户 2026-11-12 定）：
  数学练习入场费取消（原来收 10 金币、0 金币变灰），英语单词免费，
  英语口语免费；数学兑付不变、最多 +20。

## 图标（game-icons，CC BY 3.0；A8 128×128 master，运行时重着色白）

| 卡 | 图标 | 来源 |
|---|---|---|
| 今日运势 | 水晶球 | `lorc/crystal-ball.svg`（保持原状） |
| 喂饭 | 纸杯蛋糕 | `delapouite/cupcake.svg`（用户选定，饭碗不好看） |
| 学习 | 魔法书 | `delapouite/spell-book.svg`（用户选定） |
| 小游戏 | 手柄 | `delapouite/gamepad.svg`（保持原状） |

生成器 `~/workspace/main-grid-art-gen.py`（同一管线，IoU 验收 ≥0.99，
实测 0.9966–0.9996）。Art 表 `muse_main_grid_art.{c,h}` + 4 个 master
`muse_main_grid_art_{crystal_ball,cupcake,spell_book,gamepad}.c`，
顺序与主页一致（今日运势、喂饭、学习、小游戏）。

## 接线

- 主页：`build_main()` 改 2×2 网格（`grid_card()`：140×95 卡 + 56px 图标 +
  标题）；`main_button()` 随旧三行列表一起删了。
- 学习页：新 `IC_PAGE_LEARN` + `build_learn()`（小游戏列表同款纵列）；
  `on_open_learn()` 开页；返回一律走 `on_back()`。
- 入口迁移：小游戏列表删「英语口语」行（B 流临时入口）；学习页
  「英语口语」行复用同一 `muse_quiz_ui_build` / `s_quiz_page`，只改入口，
  quiz 模块不动。数学练习与英语单词两行同样从小游戏列表挪过来，
  只改名（加减练习 → 数学练习、单词练习 → 英语单词），模块不动。
- 返回：数学练习 / 英语口语 / 单词分类页的 `<` 回学习页（不再回小游戏
  列表）；卡片页 → 分类页不变；其余六个小游戏回小游戏列表不变。
- `refresh_status()`：主页面板的三行文案（喂饭已领满 / 今日运势等级）
  随副行一起删了；金币行与小游戏变灰逻辑不变；数学练习的变灰标签
  已随免费化删掉（2026-11-12，学习全部免费）。

## 文件清单

- `esp32/components/muse/muse_interact_ui.{c}`：主页网格、学习页、入口迁移、返回路由、refresh_status。
- `esp32/components/muse/muse_main_grid_art.{c,h}` + 4 个 master。
- `esp32/components/muse/muse_mathpractice_ui.{c,h}`、`muse_mathpractice.{c,h}`、
  `muse_pet_model.h`：加减练习 → 数学练习（UI 文案 + 注释）。
- `esp32/components/muse/muse_wordcards_ui.{c,h}`：单词练习 → 英语单词（分类页标题 + 注释）。
- `esp32/components/muse/CMakeLists.txt`：6 个新 art 文件。

## 三张卡片改版（用户 2026-11-12 拍板 v6 单色上色版，效果图 v6）

> v4 底行标语（本地提交 `4adeab9`，未推）已被本改版取代：那笔先撤掉，
> 在 E 流头 `8cd2ec3` 之上以一笔干净提交实现 v6。

- 三行纵列（58px 行、可滚）整段删掉，改成喂饭页同款三张卡片：
  卡片 y=120、100×100、x=22/130/238、圆角 18、CARD 底；图标 80px 居中；
  名字 y=228（DIM）；标语「好好学习，天天向上」上移 y=84（DIM，
  喂饭规则行同位同色）；底行改为金币数 y=302（TEXT，与喂饭页同行
  同色，由 `refresh_status()` 实时刷新，新增 `s_learn_coins`）。
  三张卡放得下，不用滚动。点击进对应页不变
  （`on_open_mathpractice` / `on_open_wordcards` / `on_open_quiz`）。
- 图标（game-icons，CC BY 3.0；A8 128×128 master，运行时按卡重着色，
  单词卡片同款做法——主网格是全白，这里每张卡一个色）：
  数学练习 `delapouite/calculator.svg` 金 #f6cb4b（= `MUSE_THEME_GOLD`）、
  英语单词 `lorc/open-book.svg` 浅蓝 #8ec9ff、英语口语
  `delapouite/microphone.svg` 粉 #ff8fb3。生成器
  `~/workspace/learn-cards-art-gen.py`（同一管线，IoU 验收 ≥0.99，
  实测 0.9924–0.9998）。Art 表 `muse_learn_art.{c,h}` + 3 个 master
  `muse_learn_art_{calculator,open_book,microphone}.c`。
  v7 手画像素画版（计算器/书/话筒 16×16）用户看过后否决，改回 v6。

## 验收

- 主页 4 卡顺序、图标、标题正确；学习页 3 张卡片 + 标语 y=84 + 金币行
  y=302 位置与颜色正确，点卡进对应页、返回回学习页；改名无遗漏
  （列表、页标题、分类页标题）；学习三项全部免费（数学不扣入场费、
  0 金币不变灰，兑付不变、最多 +20）。
- 主机测试与 E 流后基线一致：635 个，失败 27 + 错误 8 + 跳过 10，
  失败清单逐行相同、零新增。
