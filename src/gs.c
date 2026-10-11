#include "gs.h"
#include "log.h"

#include <string.h>

#define VRAM_WORDS (4u * 1024 * 1024 / 4)
static uint32_t g_vram[VRAM_WORDS];

/* GS register state (context 1 only for now) */
static struct {
    uint64_t prim, rgbaq, frame, xyoffset, scissor, test;
    uint64_t bitbltbuf, trxpos, trxreg;
    uint64_t dispfb, display, pmode;
    int nverts;
    struct { int32_t x, y; uint32_t color; } v[3];
    /* host->local transfer in progress */
    int trx_active;
    uint32_t trx_x, trx_y;
} g;

/* PSMCT32 layout: pages of 64x32 pixels, 32 blocks of 8x8 per page, 64 words per block */
static const uint8_t block32[4][8] = {
    {0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const uint8_t column32[8][8] = {
    {0, 1, 4, 5, 8, 9, 12, 13},         {2, 3, 6, 7, 10, 11, 14, 15},
    {16, 17, 20, 21, 24, 25, 28, 29},   {18, 19, 22, 23, 26, 27, 30, 31},
    {32, 33, 36, 37, 40, 41, 44, 45},   {34, 35, 38, 39, 42, 43, 46, 47},
    {48, 49, 52, 53, 56, 57, 60, 61},   {50, 51, 54, 55, 58, 59, 62, 63}};

/* word address of pixel (x, y) in a PSMCT32 buffer starting at block bp, bw*64 pixels wide */
static uint32_t addr32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    uint32_t page = (y >> 5) * bw + (x >> 6);
    uint32_t block = (bp + block32[(y >> 3) & 3][(x >> 3) & 7]) & 31;
    uint32_t page_base = (bp & ~31u) + page * 32;          /* in blocks */
    return ((page_base + block) * 64 + column32[y & 7][x & 7]) % VRAM_WORDS;
}

void gs_reset(void) {
    memset(&g, 0, sizeof g);
    memset(g_vram, 0, sizeof g_vram);
}

static uint64_t bits(uint64_t v, int lo, int n) { return (v >> lo) & ((1ull << n) - 1); }

static void draw_sprite(void) {
    /* flat-colour sprite between two vertices, in window coordinates, clipped to the scissor */
    int32_t ofx = (int32_t)bits(g.xyoffset, 0, 16), ofy = (int32_t)bits(g.xyoffset, 32, 16);
    int32_t x0 = (g.v[0].x - ofx) >> 4, y0 = (g.v[0].y - ofy) >> 4;
    int32_t x1 = (g.v[1].x - ofx) >> 4, y1 = (g.v[1].y - ofy) >> 4;
    if (x0 > x1) { int32_t t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int32_t t = y0; y0 = y1; y1 = t; }
    int32_t sx0 = (int32_t)bits(g.scissor, 0, 11), sx1 = (int32_t)bits(g.scissor, 16, 11);
    int32_t sy0 = (int32_t)bits(g.scissor, 32, 11), sy1 = (int32_t)bits(g.scissor, 48, 11);
    if (x0 < sx0) x0 = sx0;
    if (y0 < sy0) y0 = sy0;
    if (x1 > sx1 + 1) x1 = sx1 + 1;
    if (y1 > sy1 + 1) y1 = sy1 + 1;
    uint32_t fbp = (uint32_t)bits(g.frame, 0, 9) * 32, fbw = (uint32_t)bits(g.frame, 16, 6);
    uint32_t psm = (uint32_t)bits(g.frame, 24, 6);
    if (psm != 0) {
        log_warn("gs: sprite into PSM %u not supported yet", psm);
        return;
    }
    if (bits(g.prim, 4, 1)) {
        log_warn("gs: textured sprites not supported yet");
        return;
    }
    uint32_t color = g.v[1].color;
    for (int32_t y = y0; y < y1; y++)
        for (int32_t x = x0; x < x1; x++)
            g_vram[addr32(fbp, fbw, (uint32_t)x, (uint32_t)y)] = color;
}

static void vertex_kick(uint64_t xyz) {
    int type = (int)bits(g.prim, 0, 3);
    g.v[g.nverts].x = (int32_t)bits(xyz, 0, 16);
    g.v[g.nverts].y = (int32_t)bits(xyz, 16, 16);
    g.v[g.nverts].color = (uint32_t)g.rgbaq;
    g.nverts++;
    if (type == 6 && g.nverts == 2) {        /* sprite */
        draw_sprite();
        g.nverts = 0;
    } else if (type != 6 && g.nverts == 3) {
        log_warn("gs: primitive type %d not supported yet", type);
        g.nverts = 0;
    }
}

static void write_reg(uint32_t reg, uint64_t v) {
    switch (reg) {
    case 0x00: g.prim = v; g.nverts = 0; break;                 /* PRIM */
    case 0x01: g.rgbaq = v; break;                              /* RGBAQ */
    case 0x05: vertex_kick(v); break;                           /* XYZ2 */
    case 0x18: g.xyoffset = v; break;                           /* XYOFFSET_1 */
    case 0x40: g.scissor = v; break;                            /* SCISSOR_1 */
    case 0x47: g.test = v; break;                               /* TEST_1 */
    case 0x4C: g.frame = v; break;                              /* FRAME_1 */
    case 0x50: g.bitbltbuf = v; break;                          /* BITBLTBUF */
    case 0x51: g.trxpos = v; break;                             /* TRXPOS */
    case 0x52: g.trxreg = v; break;                             /* TRXREG */
    case 0x53:                                                  /* TRXDIR: start a transfer */
        if (bits(v, 0, 2) == 0) {
            g.trx_active = 1;
            g.trx_x = 0;
            g.trx_y = 0;
        }
        break;
    default: break;                         /* registers that don't affect what we draw yet */
    }
}

void gs_image_data(const void *src, uint32_t bytes) {
    if (!g.trx_active) return;
    uint32_t dbp = (uint32_t)bits(g.bitbltbuf, 32, 14), dbw = (uint32_t)bits(g.bitbltbuf, 48, 6);
    uint32_t dpsm = (uint32_t)bits(g.bitbltbuf, 56, 6);
    uint32_t dx = (uint32_t)bits(g.trxpos, 32, 11), dy = (uint32_t)bits(g.trxpos, 48, 11);
    uint32_t w = (uint32_t)bits(g.trxreg, 0, 12), h = (uint32_t)bits(g.trxreg, 32, 12);
    if (dpsm != 0) {
        log_warn("gs: upload in PSM %u not supported yet", dpsm);
        g.trx_active = 0;
        return;
    }
    const uint32_t *p = src;
    for (uint32_t n = bytes / 4; n && g.trx_y < h; n--) {
        g_vram[addr32(dbp, dbw, dx + g.trx_x, dy + g.trx_y)] = *p++;
        if (++g.trx_x == w) {
            g.trx_x = 0;
            g.trx_y++;
        }
    }
    if (g.trx_y >= h) g.trx_active = 0;
}

uint32_t gs_gif_packet(const void *packet) {
    const uint64_t *q = packet;
    uint32_t used = 0;
    for (;;) {
        uint64_t tag = q[0], regs = q[1];
        uint32_t nloop = (uint32_t)bits(tag, 0, 15), eop = (uint32_t)bits(tag, 15, 1);
        uint32_t pre = (uint32_t)bits(tag, 46, 1), flg = (uint32_t)bits(tag, 58, 2);
        uint32_t nreg = (uint32_t)bits(tag, 60, 4);
        if (nreg == 0) nreg = 16;
        q += 2;
        used += 16;
        if (pre && flg == 0) write_reg(0x00, bits(tag, 47, 11));
        if (flg == 0) {                                   /* PACKED */
            for (uint32_t i = 0; i < nloop; i++)
                for (uint32_t r = 0; r < nreg; r++) {
                    uint32_t desc = (uint32_t)((regs >> (4 * r)) & 15);
                    if (desc == 0x0E) write_reg((uint32_t)(q[1] & 0xFF), q[0]);      /* A+D */
                    else if (desc == 0x01) write_reg(0x01, (q[0] & 0xFFFFFFFFull));  /* RGBAQ, simplified */
                    else log_warn("gs: GIF PACKED descriptor %u not supported yet", desc);
                    q += 2;
                    used += 16;
                }
        } else if (flg == 2) {                            /* IMAGE */
            gs_image_data(q, nloop * 16);
            q += 2 * nloop;
            used += 16 * nloop;
        } else {
            log_warn("gs: GIF REGLIST mode not supported yet");
            q += 2 * nloop;
            used += 16 * nloop;
        }
        if (eop) break;
    }
    return used;
}

void gs_set_display(uint64_t pmode, uint64_t dispfb, uint64_t display) {
    g.pmode = pmode;
    g.dispfb = dispfb;
    g.display = display;
}

int gs_read_display(uint32_t *out, int *w, int *h, int max_w, int max_h) {
    if (!g.display) return 0;
    uint32_t fbp = (uint32_t)bits(g.dispfb, 0, 9) * 32, fbw = (uint32_t)bits(g.dispfb, 9, 6);
    uint32_t magh = (uint32_t)bits(g.display, 23, 4), dw = (uint32_t)bits(g.display, 32, 12);
    uint32_t dh = (uint32_t)bits(g.display, 44, 11);
    int width = (int)((dw + 1) / (magh + 1)), height = (int)dh + 1;
    if (width > max_w) width = max_w;
    if (height > max_h) height = max_h;
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            uint32_t c = g_vram[addr32(fbp, fbw, (uint32_t)x, (uint32_t)y)];   /* A B G R in memory order */
            out[y * width + x] = 0xFF000000u | (c & 0xFFu) << 16 | (c & 0xFF00u) | (c >> 16 & 0xFFu);
        }
    *w = width;
    *h = height;
    return 1;
}
