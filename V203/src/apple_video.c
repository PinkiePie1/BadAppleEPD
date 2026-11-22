#include "apple_video.h"
#include "epd_fast.h"
#include <string.h>
#include APPLE_DATA_HEADER

#ifndef APPLE_PROB_BITS
#define APPLE_PROB_BITS 7
#endif
#ifndef APPLE_RUN_MAPPED
#define APPLE_RUN_MAPPED 0
#endif

static const uint8_t *speedtropy_bitstream;
static uint32_t speedtropy_range;
static uint32_t speedtropy_value;
#define SPEEDTROPY_EMBEDDED
#define SPEEDTROPY_IMPLEMENTATION
#include "speedtropy.h"

#if APPLE_GLYPH_COUNT <= 256
static uint8_t tiles[APPLE_CELLS];
#else
static uint16_t tiles[APPLE_CELLS];
#endif
static uint8_t age[APPLE_CELLS];

// 将打包概率还原为对应量化区间的 1..255 中点。
static uint8_t probability(const uint8_t *row, unsigned index)
{
    unsigned bit = index * APPLE_PROB_BITS, offset = bit >> 3;
    unsigned word = row[offset] | ((unsigned)row[offset + 1] << 8);
    unsigned shift = 8 - APPLE_PROB_BITS;
    unsigned value = (word >> (bit & 7)) & ((1u << APPLE_PROB_BITS) - 1u);
    return (value << shift) + (1u << (shift - 1));
}

// 清空字形和 run 上下文，从码流开头重新播放。
void apple_video_reset(void)
{
    memset(tiles, 0, sizeof(tiles));
    memset(age, 0, sizeof(age));
    SpeedtropyEmbeddedReadInit(apple_payload);
}

// 解码一帧字形索引；调用者按视频总帧数控制结束。
void apple_video_next(void)
{
    for (unsigned c = 0; c < APPLE_CELLS; ++c) {
        unsigned group = apple_classes[tiles[c]];
        unsigned bucket = age[c];
#if APPLE_RUN_MAPPED
        bucket = apple_run_map[bucket];
#endif
        if (SpeedtropyEmbeddedRead(probability(apple_run[group], bucket))) {
            if (age[c] < APPLE_RUN_LIMIT - 1) ++age[c];
        } else {
            unsigned node = 0, glyph = 0;
            for (unsigned b = 0; b < APPLE_GLYPH_BITS; ++b) {
                unsigned bit = SpeedtropyEmbeddedRead(probability(apple_tree[group], node));
                glyph = (glyph << 1) | bit;
                node = node * 2 + 1 + bit;
            }
            tiles[c] = glyph;
            age[c] = 0;
        }
    }
}

// 返回视频总帧数。
uint32_t apple_video_frames(void) { return APPLE_FRAME_COUNT; }

// 返回视频帧率。
uint32_t apple_video_fps(void) { return APPLE_FPS; }

// 将画面扩展到 128×96，灰度按固定面板坐标棋盘格显示，写入面板中央。
void apple_video_render(uint8_t *frame)
{
    enum { SCALE = 128 / APPLE_WIDTH, PANEL_X = 12, PANEL_Y = 28 };
    for (unsigned y = 0; y < 96; ++y) {
        for (unsigned tx = 0; tx < APPLE_COLS; ++tx) {
            unsigned id = tiles[(y / SCALE / 8) * APPLE_COLS + tx];
            uint8_t low = apple_glyphs[id][(y / SCALE) & 7];
            uint8_t high = apple_glyphs[id][8 + ((y / SCALE) & 7)];
            unsigned row = 0;
            for (unsigned x = 0; x < 8 * SCALE; ++x) {
                unsigned bit = 128u >> (x / SCALE);
                unsigned checker = (PANEL_X + tx * 8 * SCALE + x + PANEL_Y + y) & 1;
                unsigned white = (high & bit) ? 1 : (low & bit) ? !checker : 0;
                row = (row << 1) | white;
            }
            uint8_t *dst = frame + (PANEL_Y + y) * EPD_STRIDE + PANEL_X / 8 + tx * SCALE;
            if (SCALE == 2) {
                dst[0] = (dst[0] & 0xf0u) | (row >> 12);
                dst[1] = row >> 4;
                dst[2] = (dst[2] & 0x0fu) | (uint8_t)(row << 4);
            } else {
                dst[0] = (dst[0] & 0xf0u) | (row >> 4);
                dst[1] = (dst[1] & 0x0fu) | (uint8_t)(row << 4);
            }
        }
    }
}
