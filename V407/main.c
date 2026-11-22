#include "debug.h"
#include "board.h"
#include "epd_fast.h"
#include "frame_pacer.h"
#include "apple_video.h"
#include <apple_data.h>
#include <string.h>

static uint8_t frame[EPD_FRAME_BYTES] __attribute__((aligned(8)));

static int video_start(void)
{
    apple_video_reset();
    if (apple_video_next()) return -1;
    apple_video_render(frame);
    EPD_SendDisplay(frame);
    EPD_FastPrepare();
    EPD_FastBegin(frame);
    return 0;
}

static void video_error(void)
{
    EPD_Idle();
    for (;;) __asm volatile ("nop");
}

int main(void)
{
    SystemCoreClockUpdate();
    Power_Init();
    __disable_irq();
    Time_Init();
    Time_DelayUs(500000);
    EPD_Hal_Init();
    __enable_irq();

    memset(frame, 0xff, sizeof(frame));
    if (video_start()) video_error();

    uint32_t position = 1, idle_frames = 0;
    FramePacer pacer = { EPD_TimeTicks(), 0 };
    for (;;) {
        if (position == apple_video_frames()) {
            EPD_Idle();
            if (video_start()) video_error();
            position = 1;
        }
        if (apple_video_next()) video_error();
        apple_video_render(frame);
        EPD_InvalidateRows(APPLE_VIDEO_Y, APPLE_HEIGHT);
        int driven = EPD_FastStep(frame);
        if (driven) idle_frames = 0;
        else if (idle_frames < 3 && ++idle_frames == 3) EPD_Idle();
        ++position;

        uint32_t deadline = FramePacer_Next(&pacer, EPD_TimeTicks(),
                                            EPD_TICK_HZ, APPLE_TARGET_FPS);
        if ((int32_t)(deadline - EPD_TimeTicks()) > 0) {
            EPD_FastSync();
            if ((int32_t)(deadline - EPD_TimeTicks()) > 0) {
                EPD_BusSleep();
                EPD_WaitUntil(deadline);
                EPD_BusWake();
            }
        }
    }
}
