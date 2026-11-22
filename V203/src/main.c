#include "platform.h"
#include "epd_fast.h"
#include "apple_video.h"
#include <string.h>

static uint8_t framebuffer[EPD_FRAME_BYTES] __attribute__((aligned(4)));

// 循环播放视频；每帧按绝对时刻调度，空余时间继续驱动当前画面。
int main(void)
{
    platform_init();
    EPD_Hal_Init();
    for (;;) {
        // 每轮播放前全刷白屏，清除上轮积累的直流偏置。
        memset(framebuffer, 0xff, sizeof(framebuffer));
        EPD_SendDisplay(framebuffer);
        EPD_FastPrepare();
        apple_video_reset();

        const uint32_t period = platform_tick_hz() / apple_video_fps();
        uint32_t deadline = platform_ticks();
        uint32_t scan_guard = platform_tick_hz() / 500;
        for (uint32_t f = 0; f < apple_video_frames(); ++f) {
            deadline += period;
            apple_video_next();
            apple_video_render(framebuffer);

            while ((int32_t)(deadline - platform_ticks()) > (int32_t)scan_guard) {
                if (EPD_IsBusy()) {
                    platform_wait(deadline - scan_guard, 1);
                    continue;
                }
                uint32_t start = platform_ticks();
                int driven = EPD_FastFrame(framebuffer);
                // 给下一次 SPI 提交留出已测耗时，再加 0.1ms 余量。
                uint32_t elapsed = platform_ticks() - start + platform_tick_hz() / 10000;
                if (elapsed > scan_guard) scan_guard = elapsed;
                if (!driven) break;
            }
            platform_wait(deadline, 0);
        }
        EPD_FastSync();
    }
}
