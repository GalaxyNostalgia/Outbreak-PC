/* The PS2's DMA controller, VIF1 and GIF paths, for the PC port.
 *
 * The game builds DMA chains (tags + VIF codes + GIF packets) exactly as on the PS2 and hands them to
 * sceDmaSend. Here a send runs the whole chain at once: GIF data goes to the GS model (gs.c), VIF1's
 * DIRECT/DIRECTHL go to the GS too, and VU1 microprograms (the 3D renderer) are skipped for now. When the
 * transfer ends, the channel's registers look like the hardware's afterwards and, if the game asked for it,
 * its DMA-end interrupt handler runs.
 */
#include "libdma.h"
#include "gs.h"
#include "log.h"

#include <stdint.h>
#include <string.h>

volatile u32 *pc_hw_reg(u32 addr);
void pc_raise_dmac(s32 channel);

static const u32 chan_base[10] = {0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
                                  0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400};

sceDmaChan *sceDmaGetChan(s32 id) {
    if (id < 0 || id >= 10) return NULL;
    return (sceDmaChan *)pc_hw_reg(chan_base[id]);
}

static int chan_id(const sceDmaChan *chan) {
    for (int i = 0; i < 10; i++)
        if ((const void *)pc_hw_reg(chan_base[i]) == (const void *)chan) return i;
    return -1;
}

/* Addresses in DMA tags: physical main RAM (the PC pointers the game handed out are below 0x10000000).
   Bit 31 selects the scratchpad, which the game would only use with the SPR channels. */
static const uint32_t *phys(uint32_t addr) {
    if (addr & 0x80000000u) {
        log_warn("dma: scratchpad address %08x in a tag", addr);
        return NULL;
    }
    return (const uint32_t *)(uintptr_t)(addr & 0x0FFFFFFF);
}

/* ---- VIF1 ---- */
static struct {
    uint32_t cmd, imm;        /* command waiting for data */
    uint32_t need;            /* words of data still to come */
    uint32_t cl, wl;          /* STCYCL */
    int warned_vu;
    uint32_t gif_buf[4];      /* DIRECT data arrives in words; the GIF takes quadwords */
    int gif_n;
} vif;

static uint32_t unpack_words(uint32_t cmd, uint32_t imm) {
    uint32_t num = (imm >> 16) & 0xFF, vn = (cmd >> 2) & 3, vl = cmd & 3;
    if (num == 0) num = 256;
    uint32_t reads = num;
    if (vif.wl > vif.cl && vif.wl) reads = vif.cl * (num / vif.wl) + (num % vif.wl < vif.cl ? num % vif.wl : vif.cl);
    uint32_t bits = (32u >> vl) * (vn + 1) * reads;
    return (bits + 31) / 32;
}

static void vif1_word(uint32_t w) {
    if (vif.need) {
        vif.need--;
        if (vif.cmd == 0x50 || vif.cmd == 0x51) {          /* DIRECT/DIRECTHL: GIF PATH2 */
            vif.gif_buf[vif.gif_n++] = w;
            if (vif.gif_n == 4) {
                gs_gif_write(vif.gif_buf, 1);
                vif.gif_n = 0;
            }
        } else if (vif.cmd == 0x01) {
        }
        return;
    }
    uint32_t cmd = (w >> 24) & 0x7F, imm = w & 0xFFFF;
    vif.cmd = cmd;
    vif.imm = imm;
    switch (cmd) {
    case 0x00: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x10: case 0x11: case 0x13:
        break;                                             /* NOP, OFFSET, BASE, ITOP, STMOD, MSKPATH3, MARK, FLUSH* */
    case 0x01:
        vif.cl = imm & 0xFF;
        vif.wl = (imm >> 8) & 0xFF;
        break;
    case 0x14: case 0x15: case 0x17:                       /* MSCAL, MSCALF, MSCNT: run a VU1 microprogram */
        if (!vif.warned_vu) {
            log_warn("vif1: the game runs VU1 microprograms (3D drawing); not replaced yet");
            vif.warned_vu = 1;
        }
        break;
    case 0x20: vif.need = 1; break;                        /* STMASK */
    case 0x30: case 0x31: vif.need = 4; break;             /* STROW, STCOL */
    case 0x4A: vif.need = 2 * (((w >> 16) & 0xFF) ? ((w >> 16) & 0xFF) : 256); break;   /* MPG */
    case 0x50: case 0x51:
        vif.need = 4 * (imm ? imm : 0x10000);
        vif.gif_n = 0;
        break;
    default:
        if (cmd >= 0x60) vif.need = unpack_words(cmd, w >> 0 & 0x00FFFFFF);
        else log_warn("vif1: unknown command %02x", cmd);
        break;
    }
}

static void vif1_data(const uint32_t *p, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) vif1_word(p[i]);
}

/* ---- channel transfers ---- */
static void deliver(int id, const uint32_t *data, uint32_t qwc) {
    if (!data || !qwc) return;
    if (id == 1) vif1_data(data, qwc * 4);
    else if (id == 2) gs_gif_write(data, qwc);
    else log_warn("dma: channel %d isn't modelled", id);
}

static uint32_t g_pending;
static int g_depth;

static void run_chain(int id, sceDmaChan *chan, uint32_t tadr) {
    uint32_t stack[2], sp = 0, last_tag = 0;
    for (int guard = 0; guard < 1000000; guard++) {
        const uint32_t *tag = phys(tadr);
        if (!tag) break;
        uint32_t qwc = tag[0] & 0xFFFF, idt = (tag[0] >> 28) & 7, addr = tag[1] & 0x7FFFFFFF;
        last_tag = tag[0];
        if (id == 1 && chan->chcr.TTE) vif1_data(tag + 2, 2);   /* the tag's upper half goes to VIF1 */
        int end = 0;
        switch (idt) {
        case 0: deliver(id, phys(addr), qwc); end = 1; break;                     /* refe */
        case 1: deliver(id, tag + 4, qwc); tadr += 16 + qwc * 16; break;          /* cnt */
        case 2: deliver(id, tag + 4, qwc); tadr = addr; break;                    /* next */
        case 3: case 4: deliver(id, phys(addr), qwc); tadr += 16; break;          /* ref, refs */
        case 5:                                                                   /* call */
            deliver(id, tag + 4, qwc);
            if (sp < 2) stack[sp++] = tadr + 16 + qwc * 16;
            tadr = addr;
            break;
        case 6:                                                                   /* ret */
            deliver(id, tag + 4, qwc);
            if (sp) tadr = stack[--sp];
            else end = 1;
            break;
        case 7: deliver(id, tag + 4, qwc); end = 1; break;                        /* end */
        }
        if (end || ((last_tag & 0x80000000u) && chan->chcr.TIE)) break;           /* IRQ with TIE stops */
    }
    chan->chcr.TAG = last_tag >> 16;
}

void sceDmaSend(sceDmaChan *chan, void *tag) {
    int id = chan_id(chan);
    if (id < 0) {
        log_error("dma: send on an unknown channel");
        return;
    }
    uint32_t addr = (uint32_t)(uintptr_t)tag;
    chan->chcr.STR = 1;
    if (chan->chcr.MOD == 1 || 1) {         /* the game's sends are all chains */
        chan->chcr.MOD = 1;
        run_chain(id, chan, addr);
    }
    chan->chcr.STR = 0;
    g_pending |= 1u << id;
    if (g_depth == 0) {                     /* raise end interrupts outside the send, as the hardware would */
        g_depth = 1;
        while (g_pending) {
            int ch = __builtin_ctz(g_pending);
            g_pending &= g_pending - 1;
            pc_raise_dmac(ch);
        }
        g_depth = 0;
    }
}

void sceDmaRecvN(sceDmaChan *chan, void *addr, s32 qwc) {
    log_warn("dma: reading %d quadwords back from the GS isn't supported yet", qwc);
    memset(addr, 0, (size_t)qwc * 16);
}

s32 sceDmaSync(sceDmaChan *chan, s32 mode, s32 timeout) {
    return 0;     /* transfers finish inside sceDmaSend */
}
