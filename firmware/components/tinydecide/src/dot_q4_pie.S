// ESP32-S3 PIE (Processor Instruction Extensions) kernel for the v3 Q4_0
// row-planar dot product against Q8_0-quantized activations.
//
//   float dot_q4q8_pie(const uint8_t* nib,     // a2: 16B-aligned nibble plane
//                      const int8_t*  xq,      // a3: 16B-aligned int8 activations
//                      const uint8_t* scales,  // a4: bf16 weight scales (2B each)
//                      const float*   xs,      // a5: fp32 activation scales
//                      int            nb);     // a6: number of 32-weight blocks
//
// Per block: 16 nibble bytes hold q[0..15] (low nibbles) and q[16..31] (high
// nibbles). Both unpacked vectors get the -8 offset, then two EE.VMULAS.S8
// steps accumulate 32 signed int8 MACs into the 40-bit ACCX. The block sum
// (max |sum| = 32*8*127, fits 32 bits) is scaled by bf16(wscale)*xs[b] on the
// scalar FPU. Accumulation order matches the scalar kernel in engine.cpp:
// one float madd per block, so the two implementations agree to float
// rounding (the integer part is exact).
//
// Invariants relied on:
//   - nib and xq are 16-byte aligned (EE.VLD.128 faults otherwise; model.bin's Q4
//     offsets and the engine's scratch layout guarantee this).
//   - Exactly one task per core runs this kernel (the caller + td_mm), so the
//     per-core QACC/ACCX/QR register state is never shared. ACCX is zeroed
//     per block and no state survives across calls.
//   - SAR is set once per call (ssai 4); nothing in the loop clobbers it.
//
// Assembled for the ESP32-S3 only; other chips use the scalar kernel in engine.cpp.
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S3

    .section .rodata
    .align  4
.Lconst:
    .byte   0x0F        // nibble mask, broadcast to all 16 lanes
    .byte   0x08        // the Q4_0 -8 offset, broadcast

    .text
    .align  4
    .global dot_q4q8_pie
    .type   dot_q4q8_pie, @function
dot_q4q8_pie:
    entry   a1, 32
    movi    a8, .Lconst
    ee.vldbc.8 q7, a8           // q7 = 0x0F splat
    addi    a8, a8, 1
    ee.vldbc.8 q6, a8           // q6 = 8 splat
    ssai    4                   // SAR = 4 for the high-nibble shift
    movi    a8, 0
    wfr     f0, a8              // acc = 0.0f
    loopnez a6, .Lloop_end
    ee.vld.128.ip q0, a2, 16    // 16 packed nibble bytes
    ee.vsr.32   q1, q0          // >> 4 (32-bit lanes; garbage masked next)
    ee.andq     q0, q0, q7      // low nibbles  -> q[0..15]
    ee.andq     q1, q1, q7      // high nibbles -> q[16..31]
    ee.vsubs.s8 q0, q0, q6      // -8
    ee.vsubs.s8 q1, q1, q6
    ee.vld.128.ip q2, a3, 16    // xq[0..15]
    ee.vld.128.ip q3, a3, 16    // xq[16..31]
    ee.zero.accx
    ee.vmulas.s8.accx q0, q2
    ee.vmulas.s8.accx q1, q3
    rur.accx_0  a7              // int32 block sum (exact)
    float.s     f2, a7, 0       // (float)isum
    l16ui       a8, a4, 0       // bf16 weight scale bits
    addi        a4, a4, 2
    slli        a8, a8, 16
    wfr         f3, a8          // wscale
    lsip        f4, a5, 4       // xs[b], post-increment
    mul.s       f3, f3, f4      // wscale * xscale
    madd.s      f0, f2, f3      // acc += isum * (wscale*xscale)
.Lloop_end:
    rfr     a2, f0
    retw.n
    .size   dot_q4q8_pie, . - dot_q4q8_pie

// The same dot product for tiles of four tokens: each weight block is unpacked once and applied to
// all four, and the loop over tiles runs here rather than in C.
//
//   void dot4_q4q8_pie(const uint8_t* nib,     // a2: 16B-aligned nibble plane
//                      const uint8_t* scales,  // a3: bf16 weight scales
//                      const int8_t*  xq,      // a4: token 0's int8 activations; 16B-aligned
//                      const float*   xs,      // a5: token 0's block scales
//                      int            nb,      // a6: number of 32-weight blocks
//                      int            xqs,     // a7: bytes from one token's xq to the next;
//                                              //     its xs are xqs / 32 floats further on
//                      float*         out,     // stack: 4 * ntile sums, token order
//                      int            ntile);  // stack: tiles of four tokens, >= 1
//
// Each token's sum is accumulated exactly as dot_q4q8_pie does it, so the results are identical.
//
// Frame: a1+0 tiles left, +4 nib, +8 scales, +12 nb, +16 xs stride (bytes), +20 xq of the tile,
// +24 xs of the tile, +28 xqs.

    .text
    .align  4
    .global dot4_q4q8_pie
    .type   dot4_q4q8_pie, @function
dot4_q4q8_pie:
    entry   a1, 48
    l32i    a15, a1, 48         // out (7th argument, on the caller's stack)
    l32i    a8, a1, 52          // ntile (8th)
    s32i    a8, a1, 0
    s32i    a2, a1, 4
    s32i    a3, a1, 8
    s32i    a6, a1, 12
    srai    a8, a7, 3           // xs stride in bytes: xqs / 32 floats
    s32i    a8, a1, 16
    s32i    a4, a1, 20
    s32i    a5, a1, 24
    s32i    a7, a1, 28
    movi    a8, .Lconst
    ee.vldbc.8 q7, a8           // q7 = 0x0F splat
    addi    a8, a8, 1
    ee.vldbc.8 q6, a8           // q6 = 8 splat
    ssai    4
.Ltile:
    l32i    a4, a1, 20          // this tile's token pointers
    l32i    a7, a1, 28
    add     a9, a4, a7
    add     a10, a9, a7
    add     a11, a10, a7
    l32i    a5, a1, 24
    l32i    a8, a1, 16
    add     a12, a5, a8
    add     a13, a12, a8
    add     a14, a13, a8
    l32i    a2, a1, 4           // back to the start of the weight row
    l32i    a3, a1, 8
    l32i    a6, a1, 12
    movi    a8, 0
    wfr     f0, a8
    wfr     f1, a8
    wfr     f2, a8
    wfr     f3, a8
    // Per block: unpack the weights once, then token k's MACs overlap token k-1's scalar tail.
    // Token k's sum is still acc_k += (float)isum_k * (wscale * xs_k[b]), as in dot_q4q8_pie.
    loopnez a6, .Lloop4_end
    ee.vld.128.ip q0, a2, 16    // 16 packed nibble bytes
    l16ui       a8, a3, 0       // bf16 weight scale bits
    lsip        f5, a5, 4       // xs[b] of tokens 0..3
    lsip        f6, a12, 4
    lsip        f7, a13, 4
    lsip        f8, a14, 4
    ee.vsr.32   q1, q0
    ee.andq     q0, q0, q7      // low nibbles  -> q[0..15]
    slli        a8, a8, 16
    ee.andq     q1, q1, q7      // high nibbles -> q[16..31]
    wfr         f4, a8          // wscale
    ee.vsubs.s8 q0, q0, q6
    ee.vsubs.s8 q1, q1, q6
    mul.s       f5, f4, f5      // wscale * xscale, per token
    mul.s       f6, f4, f6
    mul.s       f7, f4, f7
    mul.s       f8, f4, f8
    addi        a3, a3, 2
    // token 0
    ee.vld.128.ip q2, a4, 16
    ee.vld.128.ip q3, a4, 16
    ee.zero.accx
    ee.vmulas.s8.accx q0, q2
    ee.vld.128.ip q4, a9, 16    // token 1's activations
    ee.vmulas.s8.accx q1, q3
    ee.vld.128.ip q5, a9, 16
    rur.accx_0  a7
    // token 1
    ee.zero.accx
    ee.vmulas.s8.accx q0, q4
    ee.vld.128.ip q2, a10, 16   // token 2's
    ee.vmulas.s8.accx q1, q5
    ee.vld.128.ip q3, a10, 16
    float.s     f9, a7, 0
    rur.accx_0  a8
    // token 2
    ee.zero.accx
    ee.vmulas.s8.accx q0, q2
    ee.vld.128.ip q4, a11, 16   // token 3's
    ee.vmulas.s8.accx q1, q3
    ee.vld.128.ip q5, a11, 16
    madd.s      f0, f9, f5
    float.s     f10, a8, 0
    rur.accx_0  a7
    // token 3
    ee.zero.accx
    ee.vmulas.s8.accx q0, q4
    ee.vmulas.s8.accx q1, q5
    madd.s      f1, f10, f6
    float.s     f11, a7, 0
    rur.accx_0  a8
    madd.s      f2, f11, f7
    float.s     f12, a8, 0
    madd.s      f3, f12, f8
.Lloop4_end:
    ssi     f0, a15, 0
    ssi     f1, a15, 4
    ssi     f2, a15, 8
    ssi     f3, a15, 12
    addi    a15, a15, 16
    l32i    a7, a1, 28          // next tile: four tokens on
    slli    a7, a7, 2
    l32i    a8, a1, 20
    add     a8, a8, a7
    s32i    a8, a1, 20
    l32i    a7, a1, 16
    slli    a7, a7, 2
    l32i    a8, a1, 24
    add     a8, a8, a7
    s32i    a8, a1, 24
    l32i    a8, a1, 0
    addi    a8, a8, -1
    s32i    a8, a1, 0
    bnez    a8, .Ltile
    retw.n
    .size   dot4_q4q8_pie, . - dot4_q4q8_pie
#endif  // CONFIG_IDF_TARGET_ESP32S3
