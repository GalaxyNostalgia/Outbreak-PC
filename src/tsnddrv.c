/* The Tamsoft Sound Driver (TSNDDRV.IRX on the IOP), PC side.
 *
 * The game talks to it over two SIF RPC channels (decomp: sdr.c): number 0 takes a byte stream of
 * commands each frame (sound effects, volumes, bank uploads), number 1 answers state queries.
 * For now the commands are read and dropped, and every query answers 0 ("idle, nothing pending"),
 * which is what the game waits for. Playing the sounds comes later.
 */
#include "common.h"
#include "log.h"

#include <string.h>

#define TSND_DEV 0
#define TSND_GET 1

static unsigned g_streams, g_bytes;

static void commands(const u8 *p, s32 size) {
    g_streams++;
    g_bytes += (unsigned)size;
    (void)p;
}

/* Returns 1 if the call was for the sound driver */
int pc_tsnd_rpc(u32 rpc_id, u32 fno, const void *send, s32 ssize, void *recv, s32 rsize) {
    switch (rpc_id) {
    case TSND_DEV:
        if (send && ssize > 0) commands(send, ssize);
        return 1;
    case TSND_GET:
        if (recv && rsize >= 4) {
            memset(recv, 0, (size_t)rsize);
        }
        return 1;
    }
    return 0;
}
