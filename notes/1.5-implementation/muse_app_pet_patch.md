# muse_app.c 补丁说明（1.5a 第 1 阶段，尚未应用到仓库）

> 只改 `esp32/components/muse/muse_app.c` 两处。不要动启动顺序的其他部分，
> 不要把 worker 放到 UI / 语音之后，也不要用 `ESP_ERROR_CHECK` 包它：
> worker 启动失败只记日志、不阻塞 UI / 语音（设计 §2.5）。

## 1. 头文件

在 `muse_app.c` 的 `muse_` include 块里加一行：

```c
#include "muse_pet.h"
```

## 2. 插入点（已在分支 `docs/1.5-interaction-preview` 上核实）

`muse_app_run()` 的实测顺序是：

```text
board->init() -> muse_settings_init() -> muse_state_init()
  -> muse_battery_init() -> muse_time_start()
  -> [Test] muse_telemetry_start()
  -> muse_ui_start() -> muse_input_start(q) -> muse_voice_start(q)
```

插入位置：`muse_time_start();` 紧后、`#if CONFIG_MUSE_TELEMETRY` 之前，
即时间已 adopt、NVS 已初始化、UI 与语音尚未开始（设计 §2.5）：

```c
    muse_time_start();
    /* 1.5a pet worker: after the clock is adopted, before UI/voice start.
     * muse_pet_start() never blocks and never aborts the app: on failure it
     * only logs, and muse_pet_request() then returns false. */
    muse_pet_start();
#if CONFIG_MUSE_TELEMETRY
```

## 3. 不做的事

- 不改 `muse_settings_init()`、`muse_state_init()`、`muse_ui_start()`、
  `muse_voice_start()` 的调用与顺序。
- 不新增 Kconfig 选项（设计 §3.2：1.5a 不需要宠物开关）。
- 不在这里接脸点按或互动 tile（那是第 2 阶段）。
