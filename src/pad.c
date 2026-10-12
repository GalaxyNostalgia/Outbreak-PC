/* Controllers (Sony libpad2 + libdbc) for the PC port. For now: the library starts and sockets open, and no
   controller is plugged in. Keyboard and gamepads (SDL) come with the game's input code. */
#include "libpad2.h"
#include "log.h"

s32 sceDbcInit(void) { return 1; }
s32 sceDbcEnd(void) { return 1; }
s32 scePad2Init(s32 mode) { return 1; }
s32 scePad2End(void) { return 1; }

static int g_sockets;

s32 scePad2CreateSocket(scePad2SocketParam *param, void *buffer) {
    log_info("pad: socket for port %d slot %d", param->port, param->slot);
    return g_sockets < 8 ? g_sockets++ : -1;
}

s32 scePad2DeleteSocket(s32 socket) { return 1; }
s32 scePad2GetState(s32 socket) { return 0; }    /* no controller */
s32 scePad2Read(s32 socket, void *data) { return 0; }
s32 scePad2GetButtonProfile(s32 socket, u8 *profile) { return -1; }

/* libdbc: actuator (motor) info and data. No motors yet. */
s32 func_00189308(s32 socket, u8 *info) { return -1; }
s32 func_00189388(s32 socket, s32 n, u8 *mode, s32 len, u8 *data) { return 0; }

/* Memory cards (libmc2): not there yet (save files come later). */
s32 sceMc2Init(s32 mode) { return 0; }
static int g_mc_sockets;
s32 sceMc2CreateSocket(void *param, void *buffer) { return g_mc_sockets++; }
