#include "epd_fast.h"
#include <string.h>

#include <riscv_vector.h>
#define V vuint8m4_t
#define SPLAT(v) __riscv_vmv_v_x_u8m4((v),vl)
#define LOAD(p) __riscv_vle8_v_u8m4((p), vl)
#define STORE(p, v) __riscv_vse8_v_u8m4((p), (v), vl)
#define AND(a,b) __riscv_vand_vv_u8m4((a),(b),vl)
#define OR(a,b) __riscv_vor_vv_u8m4((a),(b),vl)
#define XOR(a,b) __riscv_vxor_vv_u8m4((a),(b),vl)
#define NOT(a) __riscv_vnot_v_u8m4((a),vl)
#define DIFFERENT(a,b) (__riscv_vcpop_m_b2(__riscv_vmsne_vv_u8m4_b2((a),(b),vl),vl) != 0)
#define SETVL(n) __riscv_vsetvl_e8m4(n)

// 生成默认黑色预算的整段黑掩码，不修改净驱动。
void epd_black7(uint8_t *restrict dst, const uint8_t *restrict target,
                const uint8_t net[restrict 4][EPD_FRAME_BYTES], size_t offset, size_t n)
{
    while (n) {
        size_t vl = SETVL(n);
        V q0=LOAD(net[0]+offset), q1=LOAD(net[1]+offset);
        V q2=LOAD(net[2]+offset), q3=LOAD(net[3]+offset), t=LOAD(target);
        STORE(dst, AND(NOT(t), NOT(AND(AND(q0,q1), AND(q2,q3)))));
        offset += vl;
        target += vl;
        dst += vl;
        n -= vl;
    }
}

// 生成白掩码，同时把四个位平面的净驱动推进一次。
void epd_white7(uint8_t *restrict dst, const uint8_t *restrict target,
                uint8_t net[restrict 4][EPD_FRAME_BYTES], size_t offset, size_t n)
{
    while (n) {
        size_t vl=SETVL(n);
        V q0=LOAD(net[0]+offset), q1=LOAD(net[1]+offset);
        V q2=LOAD(net[2]+offset), q3=LOAD(net[3]+offset), t=LOAD(target);
        V b=AND(NOT(t),NOT(AND(AND(q0,q1),AND(q2,q3))));
        V w=AND(t,OR(OR(q1,q2),q3));
        STORE(dst,w);
#define ADV(bit) do { STORE(net[bit]+offset,XOR(XOR(q##bit,b),w)); \
        b=AND(b,q##bit); w=AND(w,NOT(q##bit)); } while (0)
        ADV(0);
        ADV(1);
        ADV(2);
        ADV(3);
#undef ADV
        offset += vl;
        target += vl;
        dst += vl;
        n -= vl;
    }
}

// 复制或合成连续位图字节，返回这一段是否实际变化。
int epd_blit_changed(uint8_t *restrict dst,const uint8_t *restrict src,size_t n,unsigned mode)
{
    int changed=0;
    while (n) {
        size_t vl=SETVL(n);
        V old=LOAD(dst), next=LOAD(src);
        if (mode == 1) next=NOT(next);
        else if (mode == 2) next=OR(old,next);
        else if (mode == 3) next=AND(old,NOT(next));
        changed |= DIFFERENT(old, next);
        STORE(dst, next);
        dst += vl;
        src += vl;
        n -= vl;
    }
    return changed;
}
