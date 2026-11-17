# 单词卡片 TTS“重启后首次点按静默”修复

- 症状上报：2026-11-17，用户实测
- 基点：main `cfe1b30`（1.5.3c）
- 分支：`feat/fix-wordcards-tts-20261117`
- 选定方案：② FAILED 的词点按时单取一次（一次重试/点按）
- 状态：代码已做完并本地提交；host 全量 636 个（失败 27、错误 8、跳过 10），失败清单与基线逐行相同、零新增；未编译验证（本机无 ESP-IDF 工具链），待用户编译与实机验收

## 1. 症状

重启后 Muse 正常连接，进单词卡片点卡片翻面，TTS 不出声；跟毛毛对话一轮后，再点单词卡片 TTS 就正常了。听力练习（英语口语）与单词卡片共用同一套取词/播放机制，同款症状、同款修法（差异见 §5.4）。

## 2. 机制（逐行核过代码，`cfe1b30`）

1. **点按从不现取音频**。`esp32/components/muse/muse_wordcards_ui.c:37-45` 注释写明：“the UI never fetches on a tap”；50 ms tick（`drain_pending_audio()`，同文件 :319-336）只处理已有的 `pending_audio` 标记：READY 就播、FETCHING 就等下一 tick、**FAILED 就静默丢弃**，卡片不出声。听力练习同款：`muse_quiz_ui.c:324-345`。
2. **预取只在进组时跑**。进组 `muse_wordcards_ui_show()`（:739-751）调 `muse_wordcards_audio_prefetch()`；开始页 `begin_game()`（:306）也调一次。失败只标 FAILED，**重试只能靠离页重进**（回组再调一次 prefetch；`muse_wordcards.c:349-366` 的 `cache_begin()` 会把非 READY 的词重新标 FETCHING）。
3. **预取有内存门**。`muse_wordcards_audio.c:78`：`WC_MIN_INTERNAL_FOR_TLS 4608u`；`fetch_task()`（:349-366）内，内部最大空闲块 < 4608B 则整组跳过、未 READY 的词全标 FAILED，并打日志 `prefetch skipped: internal … largest … (floor 4608)`。门上方的注释记了实测数：IDF 6.0.3 下本板开机后内部最大块 7.68 KB、Link 连上后约 5 KB；2026-11-11 重连时实测跌到 4 KB 最大块，并因此撞出过 `Guru Meditation (StoreProhibited)`（LVGL 渲染取不到 mask 缓冲）。门是防崩机的，不能绕。
4. **预热只在语音回合调**。`muse_tts_prewarm()`（`muse_tts.c:873`，探针任务 :833）只在语音回合开始时调（`components/muse/muse_chat_session.cpp:1084`，turn_start）；单词/听力从不调。首取要自己付 3.5–4.3 秒冷 TLS 握手（2026-11-10 实测）；单取超时 `WC_HTTP_TIMEOUT_MS 8000`（`muse_wordcards_audio.c:38`），`fetch_one()`（:257-300）传输失败重试一次，再失败标 FAILED，**传输失败还会放弃本组剩余词**（`fetch_task()` 的 `r < 0` 分支）。
5. **对话一轮“修好”是偶然的**：预热把共享 TLS 连接建好并留温（`muse_tts.c:132` 注释：卡片从不调 prewarm，复用靠 `ensure_client`），加上回组重进重新预取——两件事凑巧一起发生。不是初始化被“修好”了，是第二次预取时条件变好了。

**判别办法**（重启后串口日志，二选一）：出现 `prefetch skipped` → 内存门（原因 3）；出现 `word …: fetch failed` → 冷握手超时（原因 4）。两种都指向同一个修复：点按时给 FAILED 的词一次单取机会（§5）。

## 3. 三个修法选项的利弊

### ① 进组时预热一次

- 做法：进单词/听力组时调一次 `muse_tts_prewarm()`，先把 TLS 连接握好再预取。
- 利：冷握手这半边原因直接消失，首取走温连接（服务器应答约 18 ms）。
- 弊（不选的理由，两条都是代码级硬伤）：
  1. **治不了内存门**。预热本身也要开 TLS 客户端（`ensure_client()`），而内存门只拦在 `fetch_task()` 里，预热不走那道门。堆紧张时进组预热就是 2026-11-11 撞崩机的同款操作——只不过把崩机点从预取挪到了预热。
  2. **会撞坏本来能成的取词**。`muse_tts_fetch_raw()`（`muse_tts.c:444`）在 prewarm 进行中直接返回 -1；预取进组立即开始，3.5–4.3 秒的探针期间第一次取词必被拒，`fetch_one()` 只重试一次，两次都撞上探针就标 FAILED。想避开得给预取加“等探针结束”的排序，改动面比修本身还大。
- 结论：不做。以后若日志证明冷握手是主因，可再议“预热 + 预取等探针”的组合，但不是本次。

### ② FAILED 的词点按时单取一次（本次选定）

- 做法：点按 FAILED 的词时，把它重新标为 FETCHING 并触发一次单取词；已有的 50 ms tick 会等 FETCHING 的词、READY 了立刻播——复用这条现成路径。
- 利：
  1. 两种病因都治：点按时堆已稳、连接可重试，内存门和冷握手任一原因导致的首次失败，第二次都有条件成功。
  2. 只取用户真正点的词，不烧多余流量与电；不碰预热、不碰内存门、不改预取时序。
  3. 改动面小：逻辑层一个重标函数 + 音频层一个单取入口 + 两个 drain 分支。
- 弊（已在设计里堵住）：
  1. 动了“点按从不现取音频”这条特意立的规矩——本次只开这一个例外，代码注释与本文都写明原因（§5.5）。
  2. 单取必须排队、不能抢锁、不能绕门——靠复用 `s_fetch_wanted` 排队机制与 `fetch_task()` 内原样保留的门控解决（§5.2）。
  3. 点按后要等一次取词（冷连接约 1–4 秒，温连接约零点几秒）才出声，不如预取命中时“翻面即播”快——这是单取的固有等待，实机验收要看观感（§6）。

### ③ 失败后延时自动重试

- 做法：预取失败后起个定时器，过几秒自动再取一次。
- 利：用户什么都不用做，卡片自己变好。
- 弊（不选的理由）：盲 retry——时机靠猜（太早还是同一堆/同一连接，太晚用户已经点了静默卡走了）；用户不点的词也白白烧流量与电；还要给重试加代际守卫（页都离了、轮次都换了，重试不能再写旧槽）。收益不如②直接，成本却更高。
- 结论：不做。

## 4. 选定②的理由

- 对症：用户报的是“点了没声”，②在用户点的那一刻给一次补救，是唯一直接修症状的选项。
- 覆盖两种病因：不管首次失败是门还是握手，点按时的条件都比进组时好。
- 风险最小：不碰 TLS 客户端的创建时序（①的风险）、不引入后台定时器（③的成本）；内存门与连接锁的现有防护原样保留。
- 可验证：host 层能测逻辑重标与一次重试守卫；实机只需看重启后首次点按是否出声 + 串口日志。

## 5. 实施计划

### 5.1 逻辑层：一次重试守卫（`muse_wordcards.c` / `muse_quiz.c`，可 host 测试）

- `muse_wordcards_t` 加 `bool pending_audio_retried`；`muse_quiz_t` 加 `bool audio_pending_retried`。含义：当前这个 pending 标记已经用掉它的一次点按重试。
- 新函数 `muse_wordcards_retry_failed_audio(g, i)` / `muse_quiz_retry_failed_audio(g, i)`：仅当 `i` 是有效词槽、该槽为 FAILED、且本标记还没重试过时，把槽重新标为 FETCHING、置重试位、返回 true；其余一律返回 false。
- 重试位在每次新标记设立时清零：单词卡片是 `flip_to_back()` / `replay()`（设 `pending_audio` 处）与 `reset_round()`；听力是 `reset_round()`、`tick()` 切到下一题、`replay()`（设 `audio_pending` 处）与 `init()`。于是每翻一次卡 / 每按一次喇叭，最多一次重试；失败再点喇叭可再试一次；绝不出现 50 ms tick 驱动的无限重试循环。

### 5.2 音频层：单取入口（`muse_wordcards_audio.{h,c}`，ESP-IDF only）

- `muse_wc_fetch_list_t` 加 `bool single` + `int single_slot` 两个字段：`single` 为真表示只取 `single_slot` 槽，`single` 为假表示整组。之所以不用单个 `int only`（`>= 0` 只取该槽、`-1` 整组）：列表都用 `{0}` 初始化，`only` 字段的默认值 0 会被误解为“只取 0 号槽”；`single` 为假才是 `{0}` 的安全默认值。`single` 为真但 `single_slot` 越界时当整组取（防御性回落，不静默丢单）。
- 新入口 `muse_wordcards_audio_fetch_one(g, i)` / `muse_wordcards_audio_fetch_one_quiz(g, i)`：按现有 prefetch 原样搭整组列表（同样的槽池、同样的 mark 回调、同样的代际），只把 `single` 置真、`single_slot` 置为 `i`，然后走 `muse_wordcards_audio_prefetch_list()`。列表搭建抽成 `wc_list_for()` / `quiz_list_for()` 两个小函数，prefetch 与 fetch_one 共用，行为与原来逐行填字段一致。
- 排队：复用已有的 `s_fetch_wanted` 机制——取词任务进行中，单取排在后面等它退出再跑；绝不直接碰 `s_client_lock`（锁只在 `muse_tts_fetch_raw()` 内拿）。
- 门控：`fetch_task()` 里的 `WC_MIN_INTERNAL_FOR_TLS` 门控原样保留，单取同样先过门；门槛不够时仍静默失败、不崩机。`ensure_slots` 失败与内存门失败的循环保持全范围（不取就全标 FAILED，与现状一致）。
- `fetch_task()` 按 `single` 收敛：
  - `single` 时跳过 `mark_begin()`（调用方已把该词重标 FETCHING；整组重标会把别的 FAILED 词也标成 FETCHING 却没人取它们）；
  - 主循环只取 `single_slot` 槽；传输失败只标该槽 FAILED；
  - 任务结束时（仅 `single` 模式）把列表里仍为 FETCHING 的词标 FAILED——被单取打断的整组预取留下的未取词不能悬在 FETCHING 里没人管，否则点它们会永远等下去；标 FAILED 后，下次点按可再单取、离页重进可整组重取；
  - 播放中放弃那条路（`prefetch abandoned`）：`single` 模式下把该槽标 FAILED 再退出，同样不留悬空 FETCHING；
  - `single_slot` 越界时当整组取（防御性回落，不静默丢单）。
- 单取入口返回 bool：取词任务已启动或已排队返回 true；任务建不起来（`xTaskCreateWithCaps` 失败）返回 false，drain 见 false 时把词标回 FAILED 并丢标记（见 5.3），不等一辈子。
- 不加进组预热（选项①），不加延时重试（选项③）。

### 5.3 UI 层：drain 分支（`muse_wordcards_ui.c` / `muse_quiz_ui.c`）

- `drain_pending_audio()` 的 FAILED 分支改成：先调逻辑层重试函数；返回 true 再调音频层单取入口——单取接受（true）则**保留标记**，tick 继续等 FETCHING → READY 后播；单取没接住（false）则把词标回 FAILED 并丢标记；重试函数返回 false（本标记已试过）则按现状静默丢标记。
- 其余分支不动：READY 播、FETCHING 等。
- 两个 drain 的注释、“点按从不现取音频”的头注释（`muse_wordcards_ui.c:37-45`、`muse_wordcards.h` 的对应段落、`muse_quiz_ui.c:324-330`）同步改写，写明这是一次有意的例外与原因。

### 5.4 听力练习的差异

- 共用 `muse_wordcards_audio.c` 的取词/播放机制与同一个 `fetch_task`、`s_fetch_wanted` 排队、内存门、连接锁——同款修法一并覆盖，不另起机制。
- 不同点只有三处：pending 标记是 bool（`audio_pending`）不是下标，当前词槽是 `g->q`；发音在发题与切题时自动排队（单词卡片只在翻面/按喇叭时排队）；一局 60 秒有时钟——drain 只在 PLAYING 跑，代际守卫（`quiz_gen`）与播放结束重查 READY 保证过期重试不会在 OVER 后播出来。

### 5.5 “点按从不现取音频”规矩的例外说明

- 原规矩立于 2026-11-10：翻面即播、绝不等网络，是单词卡片“进组即玩”的体验底线。
- 本次只开一个例外：**点按 FAILED 的词**。READY 的词仍走缓存即播；FETCHING 的词仍等 tick；只有 FAILED 的词（否则永远静默）才在点按时单取一次，且每标记一次。这不是把点按改成现取，而是给失败留一条点按触发的活路——预取仍是主路径，单取只是它的补救。
- 例外写进三处：本文件 §5.3、两个 UI 文件的头注释、逻辑层重试函数的注释。

### 5.6 不动的部分

- 不动预热调用点（`muse_chat_session.cpp:1084` 仍是唯一调用方）、不动 `WC_MIN_INTERNAL_FOR_TLS` 门槛值、不动 `WC_HTTP_TIMEOUT_MS`、不动 `fetch_one()` 的重试次数、不动整组预取的触发时机、不动 `muse_tts.c` 的锁与共享连接逻辑、不重构无关代码。

## 6. 验证计划

- Host 全量：`esp32/` 下 `python3 -m unittest discover -s tests -p 'test_*.py'`。基线 636 个（失败 27、错误 8、跳过 10），失败清单 `~/workspace/jiaobei-shake-after-failures.txt`。验收：失败清单与基线逐行相同、零新增；日志存 `~/workspace/fix-wordcards-tts-after.log`，失败清单存 `~/workspace/fix-wordcards-tts-after-failures.txt` 并 diff。
- 新增 host 测试（逻辑层，`wordcards_harness.c` / `quiz_harness.c`）：
  - FAILED 的词点按重试：重标为 FETCHING 并返回 true；同一标记再试返回 false；换新标记（翻面/按喇叭/切题）后可再试；
  - 非 FAILED（READY / FETCHING / EMPTY）与越界下标一律返回 false，不改状态；
  - 世代：重启/换组后重试位与缓存状态按现有规则走。
- 排队不抢锁、门控仍生效：音频层是 ESP-IDF only，host 测不到——改由实机验收（串口日志看单取是否在预取任务退出后跑、门槛不够时是否仍打 `prefetch skipped` 且不崩机）。
- 不宣称编译通过（本机无 ESP-IDF 工具链）。

## 7. 用户侧剩余待办

- 编译：`tools/muse/board.sh build lcd185b`（IDF 6.0.3，用户本机）。
- 实机验收：
  1. 重启后首次进组点卡片是否出声（主场景）；
  2. 串口日志判别：`prefetch skipped`（内存门）还是 `fetch failed`（冷握手超时）；
  3. FAILED 点按单取的等待观感：冷连接约 1–4 秒、温连接约零点几秒，是否可接受；
  4. 听力练习同款场景：重启后首次进组，首题是否出声；
  5. 快速连点多张 FAILED 卡：不应崩机、不应丢已排队的取词之外的状态（其余 FAILED 词点按可再单取、离页重进整组重取）。
- 合 main、推送均由用户定；推送时审批卡 30 分钟过期。
