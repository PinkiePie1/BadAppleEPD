#ifndef APPLE_FRAME_PACER_H
#define APPLE_FRAME_PACER_H

#include <stdint.h>

typedef struct {
    uint32_t deadline;
    uint32_t remainder;
} FramePacer;

static inline uint32_t FramePacer_Next(FramePacer *pacer, uint32_t now,
                                     uint32_t tick_hz, uint32_t fps)
{
    pacer->deadline += tick_hz / fps;
    pacer->remainder += tick_hz % fps;
    if (pacer->remainder >= fps) {
        pacer->remainder -= fps;
        ++pacer->deadline;
    }
    if ((int32_t)(now - pacer->deadline) > 0) {
        pacer->deadline = now;
        pacer->remainder = 0;
    }
    return pacer->deadline;
}

#endif
