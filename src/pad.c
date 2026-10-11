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
