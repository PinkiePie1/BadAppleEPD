#include "apple_video.h"
#include "epd_fast.h"
#include <apple_data.h>
#include <riscv_vector.h>
#include <string.h>

#define SPEEDTROPY_READ
#define SPEEDTROPY_IMPLEMENTATION
#include "speedtropy.h"

static speedtropy_reader reader;
static uint16_t tiles[APPLE_CELLS];
static uint8_t age[APPLE_CELLS];

static inline unsigned apple_probability_index(const uint8_t *row, unsigned index, unsigned bits)
{
    unsigned bit = index * bits, offset = bit >> 3;
    unsigned word = row[offset] | ((unsigned)row[offset + 1] << 8);
    return (word >> (bit & 7)) & ((1u << bits) - 1u);
}

void apple_video_reset(void)
{
    memset(tiles, 0, sizeof(tiles));
    memset(age, 0, sizeof(age));
    SpeedtropyReaderInit(&reader, apple_payload, APPLE_PAYLOAD_SIZE);
}

int apple_video_next(void)
{
    for (unsigned cell = 0; cell < APPLE_CELLS; ++cell) {
        unsigned group = apple_classes[tiles[cell]];
        unsigned run = apple_run_bucket[age[cell]];
        unsigned run_prob = apple_run_lut[apple_probability_index(apple_run[group], run,
                                                                  APPLE_RUN_PROB_BITS)];
        int same = SpeedtropyReaderRead(&reader, run_prob);
        if (same < 0) return -1;
        if (same) {
            if (age[cell] < APPLE_RUN_LIMIT - 1) ++age[cell];
            continue;
        }
        unsigned node = 0, glyph = 0;
        for (unsigned bit_index = 0; bit_index < APPLE_GLYPH_BITS; ++bit_index) {
            unsigned tree_prob = apple_tree_lut[
                apple_probability_index(apple_tree[group], node, APPLE_TREE_PROB_BITS)];
            int bit = SpeedtropyReaderRead(&reader, tree_prob);
            if (bit < 0) return -1;
            glyph = (glyph << 1) | (unsigned)bit;
            node = node * 2 + 1 + (unsigned)bit;
        }
        if (glyph >= APPLE_GLYPH_COUNT) return -1;
        tiles[cell] = (uint16_t)glyph;
        age[cell] = 0;
    }
    return 0;
}

uint32_t apple_video_frames(void) { return APPLE_FRAME_COUNT; }

static void render_row(uint8_t *dst, const uint16_t *ids, unsigned screen_y)
{
    uint16_t indices[APPLE_COLS + 2];
    indices[0] = 1;
    for (unsigned x = 0; x < APPLE_COLS; ++x) indices[x + 1] = ids[x];
    indices[APPLE_COLS + 1] = 0;
    size_t vl = __riscv_vsetvl_e16m8(APPLE_COLS + 2);
    vuint16m8_t index = __riscv_vle16_v_u16m8(indices, vl);
    index = __riscv_vsll_vx_u16m8(index, 3, vl);
    index = __riscv_vadd_vx_u16m8(index, (screen_y - APPLE_VIDEO_Y) & 7u, vl);
    vuint8m4_t glyph = __riscv_vluxei16_v_u8m4(&apple_glyphs[0][0], index, vl);
    vuint16m8_t left = __riscv_vzext_vf2_u16m8(glyph, vl);
    vuint16m8_t right = __riscv_vslide1down_vx_u16m8(left, 0, vl);
    vuint16m8_t joined = __riscv_vor_vv_u16m8(__riscv_vsll_vx_u16m8(left, 4, vl),
                                              __riscv_vsrl_vx_u16m8(right, 4, vl), vl);
    vuint8m4_t bytes = __riscv_vncvt_x_x_w_u8m4(joined, vl);
    vl = __riscv_vsetvl_e8m4(APPLE_COLS + 1);
    __riscv_vse8_v_u8m4(dst, bytes, vl);
    dst[APPLE_COLS] |= 0x0f;
}

void apple_video_render(uint8_t *frame)
{
    for (unsigned ty = 0; ty < APPLE_ROWS; ++ty) {
        unsigned screen_y = APPLE_VIDEO_Y + ty * 8;
        uint8_t *row = frame + screen_y * EPD_STRIDE;
        for (unsigned y = 0; y < 8; ++y) {
            memset(row, 0xff, EPD_STRIDE);
            render_row(row, tiles + ty * APPLE_COLS, screen_y + y);
            row += EPD_STRIDE;
        }
    }
}
