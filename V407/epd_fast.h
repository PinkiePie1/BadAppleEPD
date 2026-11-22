#ifndef EPD_FAST_H
#define EPD_FAST_H
#include "app_config.h"
#include <stdint.h>
#include <stddef.h>

// 200×200 SSD1681，行优先，MSB 在左，1 白 / 0 黑。
#define EPD_W 200
#define EPD_H 200
#define EPD_STRIDE (EPD_W / 8)
#define EPD_FRAME_BYTES (EPD_STRIDE * EPD_H)
#define EPD_DIRTY_BYTES ((EPD_H + 7) / 8)
#define EPD_TICK_HZ 1000u

void EPD_Hal_Init(void);
void EPD_Init(void);
void EPD_Cmd(uint8_t cmd);
void EPD_Dat(uint8_t data);
void EPD_DelayMs(uint32_t ms);
uint32_t EPD_TimeTicks(void);
void EPD_WaitUntil(uint32_t deadline);

void EPD_SendDisplay(const uint8_t *image);
void EPD_FastPrepare(void);
void EPD_FastBegin(const uint8_t *shown);

int EPD_FastStep(const uint8_t *next);
void EPD_InvalidateRows(unsigned y, unsigned height);
void EPD_FastSync(void);

void EPD_Idle(void);

void epd_black7(uint8_t *restrict dst, const uint8_t *restrict target,
                const uint8_t net[restrict 4][EPD_FRAME_BYTES], size_t offset, size_t n);
void epd_white7(uint8_t *restrict dst, const uint8_t *restrict target,
                uint8_t net[restrict 4][EPD_FRAME_BYTES], size_t offset, size_t n);

int epd_blit_changed(uint8_t *restrict dst, const uint8_t *restrict src, size_t n, unsigned mode);

#endif
