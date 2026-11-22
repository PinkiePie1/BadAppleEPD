#ifndef APPLE_PLATFORM_H
#define APPLE_PLATFORM_H
#include "ch32v20x.h"

#ifndef APPLE_WFI
#define APPLE_WFI 1
#endif

// 初始化连续计时与 WFI 唤醒源。
void platform_init(void);
// 主循环在开中断状态调用；busy=1 时 PA0 下降沿可提前结束等待。
void platform_wait(uint32_t deadline, uint32_t busy);

// 只读计数器低 32 位，所有等待间隔均小于 2^31 tick。
static inline uint32_t platform_ticks(void)
{
    return *(volatile uint32_t *)&SysTick->CNT;
}

// 返回计数器频率。
static inline uint32_t platform_tick_hz(void)
{
    return SystemCoreClock / 8u;
}

// 用 WFI 等待指定毫秒数。
static inline void platform_delay_ms(uint32_t ms)
{
    platform_wait(platform_ticks() + ms * (platform_tick_hz() / 1000u), 0);
}
#endif
