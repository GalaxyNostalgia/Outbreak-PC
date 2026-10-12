/* Functions the game has as hand-written assembly (copy and clear loops), in C for the PC port. */
#include "common.h"

#include <string.h>

void func_00189598(const void *src, void *dst, s32 words) { memcpy(dst, src, (size_t)words * 4); }
void func_001895B8(const void *src, void *dst, s32 n) { memcpy(dst, src, (size_t)n * 16); }
void func_001895F0(const void *src, void *dst, s32 n) { memcpy(dst, src, (size_t)n * 16); }
void func_00189618(const void *src, void *dst, s32 n) { memcpy(dst, src, (size_t)n * 16); }
void func_00189638(const void *src, void *dst, s32 n) { memcpy(dst, src, (size_t)n * 32); }
void func_00189690(const void *src, void *dst, s32 n) { memcpy(dst, src, (size_t)n * 32); }
void func_001896C8(const void *src, void *dst, s32 n) { memcpy(dst, src, (size_t)n * 32); }
void func_001896F0(const void *src, void *dst, s32 n) { memcpy(dst, src, (size_t)n * 64); }
void func_00189728(void *dst, s32 words) { memset(dst, 0, (size_t)words * 4); }
void func_00189740(void *dst, s32 qwords) { memset(dst, 0, (size_t)qwords * 16); }

/* VU0 macro-mode routines (hand-written in the original). */
#include <math.h>

/* out = b * a for 4x4 float matrices stored as four rows: out[i] = sum_j a[i][j] * b[j]. */
void func_00190C50(f32 *out, const f32 *a, const f32 *b) {
    f32 r[16];
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 4; k++) {
            r[i * 4 + k] = b[0 * 4 + k] * a[i * 4 + 0] + b[1 * 4 + k] * a[i * 4 + 1] +
                           b[2 * 4 + k] * a[i * 4 + 2] + b[3 * 4 + k] * a[i * 4 + 3];
        }
    }
    memcpy(out, r, sizeof r);
}

/* Normalizes a 3-vector in place. */
void func_00191750(f32 *v) {
    f32 d = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    f32 q = 1.0f / sqrtf(d);
    v[0] *= q;
    v[1] *= q;
    v[2] *= q;
}
