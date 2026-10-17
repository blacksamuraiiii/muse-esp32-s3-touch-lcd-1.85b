# 敲击唤醒 (tap-to-wake) 接线说明

## 1. 把这两个文件拷进固件

```
esp32/components/muse/qmi8658_tap.c
esp32/components/muse/qmi8658_tap.h
```

在 `esp32/components/muse/CMakeLists.txt` 的 `SRCS` 里加上
`qmi8658_tap.c`。

## 2. 初始化（app_main 或 board init 里，I2C 建好之后）

```c
#include "qmi8658_tap.h"

// board 的 I2C 总线上给 0x6B 建一个 device handle（如果还没有）
i2c_master_dev_handle_t qmi_dev;
i2c_master_bus_add_device(i2c_bus, &(i2c_device_config_t){
    .device_address = 0x6B,
    .scl_speed_hz = 400000,
}, &qmi_dev);

qmi8658_tap_set_i2c(qmi_dev);
ESP_ERROR_CHECK(qmi8658_tap_init());
```

## 3. 息屏时调用

```c
// 想睡的时候：
qmi8658_tap_sleep();   // 进 light sleep，敲一下板子即唤醒
// 唤醒后继续跑
if (qmi8658_tap_consumed()) {
    // 是敲击唤醒的，亮屏
}
```

## 4. 调参

tap 灵敏度是经验值，在 `qmi8658_tap.c` 的 `set1[]` 数组里：

| 寄存器 | 含义 | 当前值 | 调法 |
|---|---|---|---|
| CAL1_L | PeakWindow | 0x0A | 敲击不灵敏 → 调小 |
| CAL2_L/H | TapWindow | 0x3C00 | 误触多 → 调小阈值相关 |
| CAL3_L/H | DTapWindow | 0x7800 | 双击窗口 |

先按默认值跑，真机上敲几下看串口 `qmi_tap` 的日志再调。

## 5. 功耗

- Light sleep: ESP32-S3 约 1~2mA
- QMI8658 tap 模式: 加速度计 500Hz ODR，约 150~300µA
- 合计待机 < 3mA，一块 500mAh 电池能撑一周以上

对比：抬腕/计步需要持续轮询，ESP32 不能睡，10mA+ 起步。
