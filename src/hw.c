/* EE hardware registers and DMA channels, for the PC port.
 *
 * Game code that pokes hardware registers goes through HW_REG32() (decomp/include/ps2mem.h), which on PC
 * lands here. Timer counters advance with real time; every other register is backed by plain memory, so
 * writes are kept and reads give back what was written.
 */
#include "common.h"
#include "log.h"

#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define BUS_HZ 147456000u    /* EE bus clock: what the timers count when their mode says so */

static uint32_t g_regs[0x10000 / 4] __attribute__((aligned(16)));    /* 0x10000000-0x1000FFFF */
static uint32_t g_gs_priv[0x2000 / 4] __attribute__((aligned(8)));   /* 0x12000000-0x12001FFF */
static uint32_t g_other;

static uint64_t now_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart / freq.QuadPart) * 1000000000u + (uint64_t)(t.QuadPart % freq.QuadPart) * 1000000000u / freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
#endif
}

/* Timers 0-3 at 0x10000000 + n*0x800: COUNT +0, MODE +0x10, COMP +0x20, HOLD +0x30.
   MODE bits 0-1 pick the clock: bus, bus/16, bus/256, h-blank. */
static uint64_t g_timer_base[4];
static uint32_t g_timer_start[4];

static uint32_t timer_count(int n) {
    static const uint32_t div[4] = {1, 16, 256, 9370};   /* h-blank: ~15.7 kHz */
    uint32_t mode = g_regs[(n * 0x800 + 0x10) / 4];
    if (!(mode & 0x80)) return g_timer_start[n];           /* CUE: counting stopped */
    uint64_t ticks = (now_ns() - g_timer_base[n]) * (BUS_HZ / 1000000u) / 1000u / div[mode & 3];
    return (uint32_t)(g_timer_start[n] + ticks) & 0xFFFF;
}

/* The GS's privileged registers (PMODE, DISPFB, DISPLAY...): the display reads them from here. */
uint64_t gs_priv_reg(uint32_t offset) { return *(uint64_t *)&g_gs_priv[(offset & 0x1FFF) / 4]; }
void gs_priv_reg_set(uint32_t offset, uint64_t v) { *(uint64_t *)&g_gs_priv[(offset & 0x1FFF) / 4] = v; }

volatile u32 *pc_hw_reg(u32 addr) {
    if (addr >= 0x10000000 && addr < 0x10002000) {
        int n = (addr - 0x10000000) / 0x800, reg = (addr & 0x7FF);
        if (reg == 0x00) {                 /* COUNT: refresh on every access, remember writes */
            static uint32_t last[4];
            uint32_t *p = &g_regs[(addr - 0x10000000) / 4];
            if (*p != last[n]) {           /* the game wrote a new count */
                g_timer_start[n] = *p;
                g_timer_base[n] = now_ns();
            }
            *p = last[n] = timer_count(n);
            return p;
        }
        if (reg == 0x10) g_timer_base[n] = g_timer_base[n] ? g_timer_base[n] : now_ns();
    }
    if (addr >= 0x10000000 && addr < 0x10010000) return &g_regs[(addr - 0x10000000) / 4];
    if (addr >= 0x12000000 && addr < 0x12002000) return &g_gs_priv[(addr - 0x12000000) / 4];
    log_warn("hw: register %08x isn't modelled", addr);
    return &g_other;
}

/* func_0017FEB0 (asm in the game): resets VIF0 and VU0 and sends VIF0 its setup codes. There is no VIF0
   or VU0 on PC; the code the game runs on VU0 is replaced by C. */
void func_0017FEB0(void) {}
