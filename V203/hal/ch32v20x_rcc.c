/********************************** (C) COPYRIGHT *******************************
 * File Name          : ch32v20x_rcc.c
 * Author             : WCH
 * Version            : V1.0.1
 * Date               : 2025/04/20
 * Description        : This file provides all the RCC firmware functions.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/ 
#include "ch32v20x_rcc.h"



// 开启或关闭 AHB 外设时钟。
void RCC_AHBPeriphClockCmd(uint32_t RCC_AHBPeriph, FunctionalState NewState)
{
  if (NewState != DISABLE)
  {
    RCC->AHBPCENR |= RCC_AHBPeriph;
  }
  else
  {
    RCC->AHBPCENR &= ~RCC_AHBPeriph;
  }
}

// 开启或关闭 APB2 外设时钟。
void RCC_APB2PeriphClockCmd(uint32_t RCC_APB2Periph, FunctionalState NewState)
{
  if (NewState != DISABLE)
  {
    RCC->APB2PCENR |= RCC_APB2Periph;
  }
  else
  {
    RCC->APB2PCENR &= ~RCC_APB2Periph;
  }
}
