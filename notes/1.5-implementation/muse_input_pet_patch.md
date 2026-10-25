# muse_input.c 串口命令补丁说明（1.5a 第 1 阶段，尚未应用到仓库）

> 只改 `esp32/components/muse/muse_input.c` 的 `console_command()`。
> 命令名不与既有命令撞名：既有命令是 `status` / `power` / `power.reset` /
> `telemetry*` / `alarm*` / `ota` / `time` / `batt` / `tp` / `face=` / `chat*`，
> `pet.*` 前缀与它们都不重叠（`power` 是整行精确匹配，`pmlow=` 是另一前缀）。

## 0. 形态先说清（已在 1.4d 代码上核实）

- 用户在串口敲的是 `>pet.status`，但 `>` 是行前缀：`serial_task()` 剥掉
  `>` 后才调 `console_command(line, ...)`，所以代码里匹配的是
  `"pet.status"`，不是 `">pet.status"`。
- 输出风格照既有 `@power` / `@status` 的一行键值式，不另发明 JSON 格式，
  每条输出后 `fflush(stdout)`（既有命令都这么做）。

## 1. 头文件

在 `muse_input.c` 的 `muse_` include 块里加两行：

```c
#include "muse_pet.h"
#include "muse_pet_text.h"
```

## 2. 放置位置（锚点，不要凭行号贴）

放在 `face=` 块之后、`chat` 检查之前。锚点原文（分支
`docs/1.5-interaction-preview` 上的 `console_command()` 尾部）：

```c
    if (!strncmp(line, "face=", 5)) {
        set_face(line + 5);
        return true;
    }
    if (strncmp(line, "chat", 4) != 0) {
        return false;
    }
```

把下面的 pet 块插在 `face=` 块与 `chat` 检查之间。放错位置的后果：
放在 `chat` 检查之后，`pet.*` 永远到不了（它不以 `chat` 开头，
会被 `return false` 拦下）。

## 3. 辅助函数（放在 `console_command()` 之前，与 `set_face()` 并列）

```c
static const char *pet_result_name(muse_pet_result_t r)
{
    switch (r) {
    case PET_OK: return "OK";
    case PET_COIN: return "COIN";
    case PET_CAP: return "CAP";
    case PET_POOR: return "POOR";
    case PET_FORTUNE_NEW: return "FORTUNE_NEW";
    case PET_FORTUNE_SAME: return "FORTUNE_SAME";
    case PET_STORAGE: return "STORAGE";
    case PET_TIME: return "TIME";
    default: return "BUSY";
    }
}

/* Wait up to ~2 s for the feedback of one action. Feedback for other actions
 * is pushed back to the front so the console loop never swallows feedback
 * meant for the UI (see muse_pet_feedback_requeue()). */
static bool pet_console_wait(muse_pet_action_t action, muse_pet_feedback_t *out)
{
    for (int i = 0; i < 200; i++) {
        muse_pet_feedback_t fb;
        if (muse_pet_feedback(&fb)) {
            if (fb.action == action) {
                *out = fb;
                return true;
            }
            if (!muse_pet_feedback_requeue(&fb)) {
                printf("@pet feedback dropped\n");
                fflush(stdout);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static void pet_console_snapshot(void)
{
    muse_pet_model_t s;
    if (!muse_pet_snapshot(&s)) {
        printf("@pet snapshot unavailable ready=%d storage_error=%d\n",
               (int)muse_pet_ready(), (int)muse_pet_storage_error());
        fflush(stdout);
        return;
    }
    printf("@pet coins=%u touch=%u feed=%u fortune_day=%d fortune=%u "
           "decay_day=%d decay_notice=%u\n",
           (unsigned)s.coins, (unsigned)s.daily_touch_coins,
           (unsigned)s.daily_feed_coins, (int)s.fortune_day,
           (unsigned)s.fortune, (int)s.decay_day, (unsigned)s.decay_notice);
    fflush(stdout);
}

/* Send one action and print its result + snapshot. Returns false on
 * not-ready / queue-full / timeout, so the caller prints nothing further. */
static bool pet_console_do(muse_pet_action_t action, const char *what)
{
    if (!muse_pet_request(action)) {
        printf("@pet %s error not-ready storage_error=%d\n", what,
               (int)muse_pet_storage_error());
        fflush(stdout);
        return false;
    }
    muse_pet_feedback_t fb;
    if (!pet_console_wait(action, &fb)) {
        printf("@pet %s error timeout\n", what);
        fflush(stdout);
        return false;
    }
    printf("@pet %s result=%s\n", what, pet_result_name(fb.result));
    fflush(stdout);
    pet_console_snapshot();
    return true;
}
```

## 4. 命令块（插在锚点位置）

```c
    if (!strncmp(line, "pet.", 4)) {
        if (!strcmp(line, "pet.status")) {
            /* Read-only, always compiled (design §3.3: only pet.reset is
             * Test-gated). Never invents numbers: without a snapshot it
             * prints ready/storage_error and nothing else. */
            if (!muse_pet_ready()) {
                printf("@pet status ready=0 storage_error=%d\n",
                       (int)muse_pet_storage_error());
            } else {
                pet_console_snapshot();
            }
            fflush(stdout);
            return true;
        }
        if (!strcmp(line, "pet.touch")) {
            pet_console_do(PET_TOUCH, "touch");
            return true;
        }
        if (!strcmp(line, "pet.feed")) {
            /* Console default food is rice; the three feed actions settle
             * identically, so this verifies feed settlement, not a food. */
            pet_console_do(PET_FEED_RICE, "feed");
            return true;
        }
        if (!strcmp(line, "pet.fortune")) {
            if (pet_console_do(PET_FORTUNE, "fortune")) {
                muse_pet_model_t s;
                if (muse_pet_snapshot(&s) && s.fortune_day >= 0) {
                    const char *g = muse_pet_fortune_grade(s.fortune);
                    const char *t = muse_pet_fortune_text(s.fortune);
                    printf("@pet fortune grade=%s text=%s\n",
                           g ? g : "?", t ? t : "?");
                    fflush(stdout);
                }
            }
            return true;
        }
        if (!strcmp(line, "pet.game")) {
            /* Verifies the 5-coin deduction only. Win/loss randomness is
             * phase-2 UI-side (design §5.2); the model carries no game
             * random, so there is no result to print here beyond OK/POOR. */
            pet_console_do(PET_GAME, "game");
            return true;
        }
#if CONFIG_MUSE_BUILD_TEST
        if (!strcmp(line, "pet.reset")) {
            /* Destructive, Test only (design §3.3). Two paths:
             *  - worker running: PET_RESET through the queue (race-free,
             *    the worker is the sole NVS writer);
             *  - worker dead (e.g. corrupt blob): erase directly, then a
             *    reboot rebuilds fresh state (the worker re-inits on
             *    NOT_FOUND). Never erase behind a live worker. */
            if (muse_pet_ready()) {
                muse_pet_feedback_t fb;
                if (muse_pet_request(PET_RESET) &&
                    pet_console_wait(PET_RESET, &fb) && fb.result == PET_OK) {
                    printf("@pet reset ok\n");
                } else {
                    printf("@pet reset error\n");
                }
            } else if (muse_pet_erase_storage()) {
                printf("@pet reset erased, reboot to rebuild\n");
            } else {
                printf("@pet reset erase failed\n");
            }
            fflush(stdout);
            return true;
        }
#endif
        /* In a Release build, pet.reset falls through to here (the #if block
         * above is compiled out) instead of silently doing nothing. */
        printf("@pet unknown command\n");
        fflush(stdout);
        return true;
    }
```

## 5. 门控说明（与设计 §3.3 对照）

- `pet.status`：常驻，不包 Test 宏。
- `pet.touch` / `pet.feed` / `pet.fortune` / `pet.game`：验证用，不包 Test 宏。
  注意它们不是无副作用的“只读”：touch / feed 加币、game 扣币，bench 时
  用它们就是在改真实金币——这是既有 bench 命令的惯例（`power.reset`、
  `alarm.test` 同样不包 Test 宏且改状态），不是本补丁新开的口子。
- `pet.reset`：只包在 `#if CONFIG_MUSE_BUILD_TEST` 里。设计 §3.3 说
  “进 Release 前必须已被编译期排除，靠构建验证”——但实测
  `muse_build.h` 目前只对 `CONFIG_MUSE_POWER_TEST` /
  `CONFIG_MUSE_TELEMETRY` 做 Release `#error`，没有针对 pet.reset 的校验项
  （见 PHASE1-NOTES.md §7，第 1 条）。本补丁只做到宏门控，不擅自改
  `muse_build.h`；合入时由主 agent / 用户决定是否补校验项。
