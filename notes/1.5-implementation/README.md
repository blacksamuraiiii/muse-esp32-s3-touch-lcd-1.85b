# 1.5a 互动预览版实现（第 1–3 阶段本地稿，推送版）

> 状态：**未编译、未合入、未实测**。本分支上的代码是本地成稿的起点，
> 不是可直接刷机的版本。合入前必须按 PHASE3-NOTES.md 的顺序走完：
> 应用补丁 → 生成手图 → 编译 → 串口 → 过屏 → 回归 → 夜测 → pre-release。

## 已拍板的口径（2026-10-25）

- 版本：`1.5a` + Test 构建，USB 刷入、GitHub pre-release；`ota/latest.json` 不动；
  刷机前备份 1.4d 整颗 Flash。不要发明 `1.5-preview`，不要直接发 Release `1.5`。
- 金币经济：只剩金币。摸摸 +5（前 10 次）、喂饭 +10（每天第一次，三种食物
  结算相同）、小游戏 -5（不够 5 不开局、赢平输不返币）、生活费每天 -10；
  饱腹 / 心情 / 能量全部删除，版本号 2（与 TamaMe v1 布局不兼容）。
- 时钟：直接用开发板上的 UTC，不走 localtime_r + TZ 换算；
  UTC 日边界在北京时间 08:00，以后想改本地午夜只改
  `MUSE_PET_DAY_OFFSET_SECONDS` 一个常量（现为 0，改 8×3600 即本地午夜）。
  宠物代码不另做时钟校准，直接用 `muse_time_start()` 已提供的内置 UTC。
- 字体：**运势正文与小字做到 16 号字体就够了**。1.4 的 CJK 字体是
  `muse_font_cjk_16`（GNU Unifont 16.0.04，16×16 位图），仓库无 18 号 CJK
  字体；真要 18px 须另生成字体文件并实测 Flash 增量，用户未决定，不做。
- 占卜卡：不用卡名、只显示等级（“今日运势：<等级>”）；内容自写、
  不上网抄现成文案；等级只用大吉 / 中吉 / 小吉 / 吉，无凶卡。
- UI 红线：用户可见文案不写“宠物”/PET；返回全部左侧 `<`；互动 tile
  标题中文“互动”；不引入 IMU 手势、触摸唤醒、常驻扫描、高频轮询。

## 本次推送的文件

- `esp32/components/muse/` 下 10 个新源文件：模型（`muse_pet_model.*`）、
  文本表（`muse_pet_text.*`，20 张自写运势卡 + 米饭 / 蛋糕 / 咖啡）、
  worker（`muse_pet.*`）、互动 UI（`muse_interact_ui.*`）、食物像素格
  （`muse_interact_food_art.h`）、手图模板（`muse_interact_hand_assets.h`）。
- 本目录下：`PHASE1-NOTES.md`（模型 / worker / 串口）、
  `PHASE2-NOTES.md`（互动 tile / 三个子页 / 脸点按 / 字幕）、
  `PHASE3-NOTES.md`（跨日核实 10 项、回归 15 项、夜测规程、发布准备）、
  4 份补丁说明（`CMakeLists_pet_patch.md`、`muse_app_pet_patch.md`、
  `muse_input_pet_patch.md`、`muse_ui_patch.md`，**尚未应用到仓库文件**）。
- 设计稿：`notes/20261025-1.5互动预览版设计方案.md`（此前已推，commit 7231ec6）。
- 定稿图：`notes/1.5-interaction-images/` 下 4 张 PNG（小游戏选择页 / 结果页
  v17、喂饭 v18、今日运势 v22）。

## 尚未闭环（合入前必须做）

1. 手图资产未生成：`muse_interact_hand_assets.h` 仍是 `#error` 模板拦截；
   转换前必须删掉 Game-icons SVG 第 1 条满画布矩形 path（否则出黑方块），
   格式按 LVGL 9.5.0，绕 (256,256) 逆时针转 90 度手朝上，见 PHASE3-NOTES §6.2。
2. 补丁尚未应用、尚未编译：合入顺序 CMakeLists → muse_app → muse_input →
   muse_ui（7 处补丁，含三 tile 陷阱三处）→ 手图替换模板 → 编译。
3. 20 张卡逐张实机过屏、跨日实测、回归、电池夜测均未做，清单见 PHASE3-NOTES。
4. 占卜卡文案仍是草稿，等用户最后审；UTC 边界 08:00 等用户过目确认。
