#include "platform.h"
#include "ch32v20x_conf.h"

#if APPLE_WFI
extern const uint8_t platform_wfi_window[], platform_wfi_resume[];

// 开中断后立即执行 WFI；唤醒中断会处理开中断到入睡之间的竞态。
static void __attribute__((noinline, noclone)) sleep_once(void)
{
    __asm__ volatile (
        "fence iorw, iorw\n\t"
        "csrs 0x800, %0\n\t"
        ".global platform_wfi_window\n"
        "platform_wfi_window:\n"
        "wfi\n\t"
        ".global platform_wfi_resume\n"
        "platform_wfi_resume:\n" :: "r"(0x88u) : "memory");
}

// 中断若发生在 WFI 执行前，返回时跳过它，避免错过唤醒后再次入睡。
static inline __attribute__((always_inline)) void skip_pending_wfi(void)
{
    uint32_t epc;
    __asm__ volatile ("csrr %0, mepc" : "=r"(epc));
    if (epc == (uintptr_t)platform_wfi_window) {
        __asm__ volatile ("csrw mepc, %0" :: "r"(platform_wfi_resume) : "memory");
    }
}

void SysTick_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
// 清除定时唤醒源，保持计数器连续运行。
void SysTick_Handler(void)
{
    SysTick->CTLR = 1;
    SysTick->SR = 0;
    skip_pending_wfi();
}

void EXTI0_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
// 清除面板 BUSY 下降沿中断，结束等待。
void EXTI0_IRQHandler(void)
{
    EXTI->INTFR = 1;
    skip_pending_wfi();
}
#endif

// 配置 HCLK/8 连续计时和浅睡眠唤醒源，不改变外设时钟。
void platform_init(void)
{
    SysTick->CTLR = 0;
    SysTick->CNT = 0;
    SysTick->CMP = UINT64_MAX;
    SysTick->SR = 0;
    SysTick->CTLR = 1;
#if APPLE_WFI
    NVIC->SCTLR &= ~((1u << 1) | (1u << 2) | (1u << 3));
    RCC->APB2PCENR |= RCC_APB2Periph_AFIO | RCC_APB2Periph_GPIOA;
    AFIO->EXTICR[0] &= ~15u; // EXTI0 使用 PA0，与 BUSY 接线一致。
    EXTI->RTENR &= ~1u;
    EXTI->FTENR |= 1u;
    EXTI->INTFR = 1u;
    EXTI->INTENR |= 1u;
    NVIC_ClearPendingIRQ(EXTI0_IRQn);
    NVIC_ClearPendingIRQ(SysTick_IRQn);
    NVIC_EnableIRQ(EXTI0_IRQn);
    NVIC_EnableIRQ(SysTick_IRQn);
#endif
}

// 等待截止时刻；busy=1 时也可由 PA0 下降沿提前结束等待。
void platform_wait(uint32_t deadline, uint32_t busy)
{
    while ((int32_t)(deadline - platform_ticks()) > 0 &&
           (!busy || (GPIOA->INDR & 1u))) {
#if APPLE_WFI
        // 先关中断设置唤醒时间，最后在 WFI 前开中断。
        __disable_irq();
        SysTick->CTLR = 1;
        SysTick->SR = 0;
        NVIC_ClearPendingIRQ(SysTick_IRQn);
        volatile uint32_t *cnt = (volatile uint32_t *)&SysTick->CNT;
        volatile uint32_t *cmp = (volatile uint32_t *)&SysTick->CMP;
        uint32_t hi, lo;
        do { hi = cnt[1]; lo = cnt[0]; } while (hi != cnt[1]);
        // 将 32 位截止时间放回 64 位计数周期，跨低字回绕时也能唤醒。
        cmp[1] = UINT32_MAX;
        cmp[0] = deadline;
        cmp[1] = hi + (deadline < lo);
        SysTick->CTLR = 3;
        if ((int32_t)(deadline - platform_ticks()) > 0 &&
            (!busy || (GPIOA->INDR & 1u))) {
            sleep_once();
        }
        SysTick->CTLR = 1;
        SysTick->SR = 0;
        __enable_irq();
#endif
    }
}
