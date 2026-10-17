/* Functions the game has as hand-written assembly (copy and clear loops), in C for the PC port. */
#include "common.h"
#include "vu0.h"

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

/* The identity scaled by (x, y, z). */
void func_0018F830(f32 x, f32 y, f32 z, f32 *m) {
    memcpy(m, D_00277260, 64);
    m[0] = x;
    m[5] = y;
    m[10] = z;
}

/* Rotates rows 0-2 about x by a: y' = c*y - s*z, z' = s*y + c*z. */
void func_0018FAD0(f32 a, f32 *m) {
    f32 s = sinf(a), c = cosf(a);
    for (int r = 0; r < 3; r++) {
        f32 *v = m + r * 4, y = v[1], z = v[2];
        v[1] = c * y - s * z;
        v[2] = s * y + c * z;
    }
}

/* The inverse of a rotation-and-scale matrix with a translation: row i of the 3x3 part is
   column i of m divided by the squared length of that row of m; the translation is -t through it. */
void func_00190E00(f32 *out, const f32 *m) {
    f32 r[16], q[3];
    for (int j = 0; j < 3; j++) {
        const f32 *row = m + j * 4;
        q[j] = 1.0f / (row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
    }
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) r[i * 4 + j] = m[j * 4 + i] * q[j];
        r[i * 4 + 3] = 0.0f;
    }
    for (int j = 0; j < 3; j++)
        r[12 + j] = -(m[j * 4 + 0] * m[12] + m[j * 4 + 1] * m[13] + m[j * 4 + 2] * m[14]) * q[j];
    r[15] = 1.0f;
    memcpy(out, r, 64);
}

/* Rows: 0 = a, 2 = a x b, 1 = row 2 x a (unit length); the w column (0, 0, 0, 1), row 3's xyz kept. */
void func_00190F50(f32 *m, const f32 *a, const f32 *b) {
    f32 x[3] = {a[0], a[1], a[2]}, u[3] = {b[0], b[1], b[2]}, z[3], y[3];
    norm3(x);
    norm3(u);
    cross3(z, x, u);
    norm3(z);
    cross3(y, z, x);
    memcpy(m, x, 12);
    memcpy(m + 4, y, 12);
    memcpy(m + 8, z, 12);
    m[3] = m[7] = m[11] = 0.0f;
    m[15] = 1.0f;
}

/* Rows: 0 = a, 1 = b x a, 2 = a x row 1 (unit length); w column as above. */
void func_00191030(f32 *m, const f32 *a, const f32 *b) {
    f32 x[3] = {a[0], a[1], a[2]}, u[3] = {b[0], b[1], b[2]}, y[3], z[3];
    norm3(x);
    norm3(u);
    cross3(y, u, x);
    norm3(y);
    cross3(z, x, y);
    memcpy(m, x, 12);
    memcpy(m + 4, y, 12);
    memcpy(m + 8, z, 12);
    m[3] = m[7] = m[11] = 0.0f;
    m[15] = 1.0f;
}

/* --- more of the VU0 library --- */

static void rot_rows(f32 *m, int axis, f32 a) {
    f32 s = sinf(a), c = cosf(a);
    for (int r = 0; r < 3; r++) {
        f32 *v = m + r * 4, x = v[0], y = v[1], z = v[2];
        if (axis == 0) {
            v[1] = c * y - s * z;
            v[2] = s * y + c * z;
        } else if (axis == 1) {
            v[0] = c * x + s * z;
            v[2] = -s * x + c * z;
        } else {
            v[0] = c * x - s * y;
            v[1] = s * x + c * y;
        }
    }
}

/* Linear interpolation through (x0, y0) and (x1, y1) at t. */
f32 func_0018EC50(f32 t, f32 y0, f32 x0, f32 y1, f32 x1) {
    f32 q = 1.0f / (x1 - x0);
    return (y1 - y0) * q * t + (x1 * y0 - x0 * y1) * q;
}

/* Hermite interpolation between (x0, y0) with slope m0 and (x1, y1) with slope m1 at t. */
f32 func_0018ECD0(f32 t, f32 y0, f32 x0, f32 m0, f32 y1, f32 x1, f32 m1) {
    f32 d = t - x0, q = 1.0f / (x1 - x0);
    f32 q2 = q * q, d2 = d * d;
    f32 d2q = d2 * q, d3q2 = d2 * d * q2;
    f32 h2 = 2.0f * d3q2 * q, h3 = 3.0f * d2 * q2;
    return (h2 - h3 + 1.0f) * y0 + (h3 - h2) * y1 + (d3q2 - d2q - d2q + d) * m0 + (d3q2 - d2q) * m1;
}

/* Rows 0-2 become the identity's scaled by (x, y, z), keeping their w; row 3 kept. */
void func_0018F880(f32 x, f32 y, f32 z, f32 *m) {
    f32 s[3] = {x, y, z};
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) m[r * 4 + k] = r == k ? s[r] : 0.0f;
}

/* A translation matrix. */
void func_0018F8E0(f32 x, f32 y, f32 z, f32 *m) {
    memcpy(m, D_00277260, 64);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

void func_0018F930(f32 s, f32 *m) {
    for (int i = 0; i < 16; i++) m[i] *= s;
}

void func_0018F980(f32 s, f32 *m) {
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) m[r * 4 + k] *= s;
}

/* Rotates rows 0-2 about y, about z. */
void func_0018FB70(f32 a, f32 *m) { rot_rows(m, 1, a); }
void func_0018FC10(f32 a, f32 *m) { rot_rows(m, 2, a); }

/* Rotation matrices (rows 0-2 from the identity) in other orders, and the same applied to a
   matrix's rows 0-2. */
void func_0018FE70(f32 rx, f32 ry, f32 rz, f32 *m) {
    func_0018F7F0(m);
    rot_rows(m, 0, rx);
    rot_rows(m, 2, rz);
    rot_rows(m, 1, ry);
}
void func_00190040(f32 rx, f32 ry, f32 rz, f32 *m) {
    func_0018F7F0(m);
    rot_rows(m, 1, ry);
    rot_rows(m, 0, rx);
    rot_rows(m, 2, rz);
}
void func_00190210(f32 rx, f32 ry, f32 rz, f32 *m) {
    func_0018F7F0(m);
    rot_rows(m, 2, rz);
    rot_rows(m, 1, ry);
    rot_rows(m, 0, rx);
}
void func_00190570(f32 rx, f32 ry, f32 rz, f32 *m) {
    rot_rows(m, 0, rx);
    rot_rows(m, 2, rz);
    rot_rows(m, 1, ry);
}
void func_00190700(f32 rx, f32 ry, f32 rz, f32 *m) {
    rot_rows(m, 2, rz);
    rot_rows(m, 1, ry);
    rot_rows(m, 0, rx);
}
void func_001908A0(f32 rx, f32 ry, f32 rz, f32 *m) {
    rot_rows(m, 2, rz);
    rot_rows(m, 0, rx);
    rot_rows(m, 1, ry);
}

/* a = a * b (each row of a through b). */
void func_00190CD0(f32 *a, const f32 *b) { func_00190C50(a, a, b); }

/* out rows 0-2 (xyz) = a's rows 0-2 through b's 3x3 part. */
void func_00190D50(f32 *out, const f32 *a, const f32 *b) {
    f32 r[9];
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++)
            r[i * 3 + k] = b[k] * a[i * 4] + b[4 + k] * a[i * 4 + 1] + b[8 + k] * a[i * 4 + 2];
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) out[i * 4 + k] = r[i * 3 + k];
}
void func_00190DB0(f32 *a, const f32 *b) { func_00190D50(a, a, b); }

/* Rows: 1 = b, 0 = b x a, 2 = row 0 x b (unit length); w column (0, 0, 0, 1), row 3's xyz kept. */
void func_00191110(f32 *m, const f32 *a, const f32 *b) {
    f32 x[3] = {a[0], a[1], a[2]}, y[3] = {b[0], b[1], b[2]}, z[3];
    norm3(x);
    norm3(y);
    cross3(x, y, x);
    norm3(x);
    cross3(z, x, y);
    memcpy(m, x, 12);
    memcpy(m + 4, y, 12);
    memcpy(m + 8, z, 12);
    m[3] = m[7] = m[11] = 0.0f;
    m[15] = 1.0f;
}

/* Blends two matrices (rows 0, 1 and 3 by s and u) and makes rows 0-2 orthonormal again. */
void func_001912D0(f32 s, f32 u, f32 *out, const f32 *a, const f32 *b) {
    f32 x[3], y[3], z[3], t[3];
    for (int k = 0; k < 3; k++) {
        x[k] = a[k] * s + b[k] * u;
        y[k] = a[4 + k] * s + b[4 + k] * u;
        t[k] = a[12 + k] * s + b[12 + k] * u;
    }
    norm3(x);
    norm3(y);
    cross3(z, x, y);
    norm3(z);
    cross3(y, z, x);
    memcpy(out, x, 12);
    memcpy(out + 4, y, 12);
    memcpy(out + 8, z, 12);
    memcpy(out + 12, t, 12);
    out[3] = out[7] = out[11] = 0.0f;
    out[15] = 1.0f;
}

/* v.xyz through m's 3x3 part, in place. */
void func_00191410(f32 *v, const f32 *m) { func_001913A0(v, v, m); }

/* out = (v, 1) through m, xyz divided by w (out[3] = w). */
void func_00191480(f32 *out, const f32 *v, const f32 *m) {
    f32 r[4];
    for (int k = 0; k < 4; k++) r[k] = m[k] * v[0] + m[4 + k] * v[1] + m[8 + k] * v[2] + m[12 + k];
    f32 q = 1.0f / r[3];
    out[0] = r[0] * q;
    out[1] = r[1] * q;
    out[2] = r[2] * q;
    out[3] = r[3];
}

f32 func_00191690(const f32 *v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

f32 func_001916E0(const f32 *a, const f32 *b) {
    f32 x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}

f32 func_001917C0(const f32 *a, const f32 *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void func_00191820(f32 *out, const f32 *a, const f32 *b) { cross3(out, a, b); }

/* SUBMAIN.BIN's VU0 helper: out = v * s. */
/* SUBMAIN.BIN (VU0): out = a - b */
void func_00340410(f32 *out, const f32 *a, const f32 *b) { f32 x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; out[0] = x; out[1] = y; out[2] = z; }
void func_003405D0(f32 s, f32 *out, const f32 *v) {
    f32 x = v[0] * s, y = v[1] * s, z = v[2] * s;
    out[0] = x;
    out[1] = y;
    out[2] = z;
}
