/********************************** (C) COPYRIGHT *******************************
 * File Name          : ch32v20x_spi.c
 * Author             : WCH
 * Version            : V1.0.1
 * Date               : 2025/04/26
 * Description        : This file provides all the SPI firmware functions.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
#include "ch32v20x_spi.h"
#include "ch32v20x_rcc.h"

#define CTLR1_SPE_Set         ((uint16_t)0x0040)
#define CTLR1_SPE_Reset       ((uint16_t)0xFFBF)
#define CTLR1_CLEAR_Mask      ((uint16_t)0x3040)
#define SPI_Mode_Select       ((uint16_t)0xF7FF)

// 配置 SPI 主从模式、时钟、位宽和分频。
void SPI_Init(SPI_TypeDef *SPIx, SPI_InitTypeDef *SPI_InitStruct)
{
    uint16_t tmpreg = 0;

    tmpreg = SPIx->CTLR1;
    tmpreg &= CTLR1_CLEAR_Mask;
    tmpreg |= (uint16_t)((uint32_t)SPI_InitStruct->SPI_Direction | SPI_InitStruct->SPI_Mode |
                         SPI_InitStruct->SPI_DataSize | SPI_InitStruct->SPI_CPOL |
                         SPI_InitStruct->SPI_CPHA | SPI_InitStruct->SPI_NSS |
                         SPI_InitStruct->SPI_BaudRatePrescaler | SPI_InitStruct->SPI_FirstBit);

    SPIx->CTLR1 = tmpreg;
    SPIx->I2SCFGR &= SPI_Mode_Select;
    SPIx->CRCR = SPI_InitStruct->SPI_CRCPolynomial;
}

// 开启或关闭 SPI。
void SPI_Cmd(SPI_TypeDef *SPIx, FunctionalState NewState)
{
    if(NewState != DISABLE)
    {
        SPIx->CTLR1 |= CTLR1_SPE_Set;
    }
    else
    {
        SPIx->CTLR1 &= CTLR1_SPE_Reset;
    }
}

// 开启或关闭 SPI 的 DMA 请求。
void SPI_I2S_DMACmd(SPI_TypeDef *SPIx, uint16_t SPI_I2S_DMAReq, FunctionalState NewState)
{
    if(NewState != DISABLE)
    {
        SPIx->CTLR2 |= SPI_I2S_DMAReq;
    }
    else
    {
        SPIx->CTLR2 &= (uint16_t)~SPI_I2S_DMAReq;
    }
}

// 写入 SPI 发送寄存器。
void SPI_I2S_SendData(SPI_TypeDef *SPIx, uint16_t Data)
{
    SPIx->DATAR = Data;
}

// 读取 SPI 接收寄存器。
uint16_t SPI_I2S_ReceiveData(SPI_TypeDef *SPIx)
{
    return SPIx->DATAR;
}

// 读取 SPI 发送、接收或忙状态。
FlagStatus SPI_I2S_GetFlagStatus(SPI_TypeDef *SPIx, uint16_t SPI_I2S_FLAG)
{
    FlagStatus bitstatus = RESET;

    if((SPIx->STATR & SPI_I2S_FLAG) != (uint16_t)RESET)
    {
        bitstatus = SET;
    }
    else
    {
        bitstatus = RESET;
    }

    return bitstatus;
}
