#ifndef APPLE_BOARD_H
#define APPLE_BOARD_H
#include "../app_config.h"

#include <stdint.h>

void Power_Init(void);

/* WCH debug.h defines PRINT independently of ENABLE_PRINT. Keep a private
 * macro so a disabled UART can never be used because of include order. */
#if ENABLE_PRINT == 1
#include <stdio.h>
#define EPD_LOG(...) printf(__VA_ARGS__)
#else
#define EPD_LOG(...) ((void)0)
#endif

void EPD_WaitBusy(uint32_t timeout_ticks, int mode);
void EPD_HardReset(void);
void EPD_SendData(const uint8_t *data, unsigned length);
void EPD_DMAStart(const uint8_t *data, unsigned length);
void EPD_DMAWait(void);
void EPD_DataBegin(void);
void EPD_DataEnd(void);
void EPD_BusSleep(void);
void EPD_BusWake(void);

void Time_Init(void);
void Time_DelayUs(uint32_t us);

#endif
