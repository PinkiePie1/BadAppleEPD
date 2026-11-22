#ifndef APPLE_VIDEO_H
#define APPLE_VIDEO_H
#include <stdint.h>

// 从码流起点重新开始播放。
void apple_video_reset(void);
// 解码一帧；调用者按 apple_video_frames() 控制帧数。
void apple_video_next(void);
// 返回视频总帧数。
uint32_t apple_video_frames(void);
// 返回视频帧率。
uint32_t apple_video_fps(void);
// 将当前帧绘制到 152×152 屏幕缓存，1 表示白色。
void apple_video_render(uint8_t *panel_frame);
#endif
