#ifndef APPLE_VIDEO_H
#define APPLE_VIDEO_H

#include <stdint.h>

#define APPLE_VIDEO_X 4u
#define APPLE_VIDEO_Y 28u

void apple_video_reset(void);
int apple_video_next(void);
void apple_video_render(uint8_t *frame);
uint32_t apple_video_frames(void);

#endif
