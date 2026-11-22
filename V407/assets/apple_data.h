#ifndef APPLE_DATA_H
#define APPLE_DATA_H
#include <stdint.h>
#define APPLE_ASSET(name) __attribute__((section(".apple." #name), used, aligned(4)))
#define APPLE_RD_FORMAT 1u
#define APPLE_WIDTH 192u
#define APPLE_HEIGHT 144u
#define APPLE_COLS 24u
#define APPLE_ROWS 18u
#define APPLE_CELLS 432u
#define APPLE_FPS 30u
#define APPLE_FRAME_COUNT 6572u
#define APPLE_GLYPH_COUNT 3278u
#define APPLE_GLYPH_BITS 12u
#define APPLE_CLASS_COUNT 12u
#define APPLE_RUN_LIMIT 256u
#define APPLE_TREE_PROB_BITS 4u
#define APPLE_RUN_PROB_BITS 6u
#define APPLE_PAYLOAD_SIZE 434402u
extern const uint8_t apple_glyphs[3278][8];
extern const uint8_t apple_classes[3278];
extern const uint8_t apple_tree[12][1844];
extern const uint8_t apple_run[12][9];
extern const uint8_t apple_tree_lut[16];
extern const uint8_t apple_run_lut[64];
extern const uint8_t apple_run_bucket[256];
extern const uint8_t apple_payload[434402];
#endif
