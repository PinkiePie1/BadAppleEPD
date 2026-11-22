# appleEPD407

CH32V407 + SSD1681 200x200 墨水屏播放Bad Apple，30 FPS。压缩原理和CH32V203版本相同，glyph和码表都更大一些。

芯片内存划分选项需要用wch-linkutility选到200kRAM，需要外置VDDK供电，注意如果没有外置供电需要改代码把对VDDK相关寄存器的访问去掉。

可用 `WCH407_TOOLCHAIN_PATH` 指定工具链目录，或用 `PREFIX` 指定完整工具前缀。

`make -j flash` 烧录完整固件

`make recover` 救砖


## 配置

[app_config.h](app_config.h) ：

- `APPLE_TARGET_FPS`：目标帧率，默认 30。
- `APPLE_SLEEP_MODE`：空闲等待模式，默认 2（STOP）。
- `EPD_BUSY_SLEEP`：面板 BUSY 等待模式，默认 2（STOP）。
- `ENABLE_PRINT`：PA9 TX、115200 8N1 日志开关，默认 0。

## 硬件

| 屏幕信号 | CH32V407 |
| --- | --- |
| BUSY | PA0 |
| DC | PA3 |
| CS | PA4 |
| SCK | PA5 |
| RST | PA6 |
| MOSI | PA7 |
| VCC / GND | 3V3 / GND |

