/* Backing store for the fixed main-RAM addresses the game uses as its own buffers
   (PS2_RAM(addr) in decomp/include/ps2mem.h). 32 MiB like the PS2. */
#include <stdint.h>

uint8_t pc_ram[32u * 1024 * 1024];
