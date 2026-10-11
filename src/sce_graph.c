/* Sony graphics library (libgraph) and hardware-reset calls, implemented for PC.
 *
 * The game builds the same packets as on PS2 (struct layouts in decomp/include/libgraph.h);
 * these functions hand them to the GS model (gs.c) instead of DMA, and SyncV shows the
 * displayed buffer in the window.
 */
#include "libgraph.h"
#include "display.h"
#include "gs.h"
#include "log.h"

#include <string.h>

#define GS_FRAME_1 0x4C
#define GS_ZBUF_1 0x4E
#define GS_XYOFFSET_1 0x18
#define GS_SCISSOR_1 0x40
#define GS_PRMODECONT 0x1A
#define GS_COLCLAMP 0x46
#define GS_DTHE 0x45
#define GS_TEST_1 0x47
#define GS_PRIM 0x00
#define GS_RGBAQ 0x01
#define GS_XYZ2 0x05
#define GS_BITBLTBUF 0x50
#define GS_TRXPOS 0x51
#define GS_TRXREG 0x52
#define GS_TRXDIR 0x53

static int g_field;

static void giftag(sceGifTag *t, u32 nloop, int eop, u32 flg, u32 nreg, u64 regs) {
    t->tag[0] = (u64)nloop | (u64)(eop != 0) << 15 | (u64)flg << 58 | (u64)nreg << 60;
    t->tag[1] = regs;
}

void sceDevVif0Reset(void) {}
void sceDevVif1Reset(void) {}
void sceDevVu0Reset(void) {}
void sceDevVu1Reset(void) {}
void sceDmaReset(s32 mode) { (void)mode; }
s32 FlushCache(s32 op) { (void)op; return 0; }
void sceGsResetPath(void) {}
s32 sceGsSyncPath(s32 mode, u16 timeout) { (void)mode; (void)timeout; return 0; }

void sceGsResetGraph(s16 mode, s16 inter, s16 omode, s16 ffmode) {
    log_info("gs: reset (interlace %d, mode %d, %s)", inter, omode, ffmode ? "field" : "frame");
    gs_reset();
    display_open();
}

static void set_disp(sceGsDispEnv *d, s16 psm, s16 w, s16 h) {
    memset(d, 0, sizeof *d);
    d->pmode = 0x0000FF66ull | 1;                 /* read circuit 1, full alpha */
    d->smode2 = 1;                                /* interlaced, frame mode */
    d->dispfb.FBW = (u64)((w + 63) / 64);
    d->dispfb.PSM = (u64)psm;
    /* DX, DY, MAGH=3 (640 px), DW, DH */
    d->display = 636ull | 50ull << 12 | 3ull << 23 | (u64)(w * 4 - 1) << 32 | (u64)(h - 1) << 44;
}

static void set_draw(sceGsDrawEnv1 *e, s16 psm, s16 w, s16 h, s16 ztest, s16 zpsm) {
    u64 ofx = (u64)(2048 - (w >> 1)) << 4, ofy = (u64)(2048 - (h >> 1)) << 4;
    memset(e, 0, sizeof *e);
    e->frame1.FBW = (u64)((w + 63) / 64);
    e->frame1.PSM = (u64)psm;
    e->frame1addr = GS_FRAME_1;
    e->zbuf1 = (u64)zpsm << 24 | (u64)(ztest == 0) << 32;
    e->zbuf1addr = GS_ZBUF_1;
    e->xyoffset1 = ofx | ofy << 32;
    e->xyoffset1addr = GS_XYOFFSET_1;
    e->scissor1 = (u64)(w - 1) << 16 | (u64)(h - 1) << 48;
    e->scissor1addr = GS_SCISSOR_1;
    e->prmodecont = 1;
    e->prmodecontaddr = GS_PRMODECONT;
    e->colclamp = 1;
    e->colclampaddr = GS_COLCLAMP;
    e->dthe = 0;
    e->dtheaddr = GS_DTHE;
    e->test1 = ztest ? (1ull << 16 | (u64)ztest << 17) : (1ull << 16 | 1ull << 17);
    e->test1addr = GS_TEST_1;
}

static void set_clear(sceGsClear *c, const sceGsDrawEnv1 *e, s16 w, s16 h) {
    u64 ofx = e->xyoffset1 & 0xFFFF, ofy = e->xyoffset1 >> 32 & 0xFFFF;
    u64 *d = c->data;
    d[0] = 1ull << 16 | 1ull << 17;  d[1] = GS_TEST_1;     /* depth test: always */
    d[2] = 6;                        d[3] = GS_PRIM;       /* sprite */
    d[4] = 0x80000000ull;            d[5] = GS_RGBAQ;      /* black */
    d[6] = ofx | ofy << 16;          d[7] = GS_XYZ2;
    d[8] = (ofx + ((u64)w << 4)) | (ofy + ((u64)h << 4)) << 16; d[9] = GS_XYZ2;
    d[10] = e->test1;                d[11] = GS_TEST_1;
}

void sceGsSetDefDBuff(sceGsDBuff *db, s16 psm, s16 w, s16 h, s16 ztest, s16 zpsm, s16 clear) {
    u32 pages = (u32)((w + 63) / 64) * (u32)((h + 31) / 32);
    memset(db, 0, sizeof *db);
    set_disp(&db->disp[0], psm, w, h);
    set_disp(&db->disp[1], psm, w, h);
    set_draw(&db->draw0, psm, w, h, ztest, zpsm);
    set_draw(&db->draw1, psm, w, h, ztest, zpsm);
    if (clear) {
        set_clear(&db->clear0, &db->draw0, w, h);
        set_clear(&db->clear1, &db->draw1, w, h);
    }
    giftag(&db->giftag0, clear ? 14 : 8, 1, 0, 1, 0xE);
    giftag(&db->giftag1, clear ? 14 : 8, 1, 0, 1, 0xE);
    /* buffer A at page 0, buffer B after it: disp[1] shows B, draw0 draws into B */
    db->disp[1].dispfb.FBP = pages;
    db->draw0.frame1.FBP = pages;
}

void sceGsPutDispEnv(sceGsDispEnv *d) {
    u64 dispfb;
    memcpy(&dispfb, &d->dispfb, 8);
    gs_set_display(d->pmode, dispfb, d->display);
}

void sceGsPutDrawEnv(sceGifTag *packet) {
    gs_gif_packet(packet);
}

s32 sceGsSwapDBuff(sceGsDBuff *db, s32 id) {
    id &= 1;
    sceGsPutDispEnv(&db->disp[id]);
    sceGsPutDrawEnv(id ? &db->giftag1 : &db->giftag0);
    return 0;
}

/* The v-blank: sceGsSyncV waits for it (display_vsync paces to 59.94 Hz), then whatever the game hooked
   on it runs, as the PS2's v-blank interrupt would have run it. */
static s32 (*g_vsync_callback)(s32);

void *sceGsSyncVCallback(s32 (*func)(s32)) {
    void *old = (void *)g_vsync_callback;
    g_vsync_callback = func;
    return old;
}

void pc_raise_intc(s32 cause);

s32 sceGsSyncV(s32 mode) {
    static int in_vblank;
    (void)mode;
    display_vsync();
    g_field ^= 1;
    if (!in_vblank) {                /* a handler that waits for the next v-blank mustn't recurse */
        in_vblank = 1;
        if (g_vsync_callback) g_vsync_callback(2);
        pc_raise_intc(2);
        in_vblank = 0;
    }
    return g_field;
}

s32 sceGsSetDefLoadImage(sceGsLoadImage *lp, s16 dbp, s16 dbw, s16 dpsm, s16 x, s16 y, s16 w, s16 h) {
    static const int bits_per_pixel[64] = {[0] = 32, [1] = 24, [2] = 16, [10] = 16, [19] = 8, [20] = 4,
                                           [27] = 8, [36] = 4, [44] = 4};
    int bpp = bits_per_pixel[dpsm & 63] ? bits_per_pixel[dpsm & 63] : 32;
    u32 qwc = (u32)w * (u32)h * (u32)bpp / 128;
    if (qwc > 0x7FFF) {
        log_warn("sceGsSetDefLoadImage: %dx%d is too big for one transfer", w, h);
        return 0;
    }
    memset(lp, 0, sizeof *lp);
    giftag(&lp->giftag0, 4, 0, 0, 1, 0xE);
    lp->bitbltbuf = (u64)(dbp & 0x3FFF) << 32 | (u64)(dbw & 63) << 48 | (u64)(dpsm & 63) << 56;
    lp->bitbltbufaddr = GS_BITBLTBUF;
    lp->trxpos = (u64)(x & 0x7FF) << 32 | (u64)(y & 0x7FF) << 48;
    lp->trxposaddr = GS_TRXPOS;
    lp->trxreg = (u64)(w & 0xFFF) | (u64)(h & 0xFFF) << 32;
    lp->trxregaddr = GS_TRXREG;
    lp->trxdir = 0;
    lp->trxdiraddr = GS_TRXDIR;
    giftag(&lp->giftag1, qwc, 1, 2, 0, 0);
    return 6;
}

s32 sceGsExecLoadImage(sceGsLoadImage *lp, void *src) {
    sceGsLoadImage head = *lp;
    head.giftag0.tag[0] |= 1ull << 15;                 /* end after the four registers */
    gs_gif_packet(&head);
    gs_image_data(src, (u32)(lp->giftag1.tag[0] & 0x7FFF) * 16);
    return 0;
}
