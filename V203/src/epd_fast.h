#ifndef APPLE_EPD_FAST_H
#define APPLE_EPD_FAST_H
#include <stdint.h>
#define EPD_W 152u
#define EPD_H 152u
#define EPD_STRIDE (EPD_W / 8u)
#define EPD_FRAME_BYTES (EPD_STRIDE * EPD_H)
// 初始化面板引脚、SPI1 和 DMA1 通道 3。
void EPD_Hal_Init(void);
// 用完整波形全刷一帧，重置像素净驱动预算。
void EPD_SendDisplay(const uint8_t *image);
// 复位面板、载入快刷波形并保持升压电路开启。
void EPD_FastPrepare(void);
// 提交需要驱动的画面；全部饱和后补一次 HOLD，返回是否仍有像素驱动。
int EPD_FastFrame(const uint8_t *next);
// 等待上一轮面板扫描结束。
void EPD_FastSync(void);
// 读取面板 BUSY 引脚。
uint8_t EPD_IsBusy(void);
#endif
