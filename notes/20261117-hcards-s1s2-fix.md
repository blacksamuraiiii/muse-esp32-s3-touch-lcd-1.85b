# 单词/小游戏横向卡片 + S1/S2 修复

- 背景：2026-11-17，用户：小游戏页面和学习-单词卡片的列表都是竖的，其余页面的列表都是横向的卡片；先看单词卡片页面。评审 `review-1.5.3d.md` 的 S1/S2 同批修。
- 基点：main `d61c074`（1.5.3d）
- 分支：`feat/wordcards-minigame-hcards-20261117`
- 状态：代码已做完并本地提交；host 全量见 §5；未编译验证（本机无 ESP-IDF 工具链），待用户编译与实机验收

## 1. 单词分类页改横向卡片（效果图 v6）

### 1.1 症状/背景

单词分类页是最后一批竖向列表之一：330 宽滚动列、行高 58、图标 36px + 标题整组居中，提示“限时免费”在列尾滚动出现。用户 2026-11-17 定改成横向卡片。初版效果图 v5 是四张 80×80 一排全显；用户同日改定：改成喂饭页的横向滚动条范式（v6），卡片保持 100×100。

### 1.2 几何（v6，用户定）

- 标题“英语单词”不动（`muse_page_title`，y=44），返回钮不动。
- 标语“限时免费”固定在 y=84（DIM，`muse_font_cjk()`），不再滚动。
- 四张 100×100 卡片一横排，y=120，喂饭式 strip：左右 22px 边距、8px 间距、贴靠吸附、无滚动条、去掉弹性。初始显示前三张（水果/动物/食物），第四张（物品）露一角、左滑可见。
- 卡内图标 80px：各类的首词 art（现有 `MUSE_WORDCARDS_ART`）+ 首词颜色重着色，沿用行里的 `lv_image_set_scale` / `image_recolor` 写法。
- 卡下两行名字：中文 16px Noto DIM（y=228）、英文 Montserrat 14 DIM（y=249，`lv_font_montserrat_14`，LVGL 自带）。
- 金币行：新增 label 固定在 y=302（TEXT，初值“金币 --/100”），由 `muse_interact_ui.c` 的 `refresh_status()` 从快照重写——用户原话“也可以正常显示金币数啊，跟别的页面保持统一”。单词练习本身仍免费，只显示余额、不扣费。

### 1.3 修法

`esp32/components/muse/muse_wordcards_ui.c:563` `muse_wordcards_ui_build_sets` 重写，宏在 `:155-169`：

- strip 照抄 `muse_interact_ui.c` 的 `build_feed`（360 宽、y=120、flex row、22px 两端边距、8px 列距、横向滚动、无滚动条、`LV_SCROLL_SNAP_START`、去弹性），滚动贴靠回调 `on_wordcards_sets_strip_scroll_end`（`:557`）照抄 `on_feed_strip_scroll_end`（snap 本身不 clamp，`SCROLL_END` 时重发 `scroll_to_x` 拉回）。
- 每类一个 100×146 的列（item）：卡在上、两行名字在下，名字随卡一起滚。列是无 flex 的定高容器、卡与名字绝对定位——flex 一个 `pad_row` 表达不出 8px 与 1px 两种间距。146 = 100 卡 + 8 间隙 + 20 中文行 + 1 + 17 英文行（列内中文 y=108、英文 y=129，对应页 y=228/249）。
- 只对有 art 的类出卡（`muse_wordcards_set_has_art` 循环不动），`on_set_row` 回调不动；标题按 `" · "` 切分，中英文两行，没有分隔符时整串放中文行。
- 金币行：`s_sets_coins`（`:144`）+ `muse_wordcards_ui_set_coins()`（`:680`，声明在 `muse_wordcards_ui.h:76`），`refresh_status()` 两处分支（无快照/有快照）都接上，与 `s_learn_coins` 同款接线。
- 几何注释同步改写；滚动列的描述已删。

### 1.4 图标来源

各类的首词 art + 首词颜色，game-icons（CC BY 3.0，须署名）——与词卡一期同一批 art，不新增。

### 1.5 验证/残留

- host 测不到 LVGL 布局（tests 不编 `muse_wordcards_ui.c`），靠实机验收：四卡一横排、第四张露一角左滑可见、名字两行、金币行 y=302 与快照一致。
- v6 效果图（`wordcards-category-v6.py`）由用户随后画出后对照；v5 的四张 80×80 全显版已作废，不再实现。

## 2. S1：播放等待放弃分支不跑收尾循环

### 2.1 症状（评审 §S1）

`muse_wordcards_audio.c` 的 `fetch_task`：播放等待放弃分支（`s_play_task` 在 `WC_PLAY_MAX_WAIT_MS` 6000ms 后仍在）目前 single 模式只把 `single_slot` 标 FAILED 就退出，不跑主循环后那段收尾循环（single 时把全部非 READY 词标 FAILED）。被整组预取打断后留在 FETCHING 的其他词悬空——点按重试只接受 FAILED（`muse_wordcards_retry_failed_audio`），悬空词点按无反应，只能离页重进。

### 2.2 机制

- 放弃分支在 `muse_wordcards_audio.c:399-411`，正常退出的 single 收尾循环在 `:446-456`（`if (single)` 内）。
- Quiz 与单词共用同一个 `fetch_task`（`muse_wordcards_audio_fetch_one_quiz` 与 `fetch_one` 同路，都经 `muse_wordcards_audio_prefetch_list`），一次修好；评审称 quiz 侧对称存在同一缺口（`muse_quiz.c:228-242`、`muse_quiz_ui.c:334-363`），同路覆盖。
- 放弃分支的原注释称“其余词下次进组再试”——不准确：下次进组整组重取没错，但悬空词在本页内点按无反应才是症状。

### 2.3 修法

- 抽共享尾循环 `fail_unready_words()`（`:325-340`）：把列表里全部非 READY 词标 FAILED，READY 的词保留字节；代际过期时旧标记自失效。
- 放弃分支 single 模式改走它（`:402-408`），不再只标 `single_slot`；正常退出的 single 尾循环改用同一 helper（`:451`），去重。
- 放弃分支注释同步改写；整组（whole-list）模式的放弃分支**也**跑收尾（2026-11-17 用户拍板补修）：整组放弃后其余词同样停在 FETCHING、点按无反应、只能离页重进，是 S1 的同一症状；READY 保留字节，代际过期自失效。

### 2.4 验证/残留

- host 测不到：放弃分支依赖 `vTaskDelay` / `s_play_task`，`muse_wordcards_audio.c` 的任务体是 ESP-IDF only，host 台架编不到——**不硬造断言，靠实机验收**。复现路径：播一个词时快速点一张 FAILED 卡重试，等 6 秒后看被打断的整组预取留下的词是否可再点（应为 FAILED、可点 speaker 重试），而不是点按无反应。
- 残留：无。

## 3. S2：快速连点退化成逐词单取（修法：单取不 abort）

### 3.1 症状（评审 §S2）

快速连点多张 FAILED 卡退化成逐词单取、中间点按静默、总等待约 8 秒（上界 16 秒）。与用户笔记说法有两处出入（评审已纠正）：① 中间几次点按不是“等 1–4 秒”，而是根本不出声——它们的取词被后一次点按覆盖，永不执行；② 总等待不是 4×(1–4 秒)，而是约两段冷握手串联 ≈8 秒；若两次 perform 都撞上 8 秒超时，上界 16 秒。

### 3.2 机制（已核实）

`muse_wordcards_audio_prefetch_list()`：有取词任务在跑时，新请求一律置 `s_fetch_abort = true`。被 abort 的任务退出时释放整个 client（`muse_tts_release_client()`），下次单取必重新冷握手（冷 TLS 握手实测 3.5–4.3 秒）。连点 N 张 = N 次冷握手串行；且 `s_fetch_wanted` 只有一个槽，后一次点按覆盖前一次排队的单取。

评审给的最坏情况（4 张 FAILED 卡，t=0/0.5/1.0/1.5 秒各点一次）：t=0 点卡 1 启动冷握手；t=0.5 点卡 2（wanted=取 2，abort 置起，取 1 的 GET 仍在飞、不中断）；t=1.0 点卡 3——取 2 被覆盖，永不执行；t=1.5 点卡 4——取 3 被覆盖，永不执行；t≈4 秒取 1 的 GET 跑完，见 abort 返回 0，收尾循环把 4 个词全标 FAILED（含卡 1，字节已丢弃），释放 client，启动取 4；t≈4–8 秒取 4 重新冷握手 + GET，约 t=8 秒 READY 播出卡 4。

### 3.3 修法：选 (a) 单取不 abort

评审 §S2 给了两个修法方向，二选一：(a) 单取不 abort 整组预取；(b) 排队时不覆盖已排队的单取。本次选 **(a)**：

- 做法（`muse_wordcards_audio.c:489-508`）：**只有整组预取请求才 abort 正在跑的任务；单取请求只排队（置 `s_fetch_wanted` / `s_fetch_wanted_list`），不 abort**，等当前任务跑完由 `fetch_task_exit()` 启动排队的单取。
- 选 (a) 的理由：abort 的代价是释放整个 client——每次 abort 都白白丢掉一个温连接，下次单取必付一次冷握手；连点时这个代价被重复付 N 次，是“约 8 秒”的主因。不 abort 则整组预取跑完、连接留温，单取随后温连接跑（约零点几秒）。
- 不选 (b) 的理由：(b) 改动更小（把单值 wanted 槽改成队列），但只治覆盖、不治冷握手——整组预取仍会被第一个单取 abort 掉，冷握手仍在；且语义有问题：旧点按的词可能比新点按的词更不重要（用户已经翻走了），先到先得会让用户等旧词取完才取新词。(a) 治主因，覆盖只剩残留（§3.4）。
- `prefetch_abort()`（离页）与整组预取重入的 abort 语义原样保留；头文件 `muse_wordcards_audio.h:84-117` 的 `prefetch_list` / `prefetch_quiz` / `fetch_one` 注释同步改写。

### 3.4 残留限制：已排队的单取仍可能被覆盖

`s_fetch_wanted` 仍是单值槽：快速连点时，后一次单取覆盖前一次排队的单取。被覆盖的词不悬空——核对链如下：

- 点按重试先把词标 FETCHING（`muse_wordcards_retry_failed_audio`，`muse_wordcards.c:352-366`：仅 FAILED 可重标，一标记一次；`muse_quiz.c:237` 同款）。
- 前一任务退出时，single 尾循环把未 READY 的词标 FAILED：正常退出（`:446-456`）与播放等待放弃分支（`:402-408`，S1 修后）都跑。整组任务正常退出时不跑尾循环——但随后启动的排队单取是 single 模式，它的退出尾循环会把被覆盖的词标 FAILED。
- 离页时 `prefetch_abort()`（`:653`）清排队 + 置 abort，被覆盖的词随代际过期自失效，重进组整组重取。
- 所以被覆盖的词最终是 FAILED（不是 FETCHING 悬空），可点 speaker 或重翻再试（重试位在 `replay` / `flip_to_back` 时清零）。代价是它这次没取到，要再点一次——这是 (a) 留下的已知限制，不是悬空。

### 3.5 验证

- host 测不到：同 S1——任务调度 + TLS 客户端在 ESP-IDF 侧，host 台架编不到。**不硬造断言，靠实机验收**：快速连点三张 FAILED 卡，第一张约 3.5–4.3 秒（冷握手）出声、其余随后温连接（约零点几秒）出声、中间点按不再静默；被覆盖的词点按可再试。
- 数字口径：冷连接用评审纠正后的实测值 **3.5–4.3 秒**（2026-11-10 实测握手），不用旧笔记的“1–4 秒”（1 秒下限无依据、4 秒上限低于实测最大值，评审小问题 3）。

## 4. 小游戏列表页改横向卡片（B 方案，效果图 v3）

### 4.1 背景与三次改定

- 用户 2026-11-17：小游戏列表竖排改横向卡片（效果图 v1，同喂饭 strip）。
- 图标源四度改定：初版 game-icons 六图（剪刀/蛇/锤/金币/云/井字）→ 用户问能不能直接用游戏自身的画面，暂停等方向 → **B 方案游戏原画（v2）** → 两处修正（**v3**）：① 剪刀石头布只留一只布的手；② 贪吃蛇不要弯成毛毛虫，按游戏画法画直 → 两处修正（**v4**）：① 毛毛跳跃脚底下没有横板——v3 效果图用的旧预览图还带横板，改按 `.c` 里已擦掉横板的精灵（代码一直用的就是 `muse_doodle_sprite`，不用改）；② 金币雨不用金币、改用毛毛抱发财麻将牌。**不要再建 game-icons 的 `muse_minigame_art` 模块。**

### 4.2 几何（v1/v3，用户定）

- 标题“小游戏”不动（`page_header`），规则行“每局 10 金币”在 y=84（DIM，`muse_font_cjk()`）——六个游戏每局都是 10 金币。
- 六张 100×100 卡片 + 卡下名字（16px Noto DIM，y=228），顺序不变：石头剪刀布、贪吃蛇、打地鼠、金币雨、毛毛跳跃、OX棋；照抄喂饭页 `build_feed` 的 strip 写法（360 宽、128 高、y=120、左右 22px 边距、8px 列距、横向滚动、无滚动条、贴靠吸附、去掉弹性）；滚动贴靠回调 `on_minigame_strip_scroll_end` 照抄 feed。
- 金币行：`s_minigame_coins` 从滚动列里移出来，固定在 y=302（TEXT），`refresh_status()` 重写；`s_mg_*_lbl` 仍接各卡的名字 label，dim 接线（金币不足时名字变暗）保持可用。
- 各卡的 `on_open_*` 回调不动。

### 4.3 六个图标的来源（逐个核实）

| 卡 | 图标 | 来源与画法 |
|---|---|---|
| 石头剪刀布 | 一只布的手 | `muse_interact_hand_paper`（`muse_interact_hand_assets.h`）：game-icons john-redman/paper，**CC BY 3.0**，128×128 ARGB8888 白底透明母版、手指已朝上（生成时 rotate -90）。原样显示、不重着色；80px 槽居中（墨迹约 64px）。v2 是三只并排，v3 用户改定只留一只布。 |
| 贪吃蛇 | 直的 4 段方块蛇 + 头眼朝右 + 右上角红食物 | **LVGL 现画**，不用新图。游戏本体画法见 `muse_snake_ui.c`：段池是 18×18 圆角方块（半径 5、MUSE_THEME_OK 绿 #6ff0bf、无描边、20px 格距 `SN_CELL 20`、1px 内缩），头在端头、两只 6×6 白眼朝行进方向（RIGHT 时在 +13/+5 与 +13/+13 处），食物是 14×14 DANGER 红圆。卡片按同一 idiom 缩到 80px 槽：4 段 14×14（半径 3、16px 格距、x=8/24/40/56、y=40），头在最右一段、两只 3×3 白眼朝右（65,43)/(65,48)，右上角一颗 10px DANGER 红食物点（66,12)。**不要 S 形弯曲、不要圆段**（用户：“不是毛毛虫，画直一些”；参考 `snake-playing-v2.png` 进行中画面：横平竖直的方块蛇 + 红圆食物）。 |
| 打地鼠 | 鼹鼠头 | `muse_whackamole_mole`（`muse_whackamole_art.h`）：56×47 ARGB8888，Elecrow/Horion donor 素材、**用户已批准个人使用**（见该头文件注释），用在列表卡与游戏页本身同一语境，没有新问题。等比放进 80px 槽（80×67）。 |
| 金币雨 | 毛毛抱发财麻将牌 | `muse_goldrain_sprite`（`muse_goldrain_art.h`）：61×66，**金币雨游戏的接球手**，毛毛抱着发财麻将牌，用户上传。等比放进 80px 槽（74×80）。原定的四角星金币（`muse_goldrain_coin`，18×18 放大 4.4 倍会略软）不用了。 |
| 毛毛跳跃 | 飞天毛毛（脚底无横板） | `muse_doodle_sprite`（`muse_doodle_art.h`）：53×66 飞天造型，用户上传。等比放进 80px 槽（64×80）。**横板已擦掉**：2026-11-06（1.5.1）把脚底横板在像素数据上擦掉（63–65 行、鞋跨度外的亮像素），`.c` 数据就是无横板版；v3 效果图误用了带横板的旧预览图 `goldrain-sprite-preview.png`，v4 已按 `.c` 数据重画。 |
| OX棋 | 迷你 3×3 棋盘 + 红叉 + 绿圈 | **LVGL 现画**，不用新图。游戏自己的标记色（`muse_tictactoe_ui.c`：X 是两根 `lv_line`、O 是带边框的环；X DANGER 红、O OK 绿）：20px 格、2px DIM 线，左上红叉（3px 线）、右下绿圈（10px 环、3px 边）。 |

### 4.4 接线

- 四处图源（布手、鼹鼠、麻将毛毛、飞天毛毛）直接复用已链接的量：`CMakeLists.txt` 已列 `muse_whackamole_art.c` / `muse_goldrain_sprite.c` / `muse_doodle_sprite.c` / `muse_interact_hand_*.c`，**无新增、无改动**。
- 蛇与棋盘 LVGL 现画，不搞生成器、不新增 flash 块：`minigame_card()` + `minigame_icon(kind)`（`muse_interact_ui.c:1777` 起）；`mg_icon_image()` 按长边等比进槽；`mg_slot()` 是透明 80×80 坐标空间；`mg_line()` 的点数组用静态池（LVGL line 不拷贝点数组，蛇页/ OX 页同款做法，池子 6 条、每次建板前归零）。
- `list_row()` 已删（仅 `build_minigame` 用，留着会触发 `-Werror` 未用静态函数）；`IC_MG_ROW_SPACE` / `IC_MG_LIST_PAD_BOTTOM` 两个旧宏已删。
- 数独已归档的注释与恢复说明原样保留，恢复步骤改写为新版式（“加一个 `minigame_card()` 调用”，不再是“取消注释一行 `list_row`”）。

### 4.5 验证/残留

- host 测不到 LVGL 布局（tests 不编 `muse_interact_ui.c`），靠实机验收：六张卡的图标各是其游戏的原画、横向滚动贴靠、金币行固定 y=302。
- 已知：金币 18→80 放大略软（§4.3）；蛇段与眼按 v3 效果图取 14px/3px（游戏本体是 18px/6px，见 §4.3 的缩放对照）。

## 5. 验证

- Host 全量：`cd esp32 && python3 -m unittest discover -s tests -p 'test_*.py'`。基线 636 个（失败 27、错误 8、跳过 10），失败清单 `~/workspace/jiaobei-shake-after-failures.txt`。验收：失败清单与基线逐行相同、零新增；日志存 `~/workspace/hcards-after.log`，失败清单存 `~/workspace/hcards-after-failures.txt` 并 diff。
- **结果（2026-11-17 跑完）：Ran 636 tests，failures=27、errors=8、skipped=10——与基线数字完全一致；失败清单 35 行（27 FAIL + 8 ERROR）与基线逐行 diff 为空，零新增。**（注：三处旧宏 `IC_LIST_W` / `IC_LIST_TOP` / `IC_ROW_H` 的删除在测试跑完后做——无 host 测试编译 `muse_interact_ui.c`，不影响结果。）
- S1/S2 的放弃分支与排队语义 host 测不到（§2.4/§3.5），两处横向卡片的布局 host 测不到（§1.5/§4.5）——均靠实机验收，不硬造断言。
- 不宣称编译通过（本机无 ESP-IDF 工具链）。

## 6. 用户侧剩余待办

- 编译：`tools/muse/board.sh build lcd185b`（IDF 6.0.3，用户本机）。
- 实机验收：
  1. 单词分类页：四卡一横排、第四张露一角左滑可见、名字两行、金币行 y=302 与快照一致；
  2. 小游戏页：六张卡的图标各是其游戏的原画（布手一只、直蛇、鼹鼠、金币、飞天毛毛、迷你棋盘）、横向滚动贴靠、金币行固定；
  3. S1：播一个词时快速点 FAILED 卡重试，等 6 秒后被打断的整组预取留下的词可再点（FAILED、可点 speaker 重试），而不是点按无反应；
  4. S2：快速连点三张 FAILED 卡，第一张约 3.5–4.3 秒出声、其余随后温连接出声、中间点按不再静默；被覆盖的词点按可再试；
  5. 观感：金币放大是否可接受、蛇直排与纸手是否顺眼。
- 合 main、推送均由用户定；推送时审批卡 30 分钟过期。
