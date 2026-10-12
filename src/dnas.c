/* DNAS, Sony's online disc authentication, isn't used on the PC: the check the game runs
   (in NETDNAS.BIN, which is encrypted) always passes. Obsrv's servers don't need it. */
#include "common.h"

s32 func_743F50(s8 *a, s8 *b, u8 *data, s32 size, s32 bufSize) {
    return 1;
}
