// SSD1680 152×152 快刷：沿用 FastEPD 的波形、DMA 和像素净驱动算法。
#include "ch32v20x_conf.h"
#include "platform.h"
#include "epd_fast.h"

#include <string.h>

#define EPD_CS_PORT     GPIOA
#define EPD_CS_PIN      GPIO_Pin_4
#define EPD_DC_PORT     GPIOA
#define EPD_DC_PIN      GPIO_Pin_3
#define EPD_RST_PORT    GPIOA
#define EPD_RST_PIN     GPIO_Pin_6
#define EPD_BUSY_PORT   GPIOA
#define EPD_BUSY_PIN    GPIO_Pin_0

#define CS_H()   GPIO_SetBits(EPD_CS_PORT, EPD_CS_PIN)
#define CS_L()   GPIO_ResetBits(EPD_CS_PORT, EPD_CS_PIN)
#define DC_H()   GPIO_SetBits(EPD_DC_PORT, EPD_DC_PIN)
#define DC_L()   GPIO_ResetBits(EPD_DC_PORT, EPD_DC_PIN)
#define RST_H()  GPIO_SetBits(EPD_RST_PORT, EPD_RST_PIN)
#define RST_L()  GPIO_ResetBits(EPD_RST_PORT, EPD_RST_PIN)
#define BUSY()   ((GPIO_ReadInputDataBit(EPD_BUSY_PORT, EPD_BUSY_PIN) != 0) ? 1u : 0u)

#define EPD_SPI_PRESCALER   SPI_BaudRatePrescaler_4
#define EPD_GPIO_SPEED      GPIO_Speed_50MHz

// SPI1_TX 固定使用 DMA1 通道 3。
#define EPD_DMA_CHANNEL     DMA1_Channel3                     // SPI1_TX 固定走 DMA1 通道3
#define EPD_DMA_TC_FLAG     DMA1_FLAG_TC3
#define EPD_DMA_TC_READY()  (DMA_GetFlagStatus(EPD_DMA_TC_FLAG) != RESET)
#define EPD_DMA_TC_CLEAR()  DMA_ClearFlag(EPD_DMA_TC_FLAG)
#define EPD_DMA_MIN_LEN     256                               // 超过这个长度才走DMA

// 通过 SPI 发送一个字节，并读走接收数据。
static void spi_byte(uint8_t v)
{
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET) { }
    SPI_I2S_SendData(SPI1, v);
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) == RESET) { }
    (void)SPI_I2S_ReceiveData(SPI1);
}

// 拉低 DC，向面板发送一字节命令。
static void EPD_Cmd(uint8_t cmd)
{
    DC_L();
    CS_L();
    spi_byte(cmd);
    CS_H();
}

// 拉高 DC，向面板发送一字节数据。
static void EPD_Dat(uint8_t dat)
{
    DC_H();
    CS_L();
    spi_byte(dat);
    CS_H();
}

// 启动一块 SPI DMA 发送，供双缓冲流水使用。
static void epd_dma_start(const uint8_t *buf, uint32_t len)
{
    DMA_Cmd(EPD_DMA_CHANNEL, DISABLE);
    EPD_DMA_TC_CLEAR();
    EPD_DMA_CHANNEL->MADDR = (uint32_t)buf;
    DMA_SetCurrDataCounter(EPD_DMA_CHANNEL, (uint16_t)len);
    DMA_Cmd(EPD_DMA_CHANNEL, ENABLE);
}

// 等待当前 DMA 块发送完成。
static void epd_dma_wait_tc(void)
{
    while (!EPD_DMA_TC_READY()) { }
}

// 等 SPI 最后一字节发完，关闭 DMA 并清接收状态。
static void epd_spi_wait_idle(void)
{
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET) {
    }
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET) {
    }

    DMA_Cmd(EPD_DMA_CHANNEL, DISABLE);
    EPD_DMA_TC_CLEAR();

    // SPI传完要读一下接收
    (void)SPI1->DATAR;
    (void)SPI1->STATR;
}

// 通过 DMA 发送连续数据，并等待 SPI 完全空闲。
static void epd_dma_send(const uint8_t *buf, uint32_t len)
{
    epd_dma_start(buf, len);
    epd_dma_wait_tc();
    epd_spi_wait_idle();
}

// 发送数据；长块用 DMA，短块逐字节发送。
static void epd_send_data_raw(const uint8_t *buf, uint32_t len)
{
    if (len >= EPD_DMA_MIN_LEN) {
        epd_dma_send(buf, len);
    } else {
        for (uint32_t i = 0; i < len; ++i) {
            spi_byte(buf[i]);
        }
    }
}

// 保持 CS 为低，向面板写入一块数据。
static void epd_dat_buf(const uint8_t *buf, uint32_t len)
{
    DC_H();
    CS_L();
    epd_send_data_raw(buf, len);
    CS_H();
}

// 初始化面板引脚、SPI1 和 DMA1 通道 3。
void EPD_Hal_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    SPI_InitTypeDef spi = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO | RCC_APB2Periph_GPIOA, ENABLE);

    // CS / DC / RST 输出
    gpio.GPIO_Pin = EPD_CS_PIN | EPD_DC_PIN | EPD_RST_PIN;
    gpio.GPIO_Speed = EPD_GPIO_SPEED;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(EPD_CS_PORT, &gpio);

    // BUSY 输入下拉
    gpio.GPIO_Pin = EPD_BUSY_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IPD;
    GPIO_Init(EPD_BUSY_PORT, &gpio);

    // SPI1 的 SCK/MOSI 在 PA5/PA7，复位后默认就映射在这里，确认没开重映射
    GPIO_PinRemapConfig(GPIO_Remap_SPI1, DISABLE);

    gpio.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_7;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &gpio);

    CS_H();
    DC_H();
    RST_H();

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE);
    spi.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    spi.SPI_Mode = SPI_Mode_Master;
    spi.SPI_DataSize = SPI_DataSize_8b;
    spi.SPI_CPOL = SPI_CPOL_Low;
    spi.SPI_CPHA = SPI_CPHA_1Edge;
    spi.SPI_NSS = SPI_NSS_Soft;
    spi.SPI_BaudRatePrescaler = EPD_SPI_PRESCALER;
    spi.SPI_FirstBit = SPI_FirstBit_MSB;
    spi.SPI_CRCPolynomial = 7;
    SPI_Init(SPI1, &spi);
    SPI_Cmd(SPI1, ENABLE);

    // DMA：DMA1 通道3 固定映射到 SPI1_TX
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    {
        DMA_InitTypeDef dma = {0};

        DMA_DeInit(EPD_DMA_CHANNEL);
        dma.DMA_PeripheralBaseAddr = (uint32_t)&SPI1->DATAR;
        dma.DMA_MemoryBaseAddr = 0;
        dma.DMA_DIR = DMA_DIR_PeripheralDST;
        dma.DMA_BufferSize = 0;
        dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
        dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
        dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
        dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
        dma.DMA_Mode = DMA_Mode_Normal;
        dma.DMA_Priority = DMA_Priority_VeryHigh;
        dma.DMA_M2M = DMA_M2M_Disable;
        DMA_Init(EPD_DMA_CHANNEL, &dma);
    }
    SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Tx, ENABLE);
}

// 每像素净驱动帧数限制：黑方向不超过 +7，白方向不超过 -7。
#ifndef EPD_PULSE_BUDGET
#define EPD_PULSE_BUDGET 7
#endif
_Static_assert(EPD_PULSE_BUDGET >= 1 && EPD_PULSE_BUDGET <= 7, "pulse budget must fit signed nibble");
#define EPD_DC_BLACK        EPD_PULSE_BUDGET
#define EPD_DC_WHITE        EPD_PULSE_BUDGET
#define EPD_RESET_HIGH_MS   50     // 保持 RST 高电平的时间 (ms)
#define EPD_RESET_LOW_MS    20     // 拉低 RST 的时间 (ms)
#define EPD_HOLD_ENDING     1      // 1 = 最后一帧驱动完后补一次全0电平扫描放电。

#define EPD_PIXELS          (EPD_W * EPD_H)

static const uint8_t lut_full[159] =
{
0x80,	0x48,	0x40,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x40,	0x48,	0x80,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x80,	0x48,	0x40,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x40,	0x48,	0x80,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x9,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x5,	0x1,	0x0,	0x5,	0x1,	0x0,	0x3,
0x9,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x22,	0x22,	0x22,	0x22,	0x22,	0x22,	0x0,	0x0,	0x0,
0x22,	0x17,	0x41,	0x0,	0x32,	0x20                          //EOPT VGH VSH1 VSH2 VSL VCOM
};

// Fast waveform: the vendor partial table with the two HOLD rows silenced.
// Level rows are  00 = hold, 01 = drive white, 10 = drive black, 11 = hold,
// so a pixel that is in neither mask keeps its current colour.
static const uint8_t lut_fast_base[159] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,  // 00  LUT0
    0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,  // 01 white LUT1
    0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,  // 10 black LUT2
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,  // 11 hold  LUT3
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, //LUT4
    0x01,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00, //1A 1B SRAB 1C 1D SRCD RP
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x77,0x77,0x77,0x77,0x77,0x77,0x00,0x00,0x00, //frame rate and xon
    0x02,0x17,0x4B,0xb0,0x3A,0x28,//EOPT VGH VSH1 VSH2 VSL VCOM
};

static uint8_t analog_on;          // 升压电路是否开启
static const uint8_t s_dc_black = EPD_DC_BLACK;
static const uint8_t s_dc_white = EPD_DC_WHITE;

// 每像素净驱动帧数：正数 = 黑方向，负数 = 白方向，限制在 [-7,+7]。
// 4 bit 有符号补码（-8..+7），两个像素共用 1 字节。
#define EPD_NET_BYTES    (EPD_PIXELS / 2u)

// 后面会按 32-bit 读取 4 个 net 字节，保证 4 字节对齐。
static uint8_t s_net[EPD_NET_BYTES] __attribute__((aligned(4)));

// 掩码双缓冲 2 x 64 字节。
#define EPD_PLANE_CHUNK   64u
static uint8_t s_plane_buf[2][EPD_PLANE_CHUNK] __attribute__((aligned(4)));

//驱动掩码的LUT，上电后生成
static uint8_t s_mask_lut[2][256];

// 检测本帧是否存在需要驱动的像素的记录点
static uint32_t s_probe_pos;

static uint8_t s_hold_pending;  // 上次驱动后是否还需 HOLD 放电

// 将 4 位补码还原为有符号净驱动量。
static inline int epd_signed_nibble(uint8_t v)
{
    v &= 0x0Fu;
    return (v & 0x08u) ? ((int)v - 16) : (int)v;
}

// 按净驱动预算建立黑、白方向的允许驱动掩码表。
static void epd_build_mask_lut(void)
{
    const int hi_black = (int)s_dc_black - (int)1;
    const int lo_white = -(int)s_dc_white + (int)1;

    for (uint16_t v = 0; v < 256u; ++v) {
        const int n0 = epd_signed_nibble((uint8_t)v);
        const int n1 = epd_signed_nibble((uint8_t)(v >> 4));
        uint8_t black_ok = 0;
        uint8_t white_ok = 0;

        if (n0 <= hi_black) {
            black_ok |= 0x01u;
        }
        if (n1 <= hi_black) {
            black_ok |= 0x02u;
        }
        if (n0 >= lo_white) {
            white_ok |= 0x01u;
        }
        if (n1 >= lo_white) {
            white_ok |= 0x02u;
        }

        s_mask_lut[0][v] = black_ok;
        s_mask_lut[1][v] = white_ok;
    }
}

// 读取八个像素的净驱动量，合成指定方向的允许驱动掩码。
static inline uint8_t epd_drive_mask_byte(const uint8_t *cell, uint8_t plane)
{
    const uint8_t p0 = s_mask_lut[plane][cell[0]];
    const uint8_t p1 = s_mask_lut[plane][cell[1]];
    const uint8_t p2 = s_mask_lut[plane][cell[2]];
    const uint8_t p3 = s_mask_lut[plane][cell[3]];

    return (uint8_t)(((p0 & 0x01u) << 7) | (((p0 >> 1) & 0x01u) << 6) |
                     ((p1 & 0x01u) << 5) | (((p1 >> 1) & 0x01u) << 4) |
                     ((p2 & 0x01u) << 3) | (((p2 >> 1) & 0x01u) << 2) |
                     ((p3 & 0x01u) << 1) |  ((p3 >> 1) & 0x01u));
}

// 判断八个目标像素是否还需驱动，跳过已经同向饱和的整字节。
static inline int epd_frame_byte_needs_drive(const uint8_t *next, uint32_t frame_index)
{
    const uint8_t px = next[frame_index];
    const uint8_t *const cell = &s_net[frame_index * 4u];
    uint32_t net_word;

    memcpy(&net_word, cell, sizeof(net_word));
    if ((px == 0x00u) && (net_word == 0x77777777u)) {
        return 0;
    }
    if ((px == 0xFFu) && (net_word == 0x99999999u)) {
        return 0;
    }

    {
        const uint8_t black = (uint8_t)(epd_drive_mask_byte(cell, 0u) & (uint8_t)~px);
        const uint8_t white = (uint8_t)(epd_drive_mask_byte(cell, 1u) & px);

        return (uint8_t)(black | white) != 0u;
    }
}

// 全刷后按实际画面设置正负饱和值，避免继续驱动未变的像素。
static void epd_net_set_from_image(const uint8_t *image)
{
    const uint8_t black_nibble = (uint8_t)((int)s_dc_black & 0x0F);
    const uint8_t white_nibble = (uint8_t)((-(int)s_dc_white) & 0x0F);

    for (uint32_t i = 0; i < EPD_FRAME_BYTES; ++i) {
        const uint8_t v = image[i];
        uint8_t *const cell = &s_net[i * 4u];

        for (uint8_t b = 0; b < 4u; ++b) {
            const uint8_t m0 = (uint8_t)(0x80u >> (b * 2u));
            const uint8_t m1 = (uint8_t)(0x40u >> (b * 2u));
            const uint8_t n0 = ((v & m0) != 0u) ? white_nibble : black_nibble;
            const uint8_t n1 = ((v & m1) != 0u) ? white_nibble : black_nibble;

            cell[b] = (uint8_t)(n0 | (uint8_t)(n1 << 4));
        }
    }

    s_probe_pos = 0;
}

// 等待面板空闲；硬件异常时停在这里。
static void epd_wait(void)
{
    while (BUSY()) platform_wait(platform_ticks() + platform_tick_hz(), 1);
}

// 关闭已经开启的升压电路，并等待完成。
static void epd_analog_off(void)
{
    if (!analog_on) {
        return;
    }
    EPD_Cmd(0x22);
    EPD_Dat(0x03);
    EPD_Cmd(0x20);
    epd_wait();
    analog_on = 0;
}

// 开启面板升压电路，供连续快刷使用。
static void epd_analog_on(void)
{
    EPD_Cmd(0x22);
    EPD_Dat(0xC0);
    EPD_Cmd(0x20);
    epd_wait();
    analog_on = 1;
}

// 关闭升压后硬件复位面板，等待 BUSY 释放。
static void epd_reset(void)
{
    epd_analog_off();
    RST_H();
    platform_delay_ms(EPD_RESET_HIGH_MS);
    RST_L();
    platform_delay_ms(EPD_RESET_LOW_MS);
    RST_H();
    platform_delay_ms(EPD_RESET_HIGH_MS);
    analog_on = 0;
    epd_wait();
}

// 写入波形表及配套的驱动电压参数。
static void epd_load_lut(const uint8_t *lut)
{
    EPD_Cmd(0x32);
    epd_dat_buf(lut, 153);
    epd_wait();

    EPD_Cmd(0x3F);
    EPD_Dat(lut[153]);
    EPD_Cmd(0x03);
    EPD_Dat(lut[154]);
    EPD_Cmd(0x04);
    EPD_Dat(lut[155]);
    EPD_Dat(lut[156]);
    EPD_Dat(lut[157]);
    EPD_Cmd(0x2C);
    EPD_Dat(lut[158]);
}

// 设置面板 RAM 窗口和写入起点，按 X、Y 递减映射逻辑画面。
static void epd_window(uint8_t row0, uint8_t row1, uint8_t col0, uint8_t col1)
{
    const uint8_t x0 = (uint8_t)(EPD_STRIDE - 1 - col0);
    const uint8_t x1 = (uint8_t)(EPD_STRIDE - 1 - col1);
    const uint8_t y0 = (uint8_t)(EPD_H - 1 - row0);
    const uint8_t y1 = (uint8_t)(EPD_H - 1 - row1);

    EPD_Cmd(0x44);              // X 起止（字节列），递减
    EPD_Dat(x0);
    EPD_Dat(x1);
    EPD_Cmd(0x45);              // Y 起止，递减
    EPD_Dat(y0);
    EPD_Dat(0x00);
    EPD_Dat(y1);
    EPD_Dat(0x00);
    EPD_Cmd(0x4E);              // 光标 X = 起始列
    EPD_Dat(x0);
    EPD_Cmd(0x4F);              // 光标 Y = 起始行
    EPD_Dat(y0);
    EPD_Dat(0x00);
}

// 将 RAM 窗口设为整个 152×152 面板。
static void epd_window_full(void)
{
    epd_window(0, (uint8_t)(EPD_H - 1), 0, (uint8_t)(EPD_STRIDE - 1));
}

// 用完整波形全刷一帧，重置像素净驱动预算。
void EPD_SendDisplay(const uint8_t *image)
{
    epd_reset();

    EPD_Cmd(0x12);
    platform_delay_ms(10);
    epd_wait();

    EPD_Cmd(0x01);
    EPD_Dat(0x97);
    EPD_Dat(0x00);
    EPD_Dat(0x00);
    EPD_Cmd(0x11);
    EPD_Dat(0x00);
    epd_window_full();
    EPD_Cmd(0x3C);
    EPD_Dat(0x01);
    EPD_Cmd(0x18);
    EPD_Dat(0x80);

    EPD_Cmd(0x22);          // load temperature
    EPD_Dat(0xB1);
    EPD_Cmd(0x20);
    epd_wait();

    epd_load_lut(lut_full);

    epd_window_full();
    EPD_Cmd(0x24);
    epd_dat_buf(image, EPD_FRAME_BYTES);

    epd_window_full();
    EPD_Cmd(0x26);
    epd_dat_buf(image, EPD_FRAME_BYTES);

    EPD_Cmd(0x22);
    EPD_Dat(0xC7);
    EPD_Cmd(0x20);
    epd_wait();
    analog_on = 0;
    epd_net_set_from_image(image);
    s_hold_pending = 0;
}

// 复位面板、载入快刷波形并保持升压电路开启。
void EPD_FastPrepare(void)
{
    epd_build_mask_lut();
    epd_reset();           // hardware reset

    EPD_Cmd(0x01);
    EPD_Dat(0x97);
    EPD_Dat(0x00);
    EPD_Dat(0x00);
    EPD_Cmd(0x11);
    EPD_Dat(0x00);
    EPD_Cmd(0x18);
    EPD_Dat(0x80);
    epd_load_lut(lut_fast_base);

    EPD_Cmd(0x37);          // no RAM ping-pong
    for (uint8_t i = 0; i < 10u; ++i) {
        EPD_Dat(0x00);
    }

    EPD_Cmd(0x3C);
    EPD_Dat(0x80);

    epd_analog_on();
}

// 从上次找到的位置继续寻找需要驱动的像素，找到一个即可返回。
static int epd_probe_for_drive(const uint8_t *next)
{
    uint32_t start = s_probe_pos;
    uint32_t found = EPD_FRAME_BYTES;

    for (uint32_t p = start; p < EPD_FRAME_BYTES; ++p) {
        if (epd_frame_byte_needs_drive(next, p)) {
            found = p;
            break;
        }
    }

    if (found == EPD_FRAME_BYTES) {
        for (uint32_t p = 0; p < start; ++p) {
            if (epd_frame_byte_needs_drive(next, p)) {
                found = p;
                break;
            }
        }
    }

    if (found == EPD_FRAME_BYTES) {
        return 0;
    }

    // 下次仍然从这个像素位置附近开始探测
    s_probe_pos = found;
    return 1;
}

// 提交扫描后更新每像素净驱动量；4 位补码限制在 [-7,+7]。
static void epd_apply_charge(const uint8_t *next)
{

    for (uint32_t i = 0; i < EPD_FRAME_BYTES; ++i) {
        const uint8_t px = next[i];
        uint8_t *const cell = &s_net[i * 4u];
        const uint8_t black = (uint8_t)(epd_drive_mask_byte(cell, 0u) & (uint8_t)~px);
        const uint8_t white = (uint8_t)(epd_drive_mask_byte(cell, 1u) & px);
        const uint8_t active = (uint8_t)(black | white);

        if (active == 0u) {
            continue;
        }

        for (uint8_t b = 0; b < 4u; ++b) {
            const uint8_t m0 = (uint8_t)(0x80u >> (b * 2u));
            const uint8_t m1 = (uint8_t)(0x40u >> (b * 2u));

            if ((active & (m0 | m1)) == 0u) {
                continue;
            }

            {
                const uint8_t nb = cell[b];
                uint8_t n0 = (uint8_t)(nb & 0x0Fu);
                uint8_t n1 = (uint8_t)(nb >> 4);

                if ((black & m0) != 0u) {
                    n0 = (uint8_t)((n0 + 1) & 0x0Fu);
                } else if ((white & m0) != 0u) {
                    n0 = (uint8_t)((n0 - 1) & 0x0Fu);
                }

                if ((black & m1) != 0u) {
                    n1 = (uint8_t)((n1 + 1) & 0x0Fu);
                } else if ((white & m1) != 0u) {
                    n1 = (uint8_t)((n1 - 1) & 0x0Fu);
                }

                cell[b] = (uint8_t)(n0 | (uint8_t)(n1 << 4));
            }
        }
    }
}

// 生成一块黑或白驱动掩码；all_zero 用于 HOLD 放电收尾。
static void epd_build_plane_chunk(uint8_t *dst, uint32_t frame_offset, uint32_t len,
                                  const uint8_t *next, uint8_t plane, uint8_t all_zero)
{
    if (all_zero != 0u) {
        for (uint32_t i = 0; i < len; ++i) {
            dst[i] = 0x00;
        }
        return;
    }

    for (uint32_t i = 0; i < len; ++i) {
        const uint32_t frame_index = frame_offset + i;
        const uint8_t px = next[frame_index];
        const uint8_t *const cell = &s_net[frame_index * 4u];
        uint32_t net_word;

        // 该平面没有目标像素时直接清零。
        if (((plane == 0u) && (px == 0xFFu)) ||
            ((plane != 0u) && (px == 0x00u))) {
            dst[i] = 0x00;
            continue;
        }

        memcpy(&net_word, cell, sizeof(net_word));

        // 跳过已同向饱和的整字节。
        if ((plane == 0u) && (net_word == 0x77777777u)) {
            dst[i] = 0x00;     // 全黑饱和
            continue;
        }
        if ((plane != 0u) && (net_word == 0x99999999u)) {
            dst[i] = 0x00;      // 全白饱和
            continue;
        }

        {
            const uint8_t drive = epd_drive_mask_byte(cell, plane);
            const uint8_t target = (plane == 0u) ? (uint8_t)~px : px;

            dst[i] = (uint8_t)(drive & target);
        }
    }
}

// 交替生成和 DMA 发送两个小缓冲，传送完整黑或白平面。
static void epd_send_generated_plane(uint8_t cmd, uint8_t plane,
                                     const uint8_t *next,
                                     uint8_t all_zero)
{
    const uint32_t total = EPD_FRAME_BYTES;
    const uint32_t cur_len = EPD_PLANE_CHUNK;
    uint8_t cur = 0;

    EPD_Cmd(cmd);
    DC_H();
    CS_L();

    epd_build_plane_chunk(s_plane_buf[cur], 0, cur_len,
                          next, plane, all_zero);
    epd_dma_start(s_plane_buf[cur], cur_len);
    uint32_t sent = cur_len;

    while (sent < total) {
        const uint32_t left = total - sent;
        const uint32_t next_len = (left > EPD_PLANE_CHUNK) ? EPD_PLANE_CHUNK : left;
        const uint8_t next_buf = (uint8_t)(cur ^ 1u);

        // 当前块正在 DMA 发送，这里先算下一块。
        epd_build_plane_chunk(s_plane_buf[next_buf], sent, next_len,
                              next, plane, all_zero);

        epd_dma_wait_tc();
        epd_dma_start(s_plane_buf[next_buf], next_len);

        cur = next_buf;
        sent += next_len;
    }

    epd_dma_wait_tc();
    epd_spi_wait_idle();
    CS_H();
}

// 依次发送当前画面的黑、白驱动平面。
static void epd_send_planes(const uint8_t *next)
{
    epd_send_generated_plane(0x26, 0u, next, 0u);
    epd_send_generated_plane(0x24, 1u, next, 0u);
}

// 等待上一轮面板扫描结束。
void EPD_FastSync(void)
{
    epd_wait();
}

// 触发一轮扫描并保持升压开启，面板刷新时 CPU 可继续解码。
static void epd_activate(void)
{
    EPD_Cmd(0x22);
    EPD_Dat(0x0C);
    EPD_Cmd(0x20);
}

// 提交需要驱动的画面；全部饱和后补一次 HOLD，返回是否仍有像素驱动。
int EPD_FastFrame(const uint8_t *next)
{
    const int driven = epd_probe_for_drive(next);

    if (driven) {
        EPD_FastSync();
        epd_send_planes(next);
        epd_activate();
        s_hold_pending = 1;
        epd_apply_charge(next);
    } else if (EPD_HOLD_ENDING && s_hold_pending) {
        EPD_FastSync();
        epd_send_generated_plane(0x26, 0u, next, 1u);
        epd_send_generated_plane(0x24, 1u, next, 1u);
        epd_activate();

        s_hold_pending = 0;
    }

    return driven;
}

// 读取面板 BUSY 引脚。
uint8_t EPD_IsBusy(void) { return BUSY(); }
