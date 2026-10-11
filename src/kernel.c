/* Sony EE kernel calls for the PC port: interrupt and DMA-completion handlers, semaphores.
 *
 * The PC version runs the game on one OS thread. Interrupt handlers the game registers are called from
 * the places the PS2 would raise them: v-blank from the display pacing (sceGsSyncV), DMA completion when
 * a transfer the game kicks finishes. Threads come later (the boot path doesn't start any on PC).
 */
#include "kernel.h"
#include "log.h"

#include <string.h>

typedef s32 (*Handler)(s32 cause, void *arg, void *addr);

typedef struct {
    Handler fn;
    void *arg;
    int used;
} HandlerSlot;

#define MAX_HANDLERS 16
static HandlerSlot g_intc[16][MAX_HANDLERS], g_dmac[16][MAX_HANDLERS];
static u32 g_intc_mask, g_dmac_mask;

static s32 add(HandlerSlot (*table)[MAX_HANDLERS], s32 cause, Handler fn, void *arg) {
    if (cause < 0 || cause >= 16) return -1;
    for (int i = 0; i < MAX_HANDLERS; i++)
        if (!table[cause][i].used) {
            table[cause][i] = (HandlerSlot){fn, arg, 1};
            return cause * MAX_HANDLERS + i + 1;
        }
    return -1;
}

static s32 remove_handler(HandlerSlot (*table)[MAX_HANDLERS], s32 cause, s32 id) {
    int i = id - 1 - cause * MAX_HANDLERS;
    if (cause < 0 || cause >= 16 || i < 0 || i >= MAX_HANDLERS) return -1;
    table[cause][i].used = 0;
    return 0;
}

/* AddIntcHandler(cause, handler, next): handler(cause); AddIntcHandler2(cause, handler, next, arg):
   handler(cause, arg, addr). Calling both with three arguments is harmless in the C calling convention. */
s32 AddIntcHandler(s32 cause, s32 (*handler)(s32), s32 next) { return add(g_intc, cause, (Handler)handler, NULL); }
s32 AddIntcHandler2(s32 cause, s32 (*handler)(s32, void *, void *), s32 next, void *arg) {
    return add(g_intc, cause, handler, arg);
}
s32 RemoveIntcHandler(s32 cause, s32 id) { return remove_handler(g_intc, cause, id); }
s32 AddDmacHandler(s32 channel, s32 (*handler)(s32), s32 next) { return add(g_dmac, channel, (Handler)handler, NULL); }
s32 AddDmacHandler2(s32 channel, s32 (*handler)(s32, void *, void *), s32 next, void *arg) {
    return add(g_dmac, channel, handler, arg);
}
s32 RemoveDmacHandler(s32 channel, s32 id) { return remove_handler(g_dmac, channel, id); }

s32 EnableIntc(s32 cause) { s32 was = (g_intc_mask >> cause) & 1; g_intc_mask |= 1u << cause; return was; }
s32 DisableIntc(s32 cause) { s32 was = (g_intc_mask >> cause) & 1; g_intc_mask &= ~(1u << cause); return was; }
s32 EnableDmac(s32 channel) { s32 was = (g_dmac_mask >> channel) & 1; g_dmac_mask |= 1u << channel; return was; }
s32 DisableDmac(s32 channel) { s32 was = (g_dmac_mask >> channel) & 1; g_dmac_mask &= ~(1u << channel); return was; }
s32 _EnableIntc(s32 cause) { return EnableIntc(cause); }
s32 _DisableIntc(s32 cause) { return DisableIntc(cause); }
s32 _EnableDmac(s32 channel) { return EnableDmac(channel); }
s32 _DisableDmac(s32 channel) { return DisableDmac(channel); }

/* Interrupts are never "running" on PC, so disabling them only needs to report the previous state. */
static int g_intr_on = 1;
s32 DIntr(void) { int was = g_intr_on; g_intr_on = 0; return was; }
s32 EIntr(void) { int was = g_intr_on; g_intr_on = 1; return was; }

static void raise(HandlerSlot (*table)[MAX_HANDLERS], u32 mask, s32 cause) {
    if (!(mask & (1u << cause))) return;
    for (int i = 0; i < MAX_HANDLERS; i++)
        if (table[cause][i].used && table[cause][i].fn(cause, table[cause][i].arg, NULL) == -1)
            break;   /* -1: don't call the following handlers */
}

void pc_raise_intc(s32 cause) { raise(g_intc, g_intc_mask, cause); }
void pc_raise_dmac(s32 channel) { raise(g_dmac, g_dmac_mask, channel); }

/* Semaphores: one OS thread, so waiting on an empty semaphore would wait forever; that's reported. */
#define MAX_SEMA 64
static struct { int used; s32 count, max; } g_sema[MAX_SEMA];

s32 CreateSema(struct SemaParam *p) {
    for (int i = 1; i < MAX_SEMA; i++)
        if (!g_sema[i].used) {
            g_sema[i].used = 1;
            g_sema[i].count = p->initCount;
            g_sema[i].max = p->maxCount;
            return i;
        }
    return -1;
}
s32 DeleteSema(s32 id) { if (id <= 0 || id >= MAX_SEMA) return -1; g_sema[id].used = 0; return id; }
s32 SignalSema(s32 id) {
    if (id <= 0 || id >= MAX_SEMA || !g_sema[id].used) return -1;
    if (g_sema[id].count < g_sema[id].max || g_sema[id].max <= 0) g_sema[id].count++;
    return id;
}
s32 iSignalSema(s32 id) { return SignalSema(id); }
s32 PollSema(s32 id) {
    if (id <= 0 || id >= MAX_SEMA || !g_sema[id].used || g_sema[id].count <= 0) return -1;
    g_sema[id].count--;
    return id;
}
s32 iPollSema(s32 id) { return PollSema(id); }
s32 WaitSema(s32 id) {
    if (PollSema(id) == id) return id;
    log_error("kernel: WaitSema(%d) would wait forever (no threads on PC yet)", id);
    return id;
}

s32 GetThreadId(void) { return 1; }
s32 ChangeThreadPriority(s32 id, s32 priority) { return 0; }

s32 iEnableIntc(s32 cause) { return EnableIntc(cause); }
s32 iDisableIntc(s32 cause) { return DisableIntc(cause); }
s32 iEnableDmac(s32 channel) { return EnableDmac(channel); }
s32 iDisableDmac(s32 channel) { return DisableDmac(channel); }
void iFlushCache(s32 op) {}
