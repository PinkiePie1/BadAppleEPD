/********************************** (C) COPYRIGHT *******************************
* File Name          : system_ch32v4x7.c
* Author             : WCH
* Version            : V1.0.0
* Date               : 2025/12/01
* Description        : CH32V4x7 Device Peripheral Access Layer System Source File.
*                      For HSE = 25Mhz
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
#include "ch32v4x7.h"

/*
* Uncomment the line corresponding to the desired System clock (SYSCLK) frequency (after
* reset the HSI is used as SYSCLK source).
* If none of the define below is enabled, the HSI is used as System clock source.
*/
// #define SYSCLK_HCLK_HSE    HSE_VALUE
// #define SYSCLK_120MHz_HCLK_60MHz_HSE   120000000
// #define SYSCLK_240MHz_HCLK_120MHz_HSE  240000000
// #define SYSCLK_350MHz_HCLK_175MHz_HSE  350000000
// #define SYSCLK_400MHz_HCLK_200MHz_HSE  400000000
// #define SYSCLK_HCLK_HSI    HSI_VALUE
// #define SYSCLK_120MHz_HCLK_60MHz_HSI   120000000
// #define SYSCLK_240MHz_HCLK_120MHz_HSI  240000000
// #define SYSCLK_350MHz_HCLK_175MHz_HSI  350000000
// #define SYSCLK_400MHz_HCLK_200MHz_HSI  400000000

/*Only suitable for commercial applications, with a temperature not exceeding 70 °C and good heat dissipation*/
/*
// #define SYSCLK_480MHz_HCLK_240MHz_HSE    480000000
#define SYSCLK_480MHz_HCLK_240MHz_HSI    480000000
*/

#define SYSCLK_480MHz_HCLK_240MHz_HSI 480000000

/* Clock Definitions */
uint32_t HCLKClock;
uint32_t SystemClock         = SYSCLK_480MHz_HCLK_240MHz_HSI;        /* System Clock Frequency  */
uint32_t SystemCoreClock     = SYSCLK_480MHz_HCLK_240MHz_HSI >> 1;

static __I uint8_t HBPrescTable[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 6, 7, 8, 9};
static __I uint8_t PLLMULTB[32] = {8,9,10,11,12,25,13,27,14,29,15,31,16,33,17,35,18,37,19,39,20,21,22,24,26,28,30,32,34,36,38,40};

/* system_private_function_proto_types */
static void SetSysClock(void);

static void SetSYSCLK_480MHz_HCLK_240MHz_HSI( void );

/*********************************************************************
 * @fn      SystemInit
 *
 * @brief   Setup the microcontroller system Initialize the Embedded Flash Interface,
 *        the PLL and update the SystemClock variable.
 *
 * @return  none
 */
void SystemInit (void)
{
  RCC->CTLR |= (uint32_t)0x00000001;
  RCC->CFGR0 &= (uint32_t)0x80FF0000;
  while ((RCC->CFGR0 & (uint32_t)RCC_SWS) != (uint32_t)0x00)
  {
  }

  RCC->CTLR &= (uint32_t)0xFEA6FFFF;
  RCC->CTLR &= (uint32_t)0xFFFBFFFF;

  RCC->CFGR0 &= (uint32_t)0xFFC0FFFF;
  RCC->CFGR2 &= (uint32_t)0xFFFFEFFF;
  RCC->CFGR2 &= (uint32_t)0x03F11800;

  RCC->CFGR0 |= (uint32_t)(0x20000000);
  RCC->CFGR3 = 0x00000000;
  RCC->INTR = 0x008F0000;

  SetSysClock();
}

/*********************************************************************
 * @fn      SystemCoreClockUpdate
 *
 * @brief   Update SystemCoreClock variable according to Clock Register Values.
 *
 * @return  none
 */
void SystemCoreClockUpdate (void)
{
  uint32_t tmp = 0, tmp1 = 0, tmp2 = 0, pllmull = 0, pllsource = 0, presc = 0;
  uint8_t Plldiv2 = 0;

  tmp = RCC->CFGR0 & RCC_SWS;
  tmp1 = RCC->CFGR2 & RCC_SYSPLLSRC;

  switch (tmp)
  {
    case 0x00:
      SystemClock = HSI_VALUE;
      break;
    case 0x04:
      SystemClock = HSE_VALUE;
      break;
    case 0x08:
      pllsource = RCC->CFGR0 & RCC_PLLSRC;
      presc = ((RCC->CFGR2 & RCC_SYSPLLDIV)>>8) + 1;
      tmp2 = (RCC->CFGR0 & RCC_PLLMULL)>>17;
      pllmull = PLLMULTB[tmp2];
      if((tmp2 > 4) && (tmp2 < 20) && ((tmp2 % 2) != 0))
      {
        Plldiv2 = 1;
      }

      switch (tmp1)
      {
        case RCC_SYSPLLSRC_USBHSPLL:
          SystemClock = 480000000;
        break;

        case RCC_SYSPLLSRC_ETHPLL:
          SystemClock = 500000000;
        break;

        case RCC_SYSPLLSRC_USBHSPLL_MUL2_DIV3:
          SystemClock = 320000000;
        break;

        case RCC_SYSPLLSRC_ETHPLL_MUL2_DIV3:
          SystemClock = 333000000;
        break;

        case RCC_SYSPLLSRC_PLL:

          if(pllsource == 0x00) //HSI
          {
            SystemClock = HSI_VALUE*pllmull;
          }
          else
          {
            SystemClock = HSE_VALUE*pllmull;
          }
          SystemClock /= (1 + Plldiv2);
        break;

        default:
        break;
      }
      SystemClock /= presc;
      break;

      default:
      SystemClock = HSI_VALUE;
      break;
  }
    tmp = HBPrescTable[((RCC->CFGR0 & RCC_HPRE) >> 4)];
    SystemCoreClock = SystemClock >> tmp;
    HCLKClock =  SystemCoreClock;
}

/*********************************************************************
 * @fn      SetSysClock
 *
 * @brief   Configures the System clock frequency, HCLK, PCLK2 and PCLK1 prescalers.
 *
 * @return  none
 */
static void SetSysClock(void)
{
    GPIO_IPD_Unused();
    SetSYSCLK_480MHz_HCLK_240MHz_HSI();

/*
  * If none of the define above is enabled, the HSI is used as System clock
  * source (default after reset)
  */
}

/*********************************************************************
 * @fn      SetSYSCLK_480MHz_HCLK_240MHz_HSI
 *
 * @brief   Sets System clock frequency to 480MHz and configure HCLK, PCLK2 and PCLK1 prescalers.
 *
 * @return  none
 */
static void SetSYSCLK_480MHz_HCLK_240MHz_HSI(void)
{
    __IO uint32_t FLASH_Temp = 0;
    /* This board uses an external VDDK DCDC; keep the internal LDO off. */
    RCC->PB1PCENR |= RCC_PB1Periph_PWR;
    PWR->CTLR &= ~PWR_CTLR_LDOVDDK;

    /* HCLK = SYSCLK / 2 */
    RCC->CFGR0 |= (uint32_t)RCC_HPRE_DIV2;
    /* PCLK2 = HCLK */
    RCC->CFGR0 |= (uint32_t)RCC_PPRE2_DIV1;
    /* PCLK1 = HCLK */
    RCC->CFGR0 |= (uint32_t)RCC_PPRE1_DIV1;

    /* PLL configuration: HCLK = HSI * 24 / 1 / 2 = 240 MHz */
    RCC->CFGR0 |= (uint32_t)(RCC_PLLSRC_HSI | RCC_PLLMULL24);

    /* Enable PLL */
    RCC->CTLR |= RCC_PLLON;
    /* Wait till PLL is ready */
    while((RCC->CTLR & RCC_PLLRDY) == 0)
    {
    }

    RCC->CFGR2 |= (uint32_t)(RCC_SYSPLLSRC_PLL | RCC_SYSPLLDIV1);
    /* Wait till PLL clock is used as SYSPLLSRC clock source */
    while ((RCC->CFGR2 & (uint32_t)RCC_SYSPLLSRC) != (uint32_t)RCC_SYSPLLSRC_PLL)
    {
    }

    /* Select FLASH clock frequency*/
    FLASH->KEYR = ((uint32_t)0x45670123);
    FLASH->KEYR = ((uint32_t)0xCDEF89AB);
    FLASH->MODEKEYR = ((uint32_t)0x45670123);
    FLASH->MODEKEYR = ((uint32_t)0xCDEF89AB);

    FLASH_Temp = FLASH->CTLR ;
    FLASH_Temp &= ~((uint32_t)FLASH_CTLR_SCKMOD);
    FLASH_Temp |= FLASH_CTLR_SCKMOD_Div4;
    FLASH->CTLR = FLASH_Temp;
    FLASH->CTLR |= ((uint32_t)0x00008080);

    /* Select PLL as system clock source */
    RCC->CFGR2 |= (uint32_t)RCC_SYSPLL_GATE;
    RCC->CFGR0 &= (uint32_t)((uint32_t)~(RCC_SW));
    RCC->CFGR0 |= (uint32_t)RCC_SW_PLL;
    /* Wait till PLL is used as system clock source */
    while ((RCC->CFGR0 & (uint32_t)RCC_SWS) != (uint32_t)0x08)
    {
    }
}
