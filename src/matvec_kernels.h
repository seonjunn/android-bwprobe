/*
 * matvec_kernels.h — Shared float32 matrix-vector multiply kernels
 *
 * Kernel: y = A x,  A: MxN float32 row-major, x: N float32, y: M float32.
 *
 * Shared by matvec.c, prefault_exp.c, icc_bw.c, bw_bench.c, burst_sweep.c.
 * All functions are marked __attribute__((noinline, unused)) to prevent:
 *   - compiler merging across call sites (noinline)
 *   - warnings in translation units that include only one of the two (unused)
 */

#pragma once
#include <arm_neon.h>
#include "timer.h"    /* now_s() — needed by store_neon() */

/*
 * Scalar kernel — autovectorized by clang into 4-wide NEON FMLA, 1 accumulator.
 * At large N, this is FMA-latency-bound (~11 cycles): BW ceiling ~6.5 GB/s.
 * At small N, OOO spans row boundaries → 2-3 rows in flight → higher BW.
 */
__attribute__((noinline, unused))
static void matvec_scalar(const float * restrict A,
                          const float * restrict x,
                          float       * restrict y,
                          int M, int N)
{
    for (int i = 0; i < M; i++) {
        const float * restrict row = A + (size_t)i * N;
        float s = 0.0f;
        for (int j = 0; j < N; j++)
            s += row[j] * x[j];
        y[i] = s;
    }
}

/*
 * NEON kernel — 8 float32x4 accumulators, 32-wide inner unroll.
 * Breaks the single-accumulator FMA dep chain → DRAM-bandwidth-bound (~54 GB/s).
 * Each iteration processes 128 bytes (2 cache lines) from A and x simultaneously.
 */
__attribute__((noinline, unused))
static void matvec_neon(const float * restrict A,
                        const float * restrict x,
                        float       * restrict y,
                        int M, int N)
{
    for (int i = 0; i < M; i++) {
        const float *row = A + (size_t)i * N;
        float32x4_t acc0 = vdupq_n_f32(0.0f), acc1 = vdupq_n_f32(0.0f);
        float32x4_t acc2 = vdupq_n_f32(0.0f), acc3 = vdupq_n_f32(0.0f);
        float32x4_t acc4 = vdupq_n_f32(0.0f), acc5 = vdupq_n_f32(0.0f);
        float32x4_t acc6 = vdupq_n_f32(0.0f), acc7 = vdupq_n_f32(0.0f);
        int j = 0;
        for (; j <= N - 32; j += 32) {
            acc0 = vfmaq_f32(acc0, vld1q_f32(row + j     ), vld1q_f32(x + j     ));
            acc1 = vfmaq_f32(acc1, vld1q_f32(row + j +  4), vld1q_f32(x + j +  4));
            acc2 = vfmaq_f32(acc2, vld1q_f32(row + j +  8), vld1q_f32(x + j +  8));
            acc3 = vfmaq_f32(acc3, vld1q_f32(row + j + 12), vld1q_f32(x + j + 12));
            acc4 = vfmaq_f32(acc4, vld1q_f32(row + j + 16), vld1q_f32(x + j + 16));
            acc5 = vfmaq_f32(acc5, vld1q_f32(row + j + 20), vld1q_f32(x + j + 20));
            acc6 = vfmaq_f32(acc6, vld1q_f32(row + j + 24), vld1q_f32(x + j + 24));
            acc7 = vfmaq_f32(acc7, vld1q_f32(row + j + 28), vld1q_f32(x + j + 28));
        }
        acc0 = vaddq_f32(vaddq_f32(acc0, acc1), vaddq_f32(acc2, acc3));
        acc4 = vaddq_f32(vaddq_f32(acc4, acc5), vaddq_f32(acc6, acc7));
        float s = vaddvq_f32(vaddq_f32(acc0, acc4));
        for (; j < N; j++) s += row[j] * x[j];
        y[i] = s;
    }
}

/*
 * NEON AXPY: y[i] += alpha * x[i]  for i in [0, N)
 *
 * Memory traffic per element: 4 bytes read x + 4 bytes read y + 4 bytes write y = 12 bytes.
 * Theoretical BW demand: 3 × N × sizeof(float) per call.
 * This is the highest memory-intensity kernel (read+write streaming).
 * 4-way unroll to hide load-use latency and saturate the LSU pipeline.
 */
__attribute__((noinline, unused))
static void axpy_neon(float alpha, const float * restrict x, float * restrict y, int N)
{
    float32x4_t va = vdupq_n_f32(alpha);
    int i = 0;
    for (; i <= N - 16; i += 16) {
        float32x4_t y0 = vld1q_f32(y + i     );
        float32x4_t y1 = vld1q_f32(y + i +  4);
        float32x4_t y2 = vld1q_f32(y + i +  8);
        float32x4_t y3 = vld1q_f32(y + i + 12);
        y0 = vfmaq_f32(y0, va, vld1q_f32(x + i     ));
        y1 = vfmaq_f32(y1, va, vld1q_f32(x + i +  4));
        y2 = vfmaq_f32(y2, va, vld1q_f32(x + i +  8));
        y3 = vfmaq_f32(y3, va, vld1q_f32(x + i + 12));
        vst1q_f32(y + i     , y0);
        vst1q_f32(y + i +  4, y1);
        vst1q_f32(y + i +  8, y2);
        vst1q_f32(y + i + 12, y3);
    }
    for (; i < N; i++) y[i] += alpha * x[i];
}

/*
 * NEON non-temporal store stream — bypasses all cache levels (non-temporal hint).
 * Uses stnp (Store Non-Temporal Pair, A64 §C7.2.320) for 32B writes per instruction.
 * No write-allocate reads: produces write-only traffic without demand loads.
 *
 * 8 stnp per inner iteration = 256B per loop step.  n must be a multiple of 64
 * (256B / sizeof(float)); caller should round down: n &= ~63UL.
 *
 * Returns total passes completed; bw_alg = passes * n * sizeof(float) / elapsed.
 */
__attribute__((noinline, unused))
static long store_neon(float *buf, size_t n, double secs)
{
    float32x4_t v   = vdupq_n_f32(1.5f);   /* stored value — irrelevant to BW */
    float       *end = buf + (n & ~(size_t)63); /* round down to 256B boundary */
    long passes = 0;
    double t0 = now_s();
    while (now_s() - t0 < secs) {
        float *p = buf;
        while (p < end) {
            asm volatile(
                "stnp %q[v], %q[v], [%[p]]\n\t"
                "stnp %q[v], %q[v], [%[p], #32]\n\t"
                "stnp %q[v], %q[v], [%[p], #64]\n\t"
                "stnp %q[v], %q[v], [%[p], #96]\n\t"
                "stnp %q[v], %q[v], [%[p], #128]\n\t"
                "stnp %q[v], %q[v], [%[p], #160]\n\t"
                "stnp %q[v], %q[v], [%[p], #192]\n\t"
                "stnp %q[v], %q[v], [%[p], #224]\n\t"
                : : [p] "r"(p), [v] "w"(v) : "memory"
            );
            p += 64;   /* 64 floats = 256 bytes */
        }
        passes++;
    }
    return passes;
}
