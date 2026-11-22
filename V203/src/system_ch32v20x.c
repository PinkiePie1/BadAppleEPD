/********************************** (C) COPYRIGHT *******************************
 * File Name          : system_ch32v20x.c
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2021/06/06
 * Description        : CH32V20x Device Peripheral Access Layer System Source File.
 *                      For HSE = 32Mhz (CH32V208x/CH32V203RBT)
 *                      For HSE = 8Mhz (other CH32V203x)
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
#include "ch32v20x.h"

uint32_t SystemCoreClock = 144000000;

// 启动时复位时钟配置，再用 HSI/PLL 设置 144MHz；APB1 为 72MHz。
void SystemInit(void)
{
    RCC->CTLR |= 0x00000001;
    RCC->CFGR0 &= 0xF0FF0000;
    RCC->CTLR &= 0xFEF6FFFF;
    RCC->CTLR &= 0xFFFBFFFF;
    RCC->CFGR0 &= 0xFF00FFFF;
    RCC->INTR = 0x009F0000;

    EXTEN->EXTEN_CTR |= EXTEN_PLL_HSI_PRE;
    RCC->CFGR0 |= RCC_HPRE_DIV1;
    RCC->CFGR0 |= RCC_PPRE2_DIV1;
    RCC->CFGR0 |= RCC_PPRE1_DIV2;
    RCC->CFGR0 &= ~(RCC_PLLSRC | RCC_PLLXTPRE | RCC_PLLMULL);
    RCC->CFGR0 |= RCC_PLLSRC_HSI_Div2 | RCC_PLLMULL18;
    RCC->CTLR |= RCC_PLLON;
    while ((RCC->CTLR & RCC_PLLRDY) == 0) { }
    RCC->CFGR0 &= ~RCC_SW;
    RCC->CFGR0 |= RCC_SW_PLL;
    while ((RCC->CFGR0 & RCC_SWS) != 0x08) { }
}
