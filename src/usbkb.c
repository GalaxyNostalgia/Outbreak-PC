/* Sony libusbkb (USB keyboards, for chat) on the PC: no keyboard yet. sceUsbKbInit reports
   that none can connect, so the game skips keyboard set-up. Typing will come through SDL text
   input later. */
#include "common.h"

s32 func_001D8230(s32 *max) { if (max) *max = 0; return 0; }   /* sceUsbKbInit */
s32 func_001D84F0(void *info) { return -1; }                     /* sceUsbKbGetInfo */
s32 func_001D8830(s32 no, s32 mode) { return -1; }
s32 func_001D8930(s32 no, s32 a, s32 b) { return -1; }
s32 func_001D8978(s32 no, s32 a) { return -1; }
s32 func_001D89C8(s32 no, s32 a) { return -1; }
s32 func_001D8A18(s32 mode, s32 *result) { if (result) *result = -1; return 0; } /* sceUsbKbSync */
s32 func_001D9638(s32 no, s32 a) { return -1; }
