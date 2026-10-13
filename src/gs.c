/* A software model of the PS2's Graphics Synthesizer: 4 MiB of local memory in the GS's own
 * pixel layouts, the drawing registers, the primitives (points, lines, triangles, sprites) with
 * texturing, CLUTs, alpha blending and the alpha/depth tests, and host->local image transfers.
 *
 * It renders what the game's 2D code draws (menus, fonts, pictures, movies) the way the PS2 would.
 * The 3D scenes, drawn by VU1 microprograms, get a native renderer later (docs/DESIGN.md).
 *
 * The memory layouts (pages, blocks, columns) follow the GS User's Manual; the column orders are
 * computed rather than tabled (see addr8/addr4).
 */
#include "gs.h"
#include "log.h"

#include <math.h>
#include <string.h>

#define MEM_BYTES (4u * 1024 * 1024)
static union {
    uint8_t b[MEM_BYTES];
    uint16_t h[MEM_BYTES / 2];
    uint32_t w[MEM_BYTES / 4];
} g_mem;

/* ---- memory layouts ---- */

enum {
    PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0A, PSMT8 = 0x13, PSMT4 = 0x14,
    PSMT8H = 0x1B, PSMT4HL = 0x24, PSMT4HH = 0x2C, PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32,
    PSMZ16S = 0x3A,
};

static const uint8_t block32[4][8] = {
    {0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const uint8_t block16[8][4] = {
    {0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15},
    {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
static const uint8_t block16s[8][4] = {
    {0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27},
    {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};

/* Word order inside a 32-bit column (8x2 pixels). */
static inline uint32_t col32(uint32_t x, uint32_t y) { return (x >> 1) * 4 + (y & 1) * 2 + (x & 1); }

/* Word address of a PSMCT32 pixel: pages of 64x32, blocks of 8x8, 4 columns of 8x2 per block. */
static inline uint32_t addr32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    uint32_t block = bp + (y >> 5) * bw * 32 + (x >> 6) * 32 + block32[(y >> 3) & 3][(x >> 3) & 7];
    return ((block & 0x3FFF) * 64 + ((y >> 1) & 3) * 16 + col32(x & 7, y)) & (MEM_BYTES / 4 - 1);
}

/* Halfword address of a PSMCT16(S) pixel: pages of 64x64, blocks of 16x8, columns of 16x2. */
static inline uint32_t addr16(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, const uint8_t (*bt)[4]) {
    uint32_t block = bp + (y >> 6) * bw * 32 + (x >> 6) * 32 + bt[(y >> 3) & 7][(x >> 4) & 3];
    return ((block & 0x3FFF) * 128 + ((y >> 1) & 3) * 32 + 2 * col32(x & 7, y) + ((x >> 3) & 1)) &
           (MEM_BYTES / 2 - 1);
}

/* PSMT8: pages of 128x64, blocks of 16x16, columns of 16x4 whose odd/even row pairs are swizzled. */
static inline uint32_t addr8(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    uint32_t pw = bw >> 1 ? bw >> 1 : 1;
    uint32_t block = bp + (y >> 6) * pw * 32 + (x >> 7) * 32 + block32[(y >> 4) & 3][(x >> 4) & 7];
    uint32_t col = (y >> 2) & 3, ly = y & 3;
    uint32_t xx = (x & 7) ^ ((((ly >> 1) ^ col) & 1) ? 4 : 0);
    return ((block & 0x3FFF) * 256 + col * 64 + 4 * col32(xx, ly) + (ly >> 1) + 2 * ((x >> 3) & 1)) &
           (MEM_BYTES - 1);
}

/* PSMT4 (nibble address): pages of 128x128, blocks of 32x16, columns of 32x4. */
static inline uint32_t addr4(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    uint32_t pw = bw >> 1 ? bw >> 1 : 1;
    uint32_t block = bp + (y >> 7) * pw * 32 + (x >> 7) * 32 + block16[(y >> 4) & 7][(x >> 5) & 3];
    uint32_t col = (y >> 2) & 3, ly = y & 3;
    uint32_t xx = (x & 7) ^ ((((ly >> 1) ^ col) & 1) ? 4 : 0);
    return ((block & 0x3FFF) * 512 + col * 128 + 8 * col32(xx, ly) + (ly >> 1) + 2 * ((x >> 3) & 3)) &
           (MEM_BYTES * 2 - 1);
}

static uint32_t read_raw(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    switch (psm) {
    case PSMCT32: case PSMZ32: return g_mem.w[addr32(bp, bw, x, y)];
    case PSMCT24: case PSMZ24: return g_mem.w[addr32(bp, bw, x, y)] & 0xFFFFFF;
    case PSMCT16: case PSMZ16: return g_mem.h[addr16(bp, bw, x, y, block16)];
    case PSMCT16S: case PSMZ16S: return g_mem.h[addr16(bp, bw, x, y, block16s)];
    case PSMT8: return g_mem.b[addr8(bp, bw, x, y)];
    case PSMT4: {
        uint32_t a = addr4(bp, bw, x, y);
        return (g_mem.b[a >> 1] >> ((a & 1) * 4)) & 15;
    }
    case PSMT8H: return g_mem.w[addr32(bp, bw, x, y)] >> 24;
    case PSMT4HL: return (g_mem.w[addr32(bp, bw, x, y)] >> 24) & 15;
    case PSMT4HH: return g_mem.w[addr32(bp, bw, x, y)] >> 28;
    }
    return 0;
}

static void write_raw(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v) {
    uint32_t *w;
    switch (psm) {
    case PSMCT32: case PSMZ32: g_mem.w[addr32(bp, bw, x, y)] = v; break;
    case PSMCT24: case PSMZ24:
        w = &g_mem.w[addr32(bp, bw, x, y)];
        *w = (*w & 0xFF000000u) | (v & 0xFFFFFF);
        break;
    case PSMCT16: case PSMZ16: g_mem.h[addr16(bp, bw, x, y, block16)] = (uint16_t)v; break;
    case PSMCT16S: case PSMZ16S: g_mem.h[addr16(bp, bw, x, y, block16s)] = (uint16_t)v; break;
    case PSMT8: g_mem.b[addr8(bp, bw, x, y)] = (uint8_t)v; break;
    case PSMT4: {
        uint32_t a = addr4(bp, bw, x, y);
        uint8_t *b = &g_mem.b[a >> 1];
        *b = (a & 1) ? (uint8_t)((*b & 0x0F) | (v << 4)) : (uint8_t)((*b & 0xF0) | (v & 15));
        break;
    }
    case PSMT8H:
        w = &g_mem.w[addr32(bp, bw, x, y)];
        *w = (*w & 0x00FFFFFFu) | (v << 24);
        break;
    case PSMT4HL:
        w = &g_mem.w[addr32(bp, bw, x, y)];
        *w = (*w & 0xF0FFFFFFu) | ((v & 15) << 24);
        break;
    case PSMT4HH:
        w = &g_mem.w[addr32(bp, bw, x, y)];
        *w = (*w & 0x0FFFFFFFu) | ((v & 15) << 28);
        break;
    }
}

static int bits_per_pixel(uint32_t psm) {
    switch (psm) {
    case PSMCT32: case PSMZ32: return 32;
    case PSMCT24: case PSMZ24: return 24;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: case PSMT8H: return 8;
    case PSMT4: case PSMT4HL: case PSMT4HH: return 4;
    }
    return 0;
}

/* ---- registers ---- */

static uint64_t bits(uint64_t v, int lo, int n) { return (v >> lo) & ((1ull << n) - 1); }

typedef struct {
    uint64_t frame, zbuf, xyoffset, scissor, test, alpha, tex0, tex1, clamp, fba;
} Context;

typedef struct {
    int32_t x, y;               /* 12.4, window coordinates */
    uint32_t z;
    uint8_t r, g, b, a;
    float s, t, q;
    int32_t u, v;               /* 12.4 */
    uint8_t f;
} Vertex;

static struct {
    Context ctx[2];
    uint64_t prim, prmodecont, prmode, texclut, texa, colclamp, pabe, dthe, fogcol;
    uint8_t r, g, b, a;
    float s, t, q;
    int32_t u, v;
    uint8_t fog;
    Vertex vtx[3];
    int nvtx;
    uint64_t bitbltbuf, trxpos, trxreg;
    /* host->local transfer in progress */
    int trx_active;
    uint32_t trx_x, trx_y;
    uint8_t trx_carry[4];
    int trx_ncarry;
    int warned_z;
} g;

void gs_reset(void) {
    memset(&g, 0, sizeof g);
    memset(&g_mem, 0, sizeof g_mem);
    g.prmodecont = 1;
    g.colclamp = 1;
    g.q = 1.0f;
}

/* ---- drawing ---- */

typedef struct {
    Context *c;
    uint32_t attr;              /* PRIM's attribute bits (IIP TME FGE ABE AA1 FST CTXT FIX) */
    /* frame */
    uint32_t fbp, fbw, fpsm, fbmsk;
    /* depth */
    uint32_t zbp, zpsm, zmsk;
    int zte, ztst;
    /* texture */
    int tme, fst, tfx, tcc;
    uint32_t tbp, tbw, tpsm, tw, th, cbp, cpsm, csm, csa;
    uint32_t wms, wmt, minu, maxu, minv, maxv;
    /* tests and blending */
    int ate, atst, aref, afail, date, datm, abe;
    uint32_t alpha;
} DrawState;

static void setup(DrawState *d) {
    uint64_t p = bits(g.prmodecont, 0, 1) ? g.prim : g.prmode;
    d->attr = (uint32_t)bits(p, 3, 8);
    d->c = &g.ctx[bits(p, 9, 1)];
    Context *c = d->c;
    d->fbp = (uint32_t)bits(c->frame, 0, 9) * 32;
    d->fbw = (uint32_t)bits(c->frame, 16, 6);
    d->fpsm = (uint32_t)bits(c->frame, 24, 6);
    d->fbmsk = (uint32_t)bits(c->frame, 32, 32);
    d->zbp = (uint32_t)bits(c->zbuf, 0, 9) * 32;
    d->zpsm = 0x30 | (uint32_t)bits(c->zbuf, 24, 4);
    d->zmsk = (uint32_t)bits(c->zbuf, 32, 1);
    d->zte = (int)bits(c->test, 16, 1);
    d->ztst = (int)bits(c->test, 17, 2);
    d->tme = (int)bits(p, 4, 1);
    d->fst = (int)bits(p, 8, 1);
    d->abe = (int)bits(p, 6, 1);
    uint64_t t = c->tex0;
    d->tbp = (uint32_t)bits(t, 0, 14);
    d->tbw = (uint32_t)bits(t, 14, 6);
    d->tpsm = (uint32_t)bits(t, 20, 6);
    d->tw = 1u << (bits(t, 26, 4) > 10 ? 10 : bits(t, 26, 4));
    d->th = 1u << (bits(t, 30, 4) > 10 ? 10 : bits(t, 30, 4));
    d->tcc = (int)bits(t, 34, 1);
    d->tfx = (int)bits(t, 35, 2);
    d->cbp = (uint32_t)bits(t, 37, 14);
    d->cpsm = (uint32_t)bits(t, 51, 4);
    d->csm = (uint32_t)bits(t, 55, 1);
    d->csa = (uint32_t)bits(t, 56, 5);
    d->wms = (uint32_t)bits(c->clamp, 0, 2);
    d->wmt = (uint32_t)bits(c->clamp, 2, 2);
    d->minu = (uint32_t)bits(c->clamp, 4, 10);
    d->maxu = (uint32_t)bits(c->clamp, 14, 10);
    d->minv = (uint32_t)bits(c->clamp, 24, 10);
    d->maxv = (uint32_t)bits(c->clamp, 34, 10);
    d->ate = (int)bits(c->test, 0, 1);
    d->atst = (int)bits(c->test, 1, 3);
    d->aref = (int)bits(c->test, 4, 8);
    d->afail = (int)bits(c->test, 12, 2);
    d->date = (int)bits(c->test, 14, 1);
    d->datm = (int)bits(c->test, 15, 1);
    d->alpha = (uint32_t)c->alpha;
}

static uint32_t wrap(uint32_t mode, int32_t c, uint32_t size, uint32_t min, uint32_t max) {
    switch (mode) {
    case 0: return (uint32_t)c & (size - 1);
    case 1: return c < 0 ? 0 : (uint32_t)c >= size ? size - 1 : (uint32_t)c;
    case 2: return c < (int32_t)min ? min : c > (int32_t)max ? max : (uint32_t)c;
    default: return ((uint32_t)c & min) | max;
    }
}

/* A 16-bit colour (or texel) as RGBA8888, alpha by TEXA. */
static uint32_t expand16(uint32_t v) {
    uint32_t r = (v & 31) << 3, gg = ((v >> 5) & 31) << 3, b = ((v >> 10) & 31) << 3;
    uint32_t a;
    if (v & 0x8000) a = (uint32_t)bits(g.texa, 32, 8);
    else a = (bits(g.texa, 15, 1) && (v & 0x7FFF) == 0) ? 0 : (uint32_t)bits(g.texa, 0, 8);
    return r | gg << 8 | b << 16 | a << 24;
}

static uint32_t clut_lookup(const DrawState *d, uint32_t index) {
    if (d->csm) {                                   /* CSM2: a row of a CT16 texture at (COU, COV) */
        uint32_t cbw = (uint32_t)bits(g.texclut, 0, 6), cou = (uint32_t)bits(g.texclut, 6, 6) * 16;
        uint32_t cov = (uint32_t)bits(g.texclut, 12, 10);
        return expand16(read_raw(PSMCT16, d->cbp, cbw, cou + index, cov));
    }
    uint32_t e = (bits_per_pixel(d->tpsm) == 4) ? d->csa * 16 + index : index;
    uint32_t cpsm = d->cpsm;
    if (cpsm == PSMCT32 || cpsm == PSMCT24) e &= 0xFF;
    else e &= 0x1FF;
    /* CSM1: 16x16 (8x2 for 4-bit) with entries 8-15 and 16-23 of every 32 swapped */
    uint32_t e2 = (e & ~0x18u) | ((e & 8) << 1) | ((e & 0x10) >> 1);
    uint32_t x = e2 & 15, y = e2 >> 4;
    if (cpsm == PSMCT32 || cpsm == PSMCT24) {
        uint32_t v = read_raw(PSMCT32, d->cbp, 1, x, y);
        return cpsm == PSMCT24 ? (v & 0xFFFFFF) | (uint32_t)bits(g.texa, 0, 8) << 24 : v;
    }
    return expand16(read_raw(cpsm == PSMCT16S ? PSMCT16S : PSMCT16, d->cbp, 1, x, y));
}

static uint32_t texel(const DrawState *d, int32_t u, int32_t v) {
    uint32_t x = wrap(d->wms, u, d->tw, d->minu, d->maxu), y = wrap(d->wmt, v, d->th, d->minv, d->maxv);
    uint32_t raw = read_raw(d->tpsm, d->tbp, d->tbw, x, y);
    switch (d->tpsm) {
    case PSMCT32: case PSMZ32: return raw;
    case PSMCT24: case PSMZ24:
        return raw | ((bits(g.texa, 15, 1) && raw == 0) ? 0 : (uint32_t)bits(g.texa, 0, 8) << 24);
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return expand16(raw);
    default: return clut_lookup(d, raw);
    }
}

static inline uint32_t clamp8(int32_t v, int clampmode) {
    if (clampmode) return v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v;
    return (uint32_t)v & 0xFF;
}

static uint32_t read_frame(const DrawState *d, int32_t x, int32_t y) {
    uint32_t v = read_raw(d->fpsm, d->fbp, d->fbw, (uint32_t)x, (uint32_t)y);
    if (d->fpsm == PSMCT16 || d->fpsm == PSMCT16S) {
        return (v & 31) << 3 | ((v >> 5) & 31) << 11 | ((v >> 10) & 31) << 19 | ((v >> 15) & 1) << 31;
    }
    if (d->fpsm == PSMCT24) v |= 0x80000000u;
    return v;
}

/* One pixel through the pipeline: texture, alpha test, depth test, blending, write. */
static inline void pixel(const DrawState *d, int32_t x, int32_t y, uint32_t z, int r, int gg, int b, int a,
                  int32_t tu, int32_t tv) {
    if (d->tme) {
        uint32_t t = texel(d, tu, tv);
        int tr = t & 0xFF, tg = (t >> 8) & 0xFF, tb = (t >> 16) & 0xFF, ta = t >> 24;
        switch (d->tfx) {
        case 0:                                     /* MODULATE */
            r = (tr * r) >> 7; gg = (tg * gg) >> 7; b = (tb * b) >> 7;
            if (d->tcc) a = (ta * a) >> 7;
            break;
        case 1:                                     /* DECAL */
            r = tr; gg = tg; b = tb;
            if (d->tcc) a = ta;
            break;
        case 2:                                     /* HIGHLIGHT */
            r = ((tr * r) >> 7) + a; gg = ((tg * gg) >> 7) + a; b = ((tb * b) >> 7) + a;
            if (d->tcc) a = a + ta;
            break;
        default:                                    /* HIGHLIGHT2 */
            r = ((tr * r) >> 7) + a; gg = ((tg * gg) >> 7) + a; b = ((tb * b) >> 7) + a;
            if (d->tcc) a = ta;
            break;
        }
        if (r > 255) r = 255;
        if (gg > 255) gg = 255;
        if (b > 255) b = 255;
        if (a > 255) a = 255;
    }
    int write_rgb = 1, write_a = 1, write_z = !d->zmsk;
    if (d->ate) {
        int pass;
        switch (d->atst) {
        case 0: pass = 0; break;
        case 1: pass = 1; break;
        case 2: pass = a < d->aref; break;
        case 3: pass = a <= d->aref; break;
        case 4: pass = a == d->aref; break;
        case 5: pass = a >= d->aref; break;
        case 6: pass = a > d->aref; break;
        default: pass = a != d->aref; break;
        }
        if (!pass) {
            switch (d->afail) {
            case 0: return;                                         /* KEEP */
            case 1: write_z = 0; break;                             /* FB_ONLY */
            case 2: write_rgb = write_a = 0; break;                 /* ZB_ONLY */
            default: write_a = 0; write_z = 0; break;               /* RGB_ONLY */
            }
        }
    }
    uint32_t dst = 0;
    int have_dst = 0;
    if (d->date) {
        dst = read_frame(d, x, y);
        have_dst = 1;
        if ((dst >> 31) != (uint32_t)d->datm) return;
    }
    if (d->zte) {
        if (d->ztst == 0) return;
        if (d->ztst >= 2) {
            uint32_t zb = read_raw(d->zpsm, d->zbp, d->fbw, (uint32_t)x, (uint32_t)y);
            if (d->ztst == 2 ? z < zb : z <= zb) return;
        }
    } else {
        write_z = 0;
    }
    if (d->abe && !(bits(g.pabe, 0, 1) && a < 0x80)) {
        if (!have_dst) dst = read_frame(d, x, y);
        int dr = dst & 0xFF, dg = (dst >> 8) & 0xFF, db = (dst >> 16) & 0xFF, da = dst >> 24;
        int sel_a = d->alpha & 3, sel_b = (d->alpha >> 2) & 3, sel_c = (d->alpha >> 4) & 3, sel_d = (d->alpha >> 6) & 3;
        int fix = (int)bits(d->c->alpha, 32, 8);
        int cc = sel_c == 0 ? a : sel_c == 1 ? da : fix;
        int src[3] = {r, gg, b}, dsc[3] = {dr, dg, db}, out[3];
        for (int i = 0; i < 3; i++) {
            int va = sel_a == 0 ? src[i] : sel_a == 1 ? dsc[i] : 0;
            int vb = sel_b == 0 ? src[i] : sel_b == 1 ? dsc[i] : 0;
            int vd = sel_d == 0 ? src[i] : sel_d == 1 ? dsc[i] : 0;
            out[i] = (((va - vb) * cc) >> 7) + vd;
        }
        int cm = (int)bits(g.colclamp, 0, 1);
        r = (int)clamp8(out[0], cm);
        gg = (int)clamp8(out[1], cm);
        b = (int)clamp8(out[2], cm);
    }
    if (bits(d->c->fba, 0, 1)) a |= 0x80;
    if (write_rgb || write_a) {
        uint32_t c = (uint32_t)r | (uint32_t)gg << 8 | (uint32_t)b << 16 | (uint32_t)a << 24;
        uint32_t keep = d->fbmsk;
        if (!write_a) keep |= 0xFF000000u;
        if (!write_rgb) keep |= 0x00FFFFFFu;
        if (d->fpsm == PSMCT16 || d->fpsm == PSMCT16S) {
            uint32_t old = read_raw(d->fpsm, d->fbp, d->fbw, (uint32_t)x, (uint32_t)y);
            uint32_t v16 = (c >> 3 & 31) | (c >> 11 & 31) << 5 | (c >> 19 & 31) << 10 | (c >> 31) << 15;
            uint32_t k16 = (keep & 0xF8 ? 31 : 0) | (keep & 0xF800 ? 31 << 5 : 0) |
                           (keep & 0xF80000 ? 31 << 10 : 0) | (keep & 0x80000000u ? 0x8000 : 0);
            write_raw(d->fpsm, d->fbp, d->fbw, (uint32_t)x, (uint32_t)y, (old & k16) | (v16 & ~k16));
        } else if (keep) {
            uint32_t old = read_raw(PSMCT32, d->fbp, d->fbw, (uint32_t)x, (uint32_t)y);
            write_raw(d->fpsm, d->fbp, d->fbw, (uint32_t)x, (uint32_t)y, (old & keep) | (c & ~keep));
        } else {
            write_raw(d->fpsm, d->fbp, d->fbw, (uint32_t)x, (uint32_t)y, c);
        }
    }
    if (write_z) write_raw(d->zpsm, d->zbp, d->fbw, (uint32_t)x, (uint32_t)y, z);
}

static int frame_ok(const DrawState *d) {
    switch (d->fpsm) {
    case PSMCT32: case PSMCT24: case PSMCT16: case PSMCT16S: case PSMZ32: case PSMZ24: case PSMZ16: case PSMZ16S:
        return 1;
    }
    log_warn("gs: drawing into PSM %u isn't supported", d->fpsm);
    return 0;
}

/* Scissor and offset: the drawing area in window pixels, inclusive. */
static void area(const DrawState *d, int32_t *ofx, int32_t *ofy, int32_t *sx0, int32_t *sy0, int32_t *sx1, int32_t *sy1) {
    *ofx = (int32_t)bits(d->c->xyoffset, 0, 16);
    *ofy = (int32_t)bits(d->c->xyoffset, 32, 16);
    *sx0 = (int32_t)bits(d->c->scissor, 0, 11);
    *sx1 = (int32_t)bits(d->c->scissor, 16, 11);
    *sy0 = (int32_t)bits(d->c->scissor, 32, 11);
    *sy1 = (int32_t)bits(d->c->scissor, 48, 11);
}

static void tex_coords(const DrawState *d, const Vertex *v, float *u, float *vv, float *q) {
    if (d->fst) {
        *u = (float)v->u / 16.0f;
        *vv = (float)v->v / 16.0f;
        *q = 1.0f;
    } else {
        *u = v->s * (float)d->tw;
        *vv = v->t * (float)d->th;
        *q = v->q;
    }
}

static void draw_sprite(const Vertex *v0, const Vertex *v1) {
    DrawState d;
    setup(&d);
    if (!frame_ok(&d)) return;
    int32_t ofx, ofy, sx0, sy0, sx1, sy1;
    area(&d, &ofx, &ofy, &sx0, &sy0, &sx1, &sy1);
    int32_t X0 = v0->x - ofx, Y0 = v0->y - ofy, X1 = v1->x - ofx, Y1 = v1->y - ofy;
    float u0, t0, q0, u1, t1, q1;
    tex_coords(&d, v0, &u0, &t0, &q0);
    tex_coords(&d, v1, &u1, &t1, &q1);
    if (!d.fst) {           /* sprites take Q from the second vertex */
        u0 /= q1; t0 /= q1; u1 /= q1; t1 /= q1;
    }
    if (X0 > X1) { int32_t t = X0; X0 = X1; X1 = t; float f = u0; u0 = u1; u1 = f; }
    if (Y0 > Y1) { int32_t t = Y0; Y0 = Y1; Y1 = t; float f = t0; t0 = t1; t1 = f; }
    int32_t x0 = (X0 + 15) >> 4, x1 = (X1 + 15) >> 4, y0 = (Y0 + 15) >> 4, y1 = (Y1 + 15) >> 4;
    if (X1 == X0 || Y1 == Y0) return;
    float du = (u1 - u0) / (float)(X1 - X0) * 16.0f, dv = (t1 - t0) / (float)(Y1 - Y0) * 16.0f;
    float ub = u0 + du * ((float)(x0 * 16 - X0) / 16.0f), vb = t0 + dv * ((float)(y0 * 16 - Y0) / 16.0f);
    if (x0 < sx0) { ub += du * (float)(sx0 - x0); x0 = sx0; }
    if (y0 < sy0) { vb += dv * (float)(sy0 - y0); y0 = sy0; }
    if (x1 > sx1 + 1) x1 = sx1 + 1;
    if (y1 > sy1 + 1) y1 = sy1 + 1;
    const Vertex *c = v1;
    if (x0 >= x1 || y0 >= y1) return;
    /* plain fills (screen clears, fades without blending): straight into a 32-bit frame */
    if (!d.tme && !d.abe && !d.ate && !d.date && !(d.zte && d.ztst != 1) && d.fpsm == PSMCT32 && !d.fbmsk &&
        !bits(d.c->fba, 0, 1)) {
        uint32_t col = (uint32_t)c->r | (uint32_t)c->g << 8 | (uint32_t)c->b << 16 | (uint32_t)c->a << 24;
        int wz = d.zte && !d.zmsk;
        for (int32_t y = y0; y < y1; y++)
            for (int32_t x = x0; x < x1; x++) {
                g_mem.w[addr32(d.fbp, d.fbw, (uint32_t)x, (uint32_t)y)] = col;
                if (wz) write_raw(d.zpsm, d.zbp, d.fbw, (uint32_t)x, (uint32_t)y, v1->z);
            }
        return;
    }
    /* texture coordinates in 16.16 fixed point */
    int32_t fdu = (int32_t)(du * 65536.0f), fdv = (int32_t)(dv * 65536.0f);
    int32_t fvv = (int32_t)(vb * 65536.0f), fub = (int32_t)(ub * 65536.0f);
    for (int32_t y = y0; y < y1; y++, fvv += fdv) {
        int32_t fuu = fub;
        for (int32_t x = x0; x < x1; x++, fuu += fdu)
            pixel(&d, x, y, v1->z, c->r, c->g, c->b, c->a, fuu >> 16, fvv >> 16);
    }
}

static void draw_triangle(const Vertex *a, const Vertex *b, const Vertex *c) {
    DrawState d;
    setup(&d);
    if (!frame_ok(&d)) return;
    int32_t ofx, ofy, sx0, sy0, sx1, sy1;
    area(&d, &ofx, &ofy, &sx0, &sy0, &sx1, &sy1);
    const Vertex *v[3] = {a, b, c};
    float px[3], py[3], tu[3], tv[3], tq[3];
    for (int i = 0; i < 3; i++) {
        px[i] = (float)(v[i]->x - ofx) / 16.0f;
        py[i] = (float)(v[i]->y - ofy) / 16.0f;
        tex_coords(&d, v[i], &tu[i], &tv[i], &tq[i]);
    }
    float area2 = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
    if (area2 == 0.0f) return;
    float minx = fminf(px[0], fminf(px[1], px[2])), maxx = fmaxf(px[0], fmaxf(px[1], px[2]));
    float miny = fminf(py[0], fminf(py[1], py[2])), maxy = fmaxf(py[0], fmaxf(py[1], py[2]));
    int32_t x0 = (int32_t)ceilf(minx), x1 = (int32_t)ceilf(maxx), y0 = (int32_t)ceilf(miny), y1 = (int32_t)ceilf(maxy);
    if (x0 < sx0) x0 = sx0;
    if (y0 < sy0) y0 = sy0;
    if (x1 > sx1 + 1) x1 = sx1 + 1;
    if (y1 > sy1 + 1) y1 = sy1 + 1;
    int gouraud = d.attr & 1;
    for (int32_t y = y0; y < y1; y++)
        for (int32_t x = x0; x < x1; x++) {
            float fx = (float)x, fy = (float)y;
            float w0 = ((px[1] - fx) * (py[2] - fy) - (px[2] - fx) * (py[1] - fy)) / area2;
            float w1 = ((px[2] - fx) * (py[0] - fy) - (px[0] - fx) * (py[2] - fy)) / area2;
            float w2 = 1.0f - w0 - w1;
            /* inside, with a top-left rule for shared edges */
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            if ((w0 == 0.0f || w1 == 0.0f || w2 == 0.0f) && (x == x1 - 1 || y == y1 - 1)) continue;
            int r, gg, bb, aa;
            if (gouraud) {
                r = (int)(w0 * v[0]->r + w1 * v[1]->r + w2 * v[2]->r + 0.5f);
                gg = (int)(w0 * v[0]->g + w1 * v[1]->g + w2 * v[2]->g + 0.5f);
                bb = (int)(w0 * v[0]->b + w1 * v[1]->b + w2 * v[2]->b + 0.5f);
                aa = (int)(w0 * v[0]->a + w1 * v[1]->a + w2 * v[2]->a + 0.5f);
            } else {
                r = c->r; gg = c->g; bb = c->b; aa = c->a;
            }
            double z = w0 * (double)v[0]->z + w1 * (double)v[1]->z + w2 * (double)v[2]->z;
            float uu = w0 * tu[0] + w1 * tu[1] + w2 * tu[2], vv = w0 * tv[0] + w1 * tv[1] + w2 * tv[2];
            if (!d.fst) {
                float q = w0 * tq[0] + w1 * tq[1] + w2 * tq[2];
                if (q != 0.0f) { uu /= q; vv /= q; }
            }
            pixel(&d, x, y, (uint32_t)z, r, gg, bb, aa, (int32_t)floorf(uu), (int32_t)floorf(vv));
        }
}

static void draw_line(const Vertex *a, const Vertex *b) {
    DrawState d;
    setup(&d);
    if (!frame_ok(&d)) return;
    int32_t ofx, ofy, sx0, sy0, sx1, sy1;
    area(&d, &ofx, &ofy, &sx0, &sy0, &sx1, &sy1);
    int32_t x0 = (a->x - ofx + 8) >> 4, y0 = (a->y - ofy + 8) >> 4, x1 = (b->x - ofx + 8) >> 4, y1 = (b->y - ofy + 8) >> 4;
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int32_t n = dx > dy ? dx : dy;
    for (int32_t i = 0; i < n; i++) {
        int32_t x = x0 + (x1 - x0) * i / (n ? n : 1), y = y0 + (y1 - y0) * i / (n ? n : 1);
        if (x < sx0 || x > sx1 || y < sy0 || y > sy1) continue;
        pixel(&d, x, y, b->z, b->r, b->g, b->b, b->a, 0, 0);
    }
}

static void vertex_kick(int32_t x, int32_t y, uint32_t z, int draw) {
    Vertex *v = &g.vtx[g.nvtx];
    v->x = x;
    v->y = y;
    v->z = z;
    v->r = g.r; v->g = g.g; v->b = g.b; v->a = g.a;
    v->s = g.s; v->t = g.t; v->q = g.q;
    v->u = g.u; v->v = g.v;
    v->f = g.fog;
    g.nvtx++;
    int type = (int)bits(g.prim, 0, 3);
    switch (type) {
    case 0:                                                     /* point */
        if (draw) draw_line(v, v);
        g.nvtx = 0;
        break;
    case 1:                                                     /* line */
        if (g.nvtx == 2) {
            if (draw) draw_line(&g.vtx[0], &g.vtx[1]);
            g.nvtx = 0;
        }
        break;
    case 2:                                                     /* line strip */
        if (g.nvtx == 2) {
            if (draw) draw_line(&g.vtx[0], &g.vtx[1]);
            g.vtx[0] = g.vtx[1];
            g.nvtx = 1;
        }
        break;
    case 3:                                                     /* triangle */
        if (g.nvtx == 3) {
            if (draw) draw_triangle(&g.vtx[0], &g.vtx[1], &g.vtx[2]);
            g.nvtx = 0;
        }
        break;
    case 4:                                                     /* triangle strip */
        if (g.nvtx == 3) {
            if (draw) draw_triangle(&g.vtx[0], &g.vtx[1], &g.vtx[2]);
            g.vtx[0] = g.vtx[1];
            g.vtx[1] = g.vtx[2];
            g.nvtx = 2;
        }
        break;
    case 5:                                                     /* triangle fan */
        if (g.nvtx == 3) {
            if (draw) draw_triangle(&g.vtx[0], &g.vtx[1], &g.vtx[2]);
            g.vtx[1] = g.vtx[2];
            g.nvtx = 2;
        }
        break;
    case 6:                                                     /* sprite */
        if (g.nvtx == 2) {
            if (draw) draw_sprite(&g.vtx[0], &g.vtx[1]);
            g.nvtx = 0;
        }
        break;
    default:
        g.nvtx = 0;
        break;
    }
}

/* ---- transfers ---- */

static void local_to_local(void) {
    uint32_t sbp = (uint32_t)bits(g.bitbltbuf, 0, 14), sbw = (uint32_t)bits(g.bitbltbuf, 16, 6);
    uint32_t spsm = (uint32_t)bits(g.bitbltbuf, 24, 6);
    uint32_t dbp = (uint32_t)bits(g.bitbltbuf, 32, 14), dbw = (uint32_t)bits(g.bitbltbuf, 48, 6);
    uint32_t dpsm = (uint32_t)bits(g.bitbltbuf, 56, 6);
    uint32_t sx = (uint32_t)bits(g.trxpos, 0, 11), sy = (uint32_t)bits(g.trxpos, 16, 11);
    uint32_t dx = (uint32_t)bits(g.trxpos, 32, 11), dy = (uint32_t)bits(g.trxpos, 48, 11);
    uint32_t w = (uint32_t)bits(g.trxreg, 0, 12), h = (uint32_t)bits(g.trxreg, 32, 12);
    static uint32_t tmp[2048 * 2048 / 4];
    if ((uint64_t)w * h > sizeof tmp / sizeof tmp[0]) return;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) tmp[y * w + x] = read_raw(spsm, sbp, sbw, sx + x, sy + y);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) write_raw(dpsm, dbp, dbw, dx + x, dy + y, tmp[y * w + x]);
}

static void trx_pixel(uint32_t v) {
    uint32_t dbp = (uint32_t)bits(g.bitbltbuf, 32, 14), dbw = (uint32_t)bits(g.bitbltbuf, 48, 6);
    uint32_t dpsm = (uint32_t)bits(g.bitbltbuf, 56, 6);
    uint32_t dx = (uint32_t)bits(g.trxpos, 32, 11), dy = (uint32_t)bits(g.trxpos, 48, 11);
    uint32_t w = (uint32_t)bits(g.trxreg, 0, 12), h = (uint32_t)bits(g.trxreg, 32, 12);
    if (!g.trx_active) return;
    write_raw(dpsm, dbp, dbw, (dx + g.trx_x) & 2047, (dy + g.trx_y) & 2047, v);
    if (++g.trx_x >= w) {
        g.trx_x = 0;
        if (++g.trx_y >= h) g.trx_active = 0;
    }
}

void gs_image_data(const void *src, uint32_t bytes) {
    if (!g.trx_active) return;
    uint32_t dpsm = (uint32_t)bits(g.bitbltbuf, 56, 6);
    int bpp = bits_per_pixel(dpsm);
    const uint8_t *p = src;
    if (!bpp) {
        log_warn("gs: upload in PSM %u not supported", dpsm);
        g.trx_active = 0;
        return;
    }
    if (bpp == 4) {
        for (uint32_t i = 0; i < bytes && g.trx_active; i++) {
            trx_pixel(p[i] & 15);
            trx_pixel(p[i] >> 4);
        }
        return;
    }
    int n = bpp / 8;
    for (uint32_t i = 0; i < bytes && g.trx_active; i++) {
        g.trx_carry[g.trx_ncarry++] = p[i];
        if (g.trx_ncarry == n) {
            uint32_t v = 0;
            for (int k = 0; k < n; k++) v |= (uint32_t)g.trx_carry[k] << (8 * k);
            g.trx_ncarry = 0;
            trx_pixel(v);
        }
    }
}

/* ---- registers ---- */

static void write_reg(uint32_t reg, uint64_t v) {
    switch (reg) {
    case 0x00: g.prim = v; g.nvtx = 0; break;                   /* PRIM */
    case 0x01:                                                  /* RGBAQ */
        g.r = (uint8_t)v; g.g = (uint8_t)(v >> 8); g.b = (uint8_t)(v >> 16); g.a = (uint8_t)(v >> 24);
        { uint32_t q = (uint32_t)(v >> 32); memcpy(&g.q, &q, 4); }
        break;
    case 0x02: {                                                /* ST */
        uint32_t s = (uint32_t)v, t = (uint32_t)(v >> 32);
        memcpy(&g.s, &s, 4);
        memcpy(&g.t, &t, 4);
        break;
    }
    case 0x03: g.u = (int32_t)bits(v, 0, 14); g.v = (int32_t)bits(v, 16, 14); break;   /* UV */
    case 0x04: case 0x0C:                                       /* XYZF2, XYZF3 */
        g.fog = (uint8_t)(v >> 56);
        vertex_kick((int32_t)bits(v, 0, 16), (int32_t)bits(v, 16, 16), (uint32_t)bits(v, 32, 24), reg == 0x04);
        break;
    case 0x05: case 0x0D:                                       /* XYZ2, XYZ3 */
        vertex_kick((int32_t)bits(v, 0, 16), (int32_t)bits(v, 16, 16), (uint32_t)(v >> 32), reg == 0x05);
        break;
    case 0x06: case 0x07: g.ctx[reg - 0x06].tex0 = v; break;   /* TEX0 */
    case 0x08: case 0x09: g.ctx[reg - 0x08].clamp = v; break;  /* CLAMP */
    case 0x0A: g.fog = (uint8_t)(v >> 56); break;              /* FOG */
    case 0x14: case 0x15: g.ctx[reg - 0x14].tex1 = v; break;   /* TEX1 */
    case 0x16: case 0x17: {                                     /* TEX2: TEX0's format and CLUT fields */
        const uint64_t mask = (0x3Full << 20) | (0xFFFFFFFull << 37);
        Context *c = &g.ctx[reg - 0x16];
        c->tex0 = (c->tex0 & ~mask) | (v & mask);
        break;
    }
    case 0x18: case 0x19: g.ctx[reg - 0x18].xyoffset = v; break;
    case 0x1A: g.prmodecont = v; break;
    case 0x1B: g.prmode = v; break;
    case 0x1C: g.texclut = v; break;
    case 0x3B: g.texa = v; break;
    case 0x3D: g.fogcol = v; break;
    case 0x40: case 0x41: g.ctx[reg - 0x40].scissor = v; break;
    case 0x42: case 0x43: g.ctx[reg - 0x42].alpha = v; break;
    case 0x45: g.dthe = v; break;
    case 0x46: g.colclamp = v; break;
    case 0x47: case 0x48: g.ctx[reg - 0x47].test = v; break;
    case 0x49: g.pabe = v; break;
    case 0x4A: case 0x4B: g.ctx[reg - 0x4A].fba = v; break;
    case 0x4C: case 0x4D: g.ctx[reg - 0x4C].frame = v; break;
    case 0x4E: case 0x4F: g.ctx[reg - 0x4E].zbuf = v; break;
    case 0x50: g.bitbltbuf = v; break;
    case 0x51: g.trxpos = v; break;
    case 0x52: g.trxreg = v; break;
    case 0x53:                                                  /* TRXDIR: start a transfer */
        switch (bits(v, 0, 2)) {
        case 0:
            g.trx_active = bits(g.trxreg, 0, 12) && bits(g.trxreg, 32, 12);
            g.trx_x = g.trx_y = 0;
            g.trx_ncarry = 0;
            break;
        case 2: local_to_local(); break;
        default: break;                                         /* local->host: nothing to send back */
        }
        break;
    case 0x54: gs_image_data(&v, 8); break;                     /* HWREG */
    default: break;                         /* TEXFLUSH, DIMX, SCANMSK, MIPTBP, SIGNAL/FINISH/LABEL */
    }
}

/* ---- GIF ---- */

static struct {
    int in_packet;            /* a tag was read and its data isn't finished */
    uint32_t nloop, loop, nreg, reg, flg, eop;
    uint64_t regs;
} gif;

static void packed(uint32_t desc, const uint64_t *q) {
    switch (desc) {
    case 0x00: write_reg(0x00, q[0] & 0x7FF); break;
    case 0x01:                                                  /* RGBA (Q comes from ST) */
        g.r = (uint8_t)q[0]; g.g = (uint8_t)(q[0] >> 32); g.b = (uint8_t)q[1]; g.a = (uint8_t)(q[1] >> 32);
        break;
    case 0x02: {                                                /* ST + Q */
        uint32_t s = (uint32_t)q[0], t = (uint32_t)(q[0] >> 32), qq = (uint32_t)q[1];
        memcpy(&g.s, &s, 4);
        memcpy(&g.t, &t, 4);
        memcpy(&g.q, &qq, 4);
        break;
    }
    case 0x03: g.u = (int32_t)bits(q[0], 0, 14); g.v = (int32_t)bits(q[0], 32, 14); break;
    case 0x04: case 0x0C: {                                     /* XYZF2/3 */
        int kick = desc == 0x04 && !bits(q[1], 47, 1);
        g.fog = (uint8_t)bits(q[1], 36, 8);
        vertex_kick((int32_t)bits(q[0], 0, 16), (int32_t)bits(q[0], 32, 16), (uint32_t)bits(q[1], 4, 24), kick);
        break;
    }
    case 0x05: case 0x0D: {                                     /* XYZ2/3 */
        int kick = desc == 0x05 && !bits(q[1], 47, 1);
        vertex_kick((int32_t)bits(q[0], 0, 16), (int32_t)bits(q[0], 32, 16), (uint32_t)q[1], kick);
        break;
    }
    case 0x0A: g.fog = (uint8_t)bits(q[1], 36, 8); break;      /* FOG */
    case 0x0E: write_reg((uint32_t)(q[1] & 0xFF), q[0]); break; /* A+D */
    case 0x0F: break;                                           /* NOP */
    default: write_reg(desc, q[0]); break;                      /* TEX0, CLAMP... */
    }
}

static void gif_qword(const uint64_t *q) {
    if (!gif.in_packet) {
        uint64_t tag = q[0];
        gif.nloop = (uint32_t)bits(tag, 0, 15);
        gif.eop = (uint32_t)bits(tag, 15, 1);
        gif.flg = (uint32_t)bits(tag, 58, 2);
        gif.nreg = (uint32_t)bits(tag, 60, 4);
        if (gif.nreg == 0) gif.nreg = 16;
        gif.regs = q[1];
        gif.loop = gif.reg = 0;
        g.q = 1.0f;
        if (bits(tag, 46, 1)) write_reg(0x00, bits(tag, 47, 11));   /* PRE: PRIM */
        gif.in_packet = gif.nloop != 0;
        return;
    }
    switch (gif.flg) {
    case 0:                                                   /* PACKED */
        packed((uint32_t)((gif.regs >> (4 * gif.reg)) & 15), q);
        if (++gif.reg == gif.nreg) {
            gif.reg = 0;
            gif.loop++;
        }
        break;
    case 1:                                                   /* REGLIST: two registers per quadword */
        for (int h = 0; h < 2 && gif.loop < gif.nloop; h++) {
            uint32_t r = (uint32_t)((gif.regs >> (4 * gif.reg)) & 15);
            if (r != 0x0E && r != 0x0F) write_reg(r, q[h]);
            if (++gif.reg == gif.nreg) {
                gif.reg = 0;
                gif.loop++;
            }
        }
        break;
    default:                                                  /* IMAGE */
        gs_image_data(q, 16);
        gif.loop++;
        break;
    }
    if (gif.loop >= gif.nloop) gif.in_packet = 0;
}

void gs_gif_write(const void *data, uint32_t qwords) {
    const uint64_t *q = data;
    for (uint32_t i = 0; i < qwords; i++, q += 2) gif_qword(q);
}

/* One whole packet (up to the tag with EOP) from memory; returns the bytes it took. */
uint32_t gs_gif_packet(const void *packet) {
    const uint64_t *q = packet;
    uint32_t used = 0;
    gif.in_packet = 0;
    for (;;) {
        gif_qword(q);
        int last_tag = gif.eop;
        q += 2;
        used += 16;
        while (gif.in_packet) {
            gif_qword(q);
            q += 2;
            used += 16;
        }
        if (last_tag) break;
    }
    return used;
}

/* ---- display ---- */

uint64_t gs_priv_reg(uint32_t offset);
void gs_priv_reg_set(uint32_t offset, uint64_t v);

/* What sceGsPutDispEnv writes: read circuit 2 (PMODE EN2). */
void gs_set_display(uint64_t pmode, uint64_t dispfb, uint64_t display) {
    gs_priv_reg_set(0x00, pmode);
    gs_priv_reg_set(0x90, dispfb);
    gs_priv_reg_set(0xA0, display);
}

int gs_read_display(uint32_t *out, int *w, int *h, int max_w, int max_h) {
    /* the game programs the privileged registers directly; show circuit 2 if enabled, else circuit 1 */
    uint64_t pmode = gs_priv_reg(0x00);
    int c2 = (pmode & 2) || !(pmode & 1);
    uint64_t dispfb = gs_priv_reg(c2 ? 0x90 : 0x70);
    uint64_t display = gs_priv_reg(c2 ? 0xA0 : 0x80);
    if (!display) return 0;
    uint32_t fbp = (uint32_t)bits(dispfb, 0, 9) * 32, fbw = (uint32_t)bits(dispfb, 9, 6);
    uint32_t psm = (uint32_t)bits(dispfb, 15, 5);
    uint32_t dbx = (uint32_t)bits(dispfb, 32, 11), dby = (uint32_t)bits(dispfb, 43, 11);
    uint32_t magh = (uint32_t)bits(display, 23, 4), dw = (uint32_t)bits(display, 32, 12);
    uint32_t dh = (uint32_t)bits(display, 44, 11);
    int width = (int)((dw + 1) / (magh + 1)), height = (int)dh + 1;
    if (width > max_w) width = max_w;
    if (height > max_h) height = max_h;
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            uint32_t c;
            if (psm == 2 || psm == 0xA) {
                c = read_raw(psm, fbp, fbw, dbx + (uint32_t)x, dby + (uint32_t)y);
                c = (c & 31) << 3 | ((c >> 5) & 31) << 11 | ((c >> 10) & 31) << 19;
            } else {
                c = read_raw(PSMCT32, fbp, fbw, dbx + (uint32_t)x, dby + (uint32_t)y);   /* A B G R */
            }
            out[y * width + x] = 0xFF000000u | (c & 0xFFu) << 16 | (c & 0xFF00u) | (c >> 16 & 0xFFu);
        }
    *w = width;
    *h = height;
    return 1;
}
