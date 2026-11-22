#include "debug.h"
#include "board.h"
#include "epd_fast.h"

#define RTC_COOKIE 0xe407u

static uint32_t run_source, run_pll_source;
static int run_hse, run_hsi;
static int rtc_ready;

// 保存系统时钟来源，并配置本板外部 VDDK 供电方式。
void Power_Init(void)
{
    run_source = RCC->CFGR0 & RCC_SWS;
    run_pll_source = RCC->CFGR2 & RCC_SYSPLLSRC;
    run_hse = run_source == RCC_SWS_HSE;
    run_hsi = run_source == RCC_SWS_HSI;
    if (run_source == RCC_SWS_PLL) {
        if (run_pll_source == RCC_SYSPLLSRC_PLL) {
            run_hse = (RCC->CFGR0 & RCC_PLLSRC) == RCC_PLLSRC_HSE;
            run_hsi = !run_hse;
        } else if (run_pll_source == RCC_SYSPLLSRC_USBHSPLL ||
                   run_pll_source == RCC_SYSPLLSRC_USBHSPLL_MUL2_DIV3) {
            uint32_t source = RCC->CFGR2 & RCC_USBHSPLLSRC;
            run_hse = source == RCC_USBHSPLLSRC_HSE;
            run_hsi = source == RCC_USBHSPLLSRC_HSI;
        } else {
            run_hse = 1; // ETHPLL 使用外部高速时钟。
        }
    }
    RCC_PB1PeriphClockCmd(RCC_PB1Periph_PWR, ENABLE);
    PWR->CTLR &= ~PWR_CTLR_LDOVDDK;
    PWR_VDD18Cmd(DISABLE);
}

// 启动 1 kHz RTC 和闹钟中断，供帧调度与睡眠唤醒使用。
static void Power_ClockInit(void)
{
    RCC_PB1PeriphClockCmd(RCC_PB1Periph_PWR | RCC_PB1Periph_BKP, ENABLE);
    PWR_BackupAccessCmd(ENABLE);
    RCC_LSICmd(ENABLE);
    while (RCC_GetFlagStatus(RCC_FLAG_LSIRDY) == RESET);

    if (BKP_ReadBackupRegister(BKP_DR1) != RTC_COOKIE ||
        (RCC->BDCTLR & RCC_RTCSEL) != RCC_RTCSEL_LSI) {
        RCC_BackupResetCmd(ENABLE);
        RCC_BackupResetCmd(DISABLE);
        RCC_RTCCLKConfig(RCC_RTCCLKSource_LSI);
        RCC_RTCCLKCmd(ENABLE);
        RTC_WaitForSynchro();
        RTC_WaitForLastTask();
        RTC_SetCounter(0);
        RTC_WaitForLastTask();
        BKP_WriteBackupRegister(BKP_DR1, RTC_COOKIE);
    }
    RCC_RTCCLKCmd(ENABLE);
    RTC_WaitForSynchro();
    RTC_WaitForLastTask();
    RTC_SetPrescaler(40000u / EPD_TICK_HZ - 1u);
    RTC_WaitForLastTask();
    RTC_WaitForSynchro();
    RTC_WaitForLastTask();
    RTC_ITConfig(RTC_IT_ALR | RTC_IT_SEC | RTC_IT_OW, DISABLE);
    RTC_WaitForLastTask();
    EXTI_InitTypeDef exti = {0};
    exti.EXTI_Line = EXTI_Line17;
    exti.EXTI_Mode = EXTI_Mode_Interrupt;
    exti.EXTI_Trigger = EXTI_Trigger_Rising;
    exti.EXTI_LineCmd = ENABLE;
    EXTI_Init(&exti);
    NVIC_SetPriority(RTCAlarm_IRQn, 0x80);
    NVIC_EnableIRQ(RTCAlarm_IRQn);
    rtc_ready = 1;
}

// 清除 RTC 闹钟挂起并唤醒前台等待。
void RTCAlarm_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void RTCAlarm_IRQHandler(void)
{
    RTC_ClearITPendingBit(RTC_IT_ALR);
    EXTI_ClearITPendingBit(EXTI_Line17);
    _SEV();
}

// BUSY 下降沿表示面板扫描结束，清中断并唤醒前台。
void EXTI0_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void EXTI0_IRQHandler(void)
{
    EXTI_ClearITPendingBit(EXTI_Line0);
    _SEV();
}

// 设置下一次 RTC 唤醒期限，并清除旧挂起状态。
static void Power_AlarmArm(uint32_t deadline)
{
    RTC_WaitForLastTask();
    RTC_ITConfig(RTC_IT_ALR, DISABLE);
    RTC_WaitForLastTask();
    RTC_ClearITPendingBit(RTC_IT_ALR);
    RTC_WaitForLastTask();
    EXTI_ClearITPendingBit(EXTI_Line17);
    NVIC_ClearPendingIRQ(RTCAlarm_IRQn);
    RTC_SetAlarm(deadline);
    RTC_WaitForLastTask();
    RTC_ITConfig(RTC_IT_ALR, ENABLE);
    RTC_WaitForLastTask();
}

// 等待结束后关闭 RTC 闹钟，避免后续重复唤醒。
static void Power_AlarmDisarm(void)
{
    RTC_WaitForLastTask();
    RTC_ITConfig(RTC_IT_ALR, DISABLE);
    RTC_WaitForLastTask();
}

// STOP 后恢复原来的 HSI/HSE/PLL 系统时钟。
static void clock_resume(void)
{
    if (run_source == RCC_SWS_HSI) {
        RTC_WaitForSynchro();
        return;
    }
    if (run_hsi) {
        RCC_HSICmd(ENABLE);
        while (RCC_GetFlagStatus(RCC_FLAG_HSIRDY) == RESET);
    }
    if (run_hse) {
        RCC_HSEConfig(RCC_HSE_ON);
        while (RCC_GetFlagStatus(RCC_FLAG_HSERDY) == RESET);
    }
    if (run_source == RCC_SWS_HSE) {
        RCC_SYSCLKConfig(RCC_SYSCLKSource_HSE);
        while (RCC_GetSYSCLKSource() != RCC_SWS_HSE);
        RCC_HSICmd(DISABLE);
        RTC_WaitForSynchro();
        return;
    }
    uint32_t source = run_pll_source;
    if (source == RCC_SYSPLLSRC_USBHSPLL || source == RCC_SYSPLLSRC_USBHSPLL_MUL2_DIV3) {
        RCC->CTLR |= RCC_USBHSPLLON;
        while (!(RCC->CTLR & RCC_USBHSPLLRDY));
    } else if (source == RCC_SYSPLLSRC_ETHPLL || source == RCC_SYSPLLSRC_ETHPLL_MUL2_DIV3) {
        RCC->CTLR |= RCC_ETHPLLON;
        while (!(RCC->CTLR & RCC_ETHPLLRDY));
    } else {
        RCC_PLLCmd(ENABLE);
        while (RCC_GetFlagStatus(RCC_FLAG_PLLRDY) == RESET);
    }
    RCC_SYSCLKConfig(RCC_SYSCLKSource_PLLCLK);
    while (RCC_GetSYSCLKSource() != RCC_SWS_PLL);
    if (!run_hsi) RCC_HSICmd(DISABLE);
    RTC_WaitForSynchro();
}

// 进入 SLEEP 或保留 RAM 的 STOP，并在 STOP 后恢复时钟。
static void Power_SleepEvent(int stop)
{
#if ENABLE_PRINT
    if (stop) {
        while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET);
    }
#endif
    if (stop) {
        PWR->CTLR = (PWR->CTLR & ~PWR_CTLR_PDDS) | PWR_CTLR_LPDS | PWR_CTLR_RAMLV;
        NVIC->SCTLR |= 1u << 2;
    } else {
        NVIC->SCTLR &= ~(1u << 2);
    }
    _WFE();
    if (stop) {
        NVIC->SCTLR &= ~(1u << 2);
        PWR->CTLR = (PWR->CTLR & ~(PWR_CTLR_LPDS | PWR_CTLR_RAMLV)) | PWR_CTLR_CWUF;
        if ((RCC->CFGR0 & RCC_SWS) != run_source || run_source == RCC_SWS_HSI) {
            clock_resume();
        }
    }
}

// 按配置忙等或睡到指定 RTC tick，并避免丢失唤醒事件。
static void Power_WaitUntil(uint32_t deadline, int mode)
{
    if (!mode) {
        while ((int32_t)(EPD_TimeTicks() - deadline) < 0);
        return;
    }
    if ((int32_t)(EPD_TimeTicks() - deadline) >= 0) return;
    Power_AlarmArm(deadline);
    for (;;) {
        _SEV();
        _WFE();
        if ((int32_t)(EPD_TimeTicks() - deadline) >= 0) break;
        Power_SleepEvent(mode == 2);
    }
    Power_AlarmDisarm();
}

// 启动自由运行的 SysTick，供微秒级性能计时使用。
void Time_Init(void)
{
    SysTick->CTLR = 0;
    SysTick->CNT = 0;
    SysTick->CMP = UINT32_MAX;
    SysTick->CTLR = 1;
}

// 返回 RTC 的 1 kHz 计数，睡眠期间仍持续增加。
uint32_t EPD_TimeTicks(void) { return RTC_GetCounter(); }

// 用 SysTick 忙等指定微秒数，避免低功耗等待影响短延时。
void Time_DelayUs(uint32_t us)
{
    uint32_t start = SysTick->CNT;
    uint32_t count = us * (SystemCoreClock / 8000000U);
    while ((uint32_t)(SysTick->CNT - start) < count) {}
}

// 按毫秒提供 EPD 初始化延时。
void EPD_DelayMs(uint32_t ms)
{
    if (rtc_ready && APPLE_SLEEP_MODE && ms) {
        Power_WaitUntil(EPD_TimeTicks() + ms * (EPD_TICK_HZ / 1000U) + 1U,
                        APPLE_SLEEP_MODE);
        return;
    }
    while (ms--) Time_DelayUs(1000);
}

// 按当前睡眠配置等待到指定 RTC 期限。
void EPD_WaitUntil(uint32_t deadline)
{
    Power_WaitUntil(deadline, APPLE_SLEEP_MODE);
}

static volatile uint8_t dma_done;

#define CS_H() (GPIOA->BSHR = GPIO_Pin_4)
#define CS_L() (GPIOA->BCR = GPIO_Pin_4)
#define DC_H() (GPIOA->BSHR = GPIO_Pin_3)
#define DC_L() (GPIOA->BCR = GPIO_Pin_3)
#define BUSY() (GPIOA->INDR & GPIO_Pin_0)

// 关闭空闲 SPI/DMA 的总线时钟，面板和 RTC 保持运行。
void EPD_BusSleep(void)
{
    RCC_PB2PeriphClockCmd(RCC_PB2Periph_SPI1, DISABLE);
    RCC_HBPeriphClockCmd(RCC_HBPeriph_DMA1, DISABLE);
}

// 恢复 SPI/DMA 时钟，不改变已有外设和屏幕状态。
void EPD_BusWake(void)
{
    RCC_HBPeriphClockCmd(RCC_HBPeriph_DMA1, ENABLE);
    RCC_PB2PeriphClockCmd(RCC_PB2Periph_SPI1, ENABLE);
}

// 等待 BUSY 拉低，可按配置轮询、SLEEP 或 STOP，超时则复位。
void EPD_WaitBusy(uint32_t timeout_ticks, int mode)
{
    int sleep = mode < APPLE_SLEEP_MODE ? mode : APPLE_SLEEP_MODE;
    if (!BUSY()) return;
    uint32_t deadline = EPD_TimeTicks() + timeout_ticks + 1;
    if (sleep) {
        EPD_BusSleep();
        Power_AlarmArm(deadline);
    }
    for (;;) {
        if (sleep) { _SEV(); _WFE(); }
        if (!BUSY()) break;
        if ((int32_t)(EPD_TimeTicks() - deadline) >= 0) {
            EPD_LOG("EPD BUSY timeout\r\n");
            NVIC_SystemReset();
        }
        if (sleep) Power_SleepEvent(sleep == 2);
    }
    if (sleep) {
        Power_AlarmDisarm();
        EPD_BusWake();
    }
}

// 通过 SPI 发送一个字节并读取同时收到的数据。
static void spi_byte(uint8_t value)
{
    while (!(SPI1->STATR & SPI_I2S_FLAG_TXE)) {}
    SPI1->DATAR = value;
    while (!(SPI1->STATR & SPI_I2S_FLAG_RXNE)) {}
    (void)SPI1->DATAR;
}

// 发送命令或单个数据字节。
void EPD_Cmd(uint8_t value) { DC_L(); CS_L(); spi_byte(value); CS_H(); }
void EPD_Dat(uint8_t value) { DC_H(); CS_L(); spi_byte(value); CS_H(); }

// 开始或结束一段连续数据传输。
void EPD_DataBegin(void) { DC_H(); CS_L(); }
void EPD_DataEnd(void)
{
    while (!(SPI1->STATR & SPI_I2S_FLAG_TXE)) {}
    while (SPI1->STATR & SPI_I2S_FLAG_BSY) {}
    (void)SPI1->DATAR;
    (void)SPI1->STATR;
    CS_H();
}
// 启动 SPI TX DMA，发送完成前调用者不能修改源缓存。
void EPD_DMAStart(const uint8_t *data, unsigned length)
{
    DMA1_Channel3->CFGR &= ~DMA_CFGR1_EN;
    dma_done = 0;
    DMA1->INTFCR = DMA1_FLAG_GL3;
    DMA1_Channel3->MADDR = (uint32_t)data;
    DMA1_Channel3->CNTR = length;
    __asm volatile ("fence rw,rw" ::: "memory");
    DMA1_Channel3->CFGR |= DMA_CFGR1_EN;
}

void DMA1_Channel3_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

// DMA 中断只设置完成标志；保持标量，不干扰前台 RVV 寄存器。
void DMA1_Channel3_IRQHandler(void)
{
    DMA1->INTFCR = DMA1_FLAG_GL3;
    dma_done = 1;
    _SEV();
}

// DMA 期间只让 CPU SLEEP；SPI 和 DMA 时钟继续运行。
void EPD_DMAWait(void)
{
    for (;;) {
        _SEV();
        _WFE();
        if (dma_done) break;
        Power_SleepEvent(0);
    }
    DMA1_Channel3->CFGR &= ~DMA_CFGR1_EN;
}

// 长数据走 DMA，短数据逐字节发送。
void EPD_SendData(const uint8_t *data, unsigned length)
{
    EPD_DataBegin();
    if (length >= 16) {
        EPD_DMAStart(data, length);
        EPD_DMAWait();
    } else {
        while (length--) spi_byte(*data++);
    }
    EPD_DataEnd();
}
// 按高、低、高时序硬复位墨水屏控制器。
void EPD_HardReset(void)
{
    GPIOA->BSHR = GPIO_Pin_6; EPD_DelayMs(50);
    GPIOA->BCR = GPIO_Pin_6; EPD_DelayMs(20);
    GPIOA->BSHR = GPIO_Pin_6; EPD_DelayMs(50);
}

// 初始化 SPI、DMA、BUSY 中断和 RTC 唤醒资源。
void EPD_Hal_Init(void)
{
    RCC_PB2PeriphClockCmd(RCC_PB2Periph_GPIOA | RCC_PB2Periph_AFIO | RCC_PB2Periph_SPI1, ENABLE);
    CS_H(); DC_H(); GPIOA->BSHR = GPIO_Pin_6;
    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = GPIO_Pin_3 | GPIO_Pin_4 | GPIO_Pin_6;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_High;
    GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_0;
    gpio.GPIO_Mode = GPIO_Mode_IPD;
    GPIO_Init(GPIOA, &gpio);
    GPIO_EXTILineConfig(GPIO_PortSourceGPIOA, GPIO_PinSource0);
    EXTI_InitTypeDef exti = {0};
    exti.EXTI_Line = EXTI_Line0;
    exti.EXTI_Mode = EXTI_Mode_Interrupt;
    exti.EXTI_Trigger = EXTI_Trigger_Falling;
    exti.EXTI_LineCmd = ENABLE;
    EXTI_Init(&exti);
    EXTI_ClearITPendingBit(EXTI_Line0);
    NVIC_SetPriority(EXTI0_IRQn, 0x80);
    NVIC_EnableIRQ(EXTI0_IRQn);
    GPIO_PinRemapConfig(GPIO_Remap_SPI1, DISABLE);
    gpio.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_7;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &gpio);
    SPI_InitTypeDef spi = {0};
    spi.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    spi.SPI_Mode = SPI_Mode_Master;
    spi.SPI_DataSize = SPI_DataSize_8b;
    spi.SPI_CPOL = SPI_CPOL_Low;
    spi.SPI_CPHA = SPI_CPHA_1Edge;
    spi.SPI_NSS = SPI_NSS_Soft;
    spi.SPI_FirstBit = SPI_FirstBit_MSB;
    spi.SPI_CRCPolynomial = 7;
    spi.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_8;
    SPI_Init(SPI1, &spi);
    SPI_Cmd(SPI1, ENABLE);
    RCC_HBPeriphClockCmd(RCC_HBPeriph_DMA1, ENABLE);
    DMA_DeInit(DMA1_Channel3);
    DMA_InitTypeDef dma = {0};
    dma.DMA_PeripheralBaseAddr = (uint32_t)&SPI1->DATAR;
    dma.DMA_DIR = DMA_DIR_PeripheralDST;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    dma.DMA_Mode = DMA_Mode_Normal;
    dma.DMA_Priority = DMA_Priority_VeryHigh;
    DMA_Init(DMA1_Channel3, &dma);
    DMA_ITConfig(DMA1_Channel3, DMA_IT_TC, ENABLE);
    NVIC_SetPriority(DMA1_Channel3_IRQn, 0x80);
    NVIC_EnableIRQ(DMA1_Channel3_IRQn);
    SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Tx, ENABLE);
    Power_ClockInit();
    RCC_PB2PeriphClockCmd(RCC_PB2Periph_AFIO, DISABLE);
    RCC_ClocksTypeDef clocks;
    RCC_GetClocksFreq(&clocks);
    EPD_LOG("EPD HAL: SPI1=%lu Hz, RTC=%u Hz, BUSY sleep=%u, frame sleep=%u\r\n",
            (unsigned long)(clocks.PCLK2_Frequency / 8), (unsigned)EPD_TICK_HZ,
            (unsigned)EPD_BUSY_SLEEP, (unsigned)APPLE_SLEEP_MODE);
}

/* Keep the fault PC visible to the debugger instead of resetting in a loop. */
volatile uint32_t fault_cause, fault_pc, fault_value;
void NMI_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void HardFault_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void NMI_Handler(void)
{
    __asm volatile ("csrr %0, mcause" : "=r" (fault_cause));
    __asm volatile ("csrr %0, mepc" : "=r" (fault_pc));
    __asm volatile ("csrr %0, mtval" : "=r" (fault_value));
    for (;;) __asm volatile ("nop");
}
void HardFault_Handler(void)
{
    __asm volatile ("csrr %0, mcause" : "=r" (fault_cause));
    __asm volatile ("csrr %0, mepc" : "=r" (fault_pc));
    __asm volatile ("csrr %0, mtval" : "=r" (fault_value));
    for (;;) __asm volatile ("nop");
}
