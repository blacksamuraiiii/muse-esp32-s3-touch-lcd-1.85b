# CMakeLists.txt 改动说明（1.5a 第 1 + 2 阶段，尚未应用到仓库）

## 第 2 阶段补充（互动 UI 文件）

在第 1 阶段的 3 个 `.c` 之后再加 1 个：

```cmake
        "muse_settings.c"
        "muse_pet_model.c"
        "muse_pet.c"
        "muse_pet_text.c"
        "muse_interact_ui.c"
        "muse_settings_ui.c"
```

- `muse_interact_food_art.h` 是纯头文件数据（食物像素格），
  由 `muse_interact_ui.c` include，不列进 `srcs`。
- `muse_interact_hand_assets.h` 在第 2 阶段是**模板占位**：
  它含 `#error`，故意阻止游戏子页在手图生成前编译通过。
  生成手图后（见该文件头的 5 步），把它替换成生成的描述符文件，
  符号名不变（`muse_interact_hand_rock/paper/scissors`），
  不用再改 CMakeLists。
- 第 2 阶段后 `srcs` 新增 `.c` 总计 4 个（模型、worker、文本、互动 UI），
  与设计 §3.2 的“4 个新文件”数字巧合一致，但含义不同：
  设计原意是模型 + worker + 文本 + 互动 UI 四组文件（每组 .h/.c），
  实际新增文件为 4 个 `.c` + 5 个 `.h`
  （模型、文本、worker、互动 UI、食物图；手图资产文件另计，
  合入时由生成文件替换模板）。

---

# CMakeLists.txt 改动说明（1.5a 第 1 阶段，尚未应用到仓库）

> 只改 `esp32/components/muse/CMakeLists.txt` 一处。不要把宠物文件塞进
> 板级条件分支，也不要包进 Test / Telemetry / Power-test 的条件块：
> 宠物核心与构建类型无关（设计 §3.3），1.5a 的 Test 与未来 Release 1.5
> 用同一份宠物代码。

## 位置（已在分支 `docs/1.5-interaction-preview` 上核实）

文件开头的第一个 `list(APPEND srcs ...)`，即 `if(CONFIG_MUSE_ENABLED)` 下的
共享核心段（不是后面“Shared core, independent of board selection”那段，
那段只放 `muse_power_test.c` / `muse_telemetry.c` / `muse_powcfg.c`）。
锚点原文：

```cmake
        "muse_settings.c"
        "muse_settings_ui.c"
```

改成：

```cmake
        "muse_settings.c"
        "muse_pet_model.c"
        "muse_pet.c"
        "muse_pet_text.c"
        "muse_settings_ui.c"
```

即：放在 `muse_settings.c` 之后、`muse_settings_ui.c` 之前，
与设计 §3.2 说的“位置照现有 `muse_state.c` / `muse_settings.c` 一段”一致。

## 计数说明（设计 §3.2 的“4 个新文件”与实际的出入）

- `srcs` 列表实际新增 3 个 `.c` 文件（模型、worker、文本）。
- `.h` 文件不用列进 `srcs`：`idf_component_register(INCLUDE_DIRS ".")`
  已覆盖本目录，头文件靠 include 找到。
- 连同 3 个 `.h`，本次新增文件共 6 个。设计 §3.2 的“加 4 个新文件
  （模型 + worker；文本文件若并入则为 2 个）”按字面数不拢（模型 .h/.c +
  worker .h/.c 已是 4 个，文本再加 2 个是 6 个），以本说明的 3 个 `.c`
  为准，不按“4”去凑数。

## 不改动的部分

- 不新增 Kconfig 选项，不新增 `PRIV_REQUIRES`：`nvs_flash`、`esp_timer`
  已在 `muse_priv_requires` 里，`esp_random.h` 属 ESP-IDF 核心组件
  （`esp_system`），首次编译时若报缺头文件再补，不要预先加。
- 不动板级 `elseif(CONFIG_MUSE_BOARD_...)` 分支：1.85B 走
  `board_waveshare_s3_touch_lcd_185b.c` 那支，与宠物文件无关。
- 不动 `muse_build.h`：见 `muse_input_pet_patch.md` §5，
  pet.reset 的 Release 校验项缺失问题留给合入时定夺，本阶段不擅自改。
