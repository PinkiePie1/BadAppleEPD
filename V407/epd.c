#include "app_config.h"
#include "epd_fast.h"
#include "board.h"
#include <string.h>

// 全刷波形与电压参数，保留原驱动数值。
static const uint8_t lut_full[159] = {
    0x80, 0x48, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x48, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x48, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x48, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x01, 0x00, 0x05, 0x01,
    0x00, 0x03, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00, 0x22, 0x17, 0x41,
    0x00, 0x32, 0x20,
};

// 快刷波形只驱动黑白掩码，其余像素保持。
static const uint8_t lut_fast_base[159] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x77, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00, 0x02, 0x17, 0x4b,
    0xb0, 0x3a, 0x28,
};

/* 八个像素共用四个字节；四个位平面分别连续存放，便于 RVV 连续访问。 */
static uint8_t net[4][EPD_FRAME_BYTES] __attribute__((aligned(8)));
static uint8_t active_rows[EPD_DIRTY_BYTES];
static uint8_t pending, analog_on, hold_pending;
static uint8_t old_first, old_last;
static uint8_t plane_buf[2][EPD_FRAME_BYTES] __attribute__((aligned(8)));
static uint8_t panel_masks[2][EPD_FRAME_BYTES] __attribute__((aligned(8)));
static uint8_t masks_valid;

// 复位、全刷或原始 RAM 写入后，强制重新建立控制器掩码镜像。
static void masks_forget(void)
{
    masks_valid = 0;
}

// 比较并更新对应 RAM 区间；两个平面分别判断是否必须发送。
static int mask_changed(unsigned plane, unsigned offset, unsigned length)
{
    int changed = epd_blit_changed(panel_masks[plane] + offset, plane_buf[plane], length, 0);
    return !masks_valid || changed;
}

/* 净驱动计算 */

// 按目标颜色和净驱动预算，生成八个像素的黑、白驱动掩码。
static inline void net_masks(unsigned index, uint8_t target, uint8_t *black, uint8_t *white)
{
    *black = (uint8_t)(~target & ~(net[0][index] & net[1][index] & net[2][index] & net[3][index]));
    *white = (uint8_t)(target & (net[1][index] | net[2][index] | net[3][index]));
}

// 把一段行标为待检查；手工改帧缓存时使用。
void EPD_InvalidateRows(unsigned y, unsigned height)
{
    if (y >= EPD_H) return;
    if (height > EPD_H - y) height = EPD_H - y;
    for (unsigned end = y + height; y < end; ++y)
        active_rows[y >> 3] |= (uint8_t)(1u << (y & 7));
}

// 按已显示图像初始化饱和净驱动量；传空指针则从零驱动量开始。
static void net_begin(const uint8_t *shown)
{
    for (unsigned i = 0; i < EPD_FRAME_BYTES; ++i) {
        for (unsigned bit = 0; bit < 4; ++bit) {
            uint8_t black = (15u & (1u << bit)) ? 0xff : 0;
            uint8_t white = (1u & (1u << bit)) ? 0xff : 0;
            net[bit][i] = shown ? (uint8_t)((black & ~shown[i]) | (white & shown[i]))
                               : (bit == 3 ? 0xff : 0);
        }
    }
    memset(active_rows, shown ? 0 : 0xff, sizeof(active_rows));
}

// 只检查标记行，去掉已收敛行并返回还需驱动的首尾行。
static int find_drive(const uint8_t *next, uint8_t *first, uint8_t *last)
{
    int found = 0;
    for (unsigned y = 0; y < EPD_H; ++y) {
        if (!(active_rows[y >> 3] & (1u << (y & 7)))) continue;
        unsigned i = y * EPD_STRIDE, end = i + EPD_STRIDE;
        for (; i < end; ++i) {
            uint8_t black, white;
            net_masks(i, next[i], &black, &white);
            if (black | white) break;
        }
        if (i < end) {
            if (!found) *first = (uint8_t)y;
            *last = (uint8_t)y;
            found = 1;
        } else {
            active_rows[y >> 3] &= (uint8_t)~(1u << (y & 7));
        }
    }
    return found;
}

/* 屏幕刷新 */
// SSD1681：X 字节列 0→24，Y 从 199-first 递减到 199-last。
// 帧缓存仍按行优先、MSB 在左；每次写 RAM 前分别重设窗口与光标。
static void set_window(uint8_t first, uint8_t last)
{
    EPD_Cmd(0x44);
    EPD_Dat(0);
    EPD_Dat(EPD_STRIDE - 1);
    EPD_Cmd(0x45);
    EPD_Dat(EPD_H - 1 - first);
    EPD_Dat(0);
    EPD_Dat(EPD_H - 1 - last);
    EPD_Dat(0);
    EPD_Cmd(0x4e);
    EPD_Dat(0);
    EPD_Cmd(0x4f);
    EPD_Dat(EPD_H - 1 - first);
    EPD_Dat(0);
}

// 设置分辨率、RAM 方向和温度来源，恢复整屏 RAM 窗口。
static void panel_setup(void)
{
    EPD_Cmd(0x01);
    EPD_Dat((EPD_H - 1) & 0xff);
    EPD_Dat((EPD_H - 1) >> 8);
    EPD_Dat(0x01); // GDR：沿用 SmoothEPD 的 SSD1681 扫描方向
    EPD_Cmd(0x11);
    EPD_Dat(0x01); // X 递增、Y 递减，X 优先
    EPD_Cmd(0x18);
    EPD_Dat(0x80);
    set_window(0, EPD_H - 1);
}

// 写入 159 字节波形表及对应的驱动电压参数。
static void load_lut(const uint8_t *lut)
{
    EPD_Cmd(0x32);
    EPD_SendData(lut, 153);
    EPD_WaitBusy(4u * EPD_TICK_HZ, 2);
    EPD_Cmd(0x3f);
    EPD_Dat(lut[153]);
    EPD_Cmd(0x03);
    EPD_Dat(lut[154]);
    EPD_Cmd(0x04);
    EPD_SendData(lut + 155, 3);
    EPD_Cmd(0x2c);
    EPD_Dat(lut[158]);
}

// 等待上一次已提交的面板扫描完成，清除 pending 标记。
void EPD_FastSync(void)
{
    if (pending) {
        EPD_WaitBusy(EPD_TICK_HZ, EPD_BUSY_SLEEP);
        pending = 0;
    }
}

// 开启或关闭屏幕升压和振荡器，状态相同时不重复发送。
static void analog(int on)
{
    if (analog_on == on) return;
    EPD_Cmd(0x22);
    EPD_Dat(on ? 0xc0 : 0x03);
    EPD_Cmd(0x20);
    EPD_WaitBusy(4u * EPD_TICK_HZ, 2);
    analog_on = (uint8_t)on;
}

// 发送刷新序列并启动扫描，记录面板还有一次扫描待完成。
static void activate(uint8_t mode)
{
    EPD_Cmd(0x22);
    EPD_Dat(mode);
    EPD_Cmd(0x20);
    pending = 1;
}

// 等待当前扫描、关闭升压，硬复位和软复位后配置屏幕。
void EPD_Init(void)
{
    masks_forget();
    EPD_FastSync();
    analog(0);
    EPD_HardReset();
    pending = hold_pending = 0;
    EPD_WaitBusy(4u * EPD_TICK_HZ, 2);
    EPD_Cmd(0x12);
    EPD_DelayMs(10);
    EPD_WaitBusy(4u * EPD_TICK_HZ, 2);
    panel_setup();
    EPD_Cmd(0x3c);
    EPD_Dat(0x01);
}

// 以已显示画面建立净驱动状态，并清零快刷计数。
void EPD_FastBegin(const uint8_t *shown)
{
    EPD_FastSync();
    net_begin(shown);
}

// 执行完整刷新，完成后把图像作为已显示的净驱动基准。
void EPD_SendDisplay(const uint8_t *image)
{
    EPD_Init();
    EPD_Cmd(0x22);
    EPD_Dat(0xb1);
    EPD_Cmd(0x20);
    EPD_WaitBusy(4u * EPD_TICK_HZ, 2);
    load_lut(lut_full);
    set_window(0, EPD_H - 1);
    EPD_Cmd(0x24);
    EPD_SendData(image, EPD_FRAME_BYTES);
    set_window(0, EPD_H - 1);
    EPD_Cmd(0x26);
    EPD_SendData(image, EPD_FRAME_BYTES);
    activate(0xc7);
    EPD_WaitBusy(4u * EPD_TICK_HZ, 2);
    pending = 0;
    analog_on = 0;
    EPD_FastBegin(image);
}

static void build_plane(uint8_t *dst, unsigned offset, unsigned len,
                        const uint8_t *next, unsigned plane, int zero)
{
    if (zero) { memset(dst, 0, len); return; }
    if (plane) epd_white7(dst, next + offset, net, offset, len);
    else epd_black7(dst, next + offset, net, offset, len);
}

// 只写变化的 RAM 平面；黑平面 DMA 期间计算白平面，每次都推进净驱动。
static int send_planes(const uint8_t *next, int zero, uint8_t first, uint8_t last)
{
    unsigned offset = (unsigned)first * EPD_STRIDE;
    unsigned total = (unsigned)(last - first + 1) * EPD_STRIDE;
    build_plane(plane_buf[0], offset, total, next, 0, zero);
    int black_changed = mask_changed(0, offset, total);
    if (black_changed) {
        set_window(first, last);
        EPD_Cmd(0x26);
        EPD_DataBegin();
        EPD_DMAStart(plane_buf[0], total);
    }
    build_plane(plane_buf[1], offset, total, next, 1, zero);
    int white_changed = mask_changed(1, offset, total);
    if (black_changed) {
        EPD_DMAWait();
        EPD_DataEnd();
    }
    if (white_changed) {
        set_window(first, last);
        EPD_Cmd(0x24);
        EPD_SendData(plane_buf[1], total);
    }
    if (offset == 0 && total == EPD_FRAME_BYTES) masks_valid = 1;
    return black_changed || white_changed;
}

// 装入快刷波形并清零两块控制器 RAM，升压留到首次刷新再开。
void EPD_FastPrepare(void)
{
    masks_forget();
    EPD_FastSync();
    analog(0);
    EPD_HardReset();
    EPD_WaitBusy(4u * EPD_TICK_HZ, 2);
    panel_setup();
    load_lut(lut_fast_base);
    EPD_Cmd(0x37);
    for (unsigned i = 0; i < 10; ++i) EPD_Dat(0);
    EPD_Cmd(0x3c);
    EPD_Dat(0x80);
    send_planes(0, 1, 0, EPD_H - 1);
    set_window(0, EPD_H - 1);
    hold_pending = 0;
}

// 推进待检查行的一次扫描或 HOLD；已收敛时返回 0，不再驱动。
int EPD_FastStep(const uint8_t *next)
{
    EPD_FastSync();
    uint8_t first = 0, last = 0;
    int driven = find_drive(next, &first, &last);
    if (!driven && !hold_pending) return 0;
    analog(1);
    uint8_t current_first = first, current_last = last;
    // 覆盖上次驱动行，擦掉旧掩码，否则移动后的旧位置还会继续通电。
    if (hold_pending) {
        if (!driven || old_first < first) first = old_first;
        if (!driven || old_last > last) last = old_last;
    }
    if (send_planes(next, !driven, first, last)) set_window(0, EPD_H - 1);
    activate(0x0c);
    hold_pending = (uint8_t)driven;
    old_first = current_first;
    old_last = current_last;
    return 1;
}

// 等扫描完成，必要时补 HOLD，再关升压；保留 RAM、LUT 和净驱动状态。
void EPD_Idle(void)
{
    EPD_FastSync();
    if (hold_pending) {
        if (send_planes(0, 1, old_first, old_last)) set_window(0, EPD_H - 1);
        activate(0x0c);
        EPD_FastSync();
        hold_pending = 0;
    }
    analog(0);
}
