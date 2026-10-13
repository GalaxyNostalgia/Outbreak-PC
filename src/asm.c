/* Functions the game has as hand-written assembly (copy and clear loops), in C for the PC port. */
#include "common.h"

#include <math.h>
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

/* VU0 trigonometry (vu0math): sin, cos, and both at once into a vector */
f32 func_0018EDB0(f32 x) { return sinf(x); }
f32 func_0018EED0(f32 x) { return cosf(x); }
void func_0018EFC0(f32 *out, f32 x) {
    out[0] = sinf(x);
    out[1] = cosf(x);
    out[2] = 0.0f;
    out[3] = 0.0f;
}
/* VU0 matrix copies: the identity (main's data at 0x277260), and any 4x4 matrix */
extern f32 D_00277260[16];
void func_0018F7C0(f32 *m) { memcpy(m, D_00277260, 64); }
void func_00190ED0(f32 *dst, const f32 *src) { memmove(dst, src, 64); }

/* Rotates the first three rows of a 4x4 matrix about x, then y, then z (radians). */
void func_001903E0(f32 rx, f32 ry, f32 rz, f32 *m) {
    for (int step = 0; step < 3; step++) {
        f32 a = step == 0 ? rx : step == 1 ? ry : rz;
        f32 s = sinf(a), c = cosf(a);
        for (int r = 0; r < 3; r++) {
            f32 *v = m + r * 4, x = v[0], y = v[1], z = v[2];
            if (step == 0) {
                v[1] = c * y - s * z;
                v[2] = s * y + c * z;
            } else if (step == 1) {
                v[0] = c * x + s * z;
                v[2] = -s * x + c * z;
            } else {
                v[0] = c * x - s * y;
                v[1] = s * x + c * y;
            }
        }
    }
}

/* A rotation looking along dir: row 2 = dir, row 0 = up x dir, row 1 = dir x row 0 (all unit
   length); the w column becomes (0, 0, 0, 1) and row 3's xyz is left alone. */
static void norm3(f32 *v) {
    f32 q = 1.0f / sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] *= q; v[1] *= q; v[2] *= q;
}
static void cross3(f32 *o, const f32 *a, const f32 *b) {
    f32 x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
void func_001911F0(f32 *m, const f32 *dir, const f32 *up) {
    f32 d[3] = {dir[0], dir[1], dir[2]}, u[3] = {up[0], up[1], up[2]}, x[3], y[3];
    norm3(u);
    norm3(d);
    cross3(x, u, d);
    norm3(x);
    cross3(y, d, x);
    memcpy(m, x, 12);
    memcpy(m + 4, y, 12);
    memcpy(m + 8, d, 12);
    m[3] = m[7] = m[11] = 0.0f;
    m[15] = 1.0f;
}

/* Transposes the 3x3 part of a 4x4 matrix in place (w column kept). */
void func_00190BA0(f32 *m) {
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++) {
            f32 t = m[i * 4 + j];
            m[i * 4 + j] = m[j * 4 + i];
            m[j * 4 + i] = t;
        }
}

/* out.xyz = v.x * row 0 + v.y * row 1 + v.z * row 2 */
void func_001913A0(f32 *out, const f32 *v, const f32 *m) {
    f32 x = v[0], y = v[1], z = v[2];
    for (int k = 0; k < 3; k++) out[k] = m[k] * x + m[4 + k] * y + m[8 + k] * z;
}

/* Rows 0-2 of m become the identity's (xyz only; w and row 3 kept). */
void func_0018F7F0(f32 *m) {
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) m[r * 4 + k] = D_00277260[r * 4 + k];
}

/* A rotation matrix from x, y, z angles (radians), rotated in that order; row 3 kept. */
void func_0018FCA0(f32 rx, f32 ry, f32 rz, f32 *m) {
    func_0018F7F0(m);
    func_001903E0(rx, ry, rz, m);
}

/* VU0 square root */
f32 func_00191BF0(f32 x) { return sqrtf(x); }
