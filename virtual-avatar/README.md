# virtual-avatar —— 板载虚拟形象工作区（规划中，未开工）

给 Waveshare ESP32-S3-Touch-LCD-1.85B 的 360×360 圆屏做**板载**虚拟形象：
多状态动画 + 表情 + 情绪映射，并与语音播报（TTS）状态同步口型。

**完整计划**：`~/hermes-sync/docs/plan/2026-10-16_ESP32-185B虚拟形象-OTA-TTS计划.md`（阶段 1，任务 T1–T5）

## 目录约定

| 子路径 | 放什么 |
|---|---|
| `preview/` | `tools/muse/make_gifs.py` 出的每状态 GIF，作迭代对照（不提交，见 .gitignore） |
| `art/` | 形象源素材与造型稿（自绘像素稿、色板） |
| `spec/` | 状态-表情-情绪对照表、动画节拍说明 |

## 关键事实（动手前必看）

- 渲染器接口在 `esp32/components/muse/muse_pixel.h`：64×64 网格，
  `muse_pixel_render(const muse_pose_t *pose)`，pose = `{mode, t, mode_t, level, happy}`。
- 动画状态只有七个：`esp32/components/muse/muse_state.h` 的 `muse_mode_t`
  （BOOT / IDLE / LISTENING / THINKING / SPEAKING / ERROR / OFF），外加 0..1 的摸头反应 `happy`。
- 帧周期 40 ms（25 fps），见 `esp32/components/muse/boards/board_waveshare_s3_touch_lcd_185b.c` 的 `.frame_ms = 40`。
  重绘要留在 10 ms 以内，否则掉帧。
- 默认形象是 Meta 的 Jollybot，Apache 许可**不覆盖**（根 `README.md` 末尾）→ 必须自绘。
- 上游的自定义形象目录 `esp32/components/muse/avatar/` **被 gitignore**，形象进不了固件发布，
  与 OTA 冲突；本仓库改为**仓库内模块 + Kconfig 开关**（计划 T2）。
- 口型数据源要改：`pose.level` 现在接的是麦克风电平，TTS 播放时恒低（计划 T5）。
