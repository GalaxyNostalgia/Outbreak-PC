/* The IOP side, replaced on PC.
 *
 * On a PS2 the EE reboots the I/O processor, sends it driver modules (CD/DVD, HDD, pad, memory card,
 * sound, network) and talks to them over SIF RPC. The PC version has no IOP: each of those drivers is
 * replaced by native code elsewhere in src/. These calls report what the game needs to carry on:
 * the reboot and module loads succeed, the HDD's drivers report "no device" (so the game reads
 * everything from the disc), and SIF DMA finishes at once.
 */
#include "kernel.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

void sceSifInitRpc(u32 mode) { (void)mode; }
s32 sceSifRebootIop(const char *img) { log_info("iop: reboot with %s (nothing to do on PC)", img); return 1; }
s32 sceSifSyncIop(void) { return 1; }
s32 sceSifInitIopHeap(void) { return 0; }
s32 sceSifLoadFileReset(void) { return 0; }
s32 sceFsReset(void) { return 0; }
s32 sceCdInit(s32 mode) { (void)mode; return 1; }
s32 sceCdMmode(s32 media) { (void)media; return 1; }

/* IOP memory: the game copies module archives there; nothing reads them on PC. */
void *sceSifAllocIopHeap(u32 size) { return malloc(size ? size : 1); }
s32 sceSifFreeIopHeap(void *addr) { free(addr); return 0; }

u32 sceSifSetDma(sceSifDmaData *sdd, s32 len) { (void)sdd; (void)len; return 1; }
s32 sceSifDmaStat(u32 id) { (void)id; return -1; }   /* < 0: finished */

/* Modules. MODULE_NO_RESIDENT_END (1) means "loaded, found nothing to drive, unloaded": what a PS2
   reports for the HDD drivers when no HDD is attached. */
#define NO_RESIDENT_END 1

s32 sceSifSearchModuleByName(const char *name) { (void)name; return -1; }

s32 sceSifLoadModule(const char *path, s32 argLen, const char *args) {
    log_info("iop: load %s (replaced on PC)", path);
    return 1;
}

s32 sceSifLoadStartModule(const char *path, s32 argLen, const char *args, s32 *result) {
    log_info("iop: start %s (replaced on PC: no such device)", path);
    if (result) *result = NO_RESIDENT_END;
    return 1;
}

s32 sceSifLoadStartModuleBuffer(void *addr, s32 args, const char *argp, s32 *result) {
    if (result) *result = 0;
    return 1;
}

s32 sceDevctl(const char *name, s32 cmd, void *arg, u32 argLen, void *buf, u32 bufLen) {
    return -19;   /* -ENODEV */
}

/* SIF RPC: the EE side binds to an IOP server and calls it. The PC has no IOP; each driver the
   game talks to is answered here. So far: the Tamsoft sound driver (channels 0 and 1), whose
   answers are all zero (no sound yet). */
typedef struct {
    u32 pkt_addr;
    u32 rpc_id;
    s32 sema_id;
    u32 mode;
    u32 command;
    void *buff;
    void *cbuff;
    void (*end_function)(void *);
    void *end_param;
    void *server;
} sceSifClientData;

static u32 g_rpc_server_dummy;

s32 sceSifBindRpc(sceSifClientData *cd, u32 rpc_number, u32 mode) {
    cd->rpc_id = rpc_number;
    cd->server = &g_rpc_server_dummy;     /* "the IOP side is up" */
    return 0;
}

s32 sceSifCallRpc(sceSifClientData *cd, u32 fno, u32 mode, void *send, s32 ssize, void *recv, s32 rsize,
                  void (*end_func)(void *), void *end_param) {
    extern int pc_tsnd_rpc(u32 rpc_id, u32 fno, const void *send, s32 ssize, void *recv, s32 rsize);
    if (!pc_tsnd_rpc(cd->rpc_id, fno, send, ssize, recv, rsize)) {
        if (recv && rsize > 0 && recv != send) memset(recv, 0, (size_t)rsize);
    }
    if (end_func) end_func(end_param);     /* finished at once */
    return 0;
}
