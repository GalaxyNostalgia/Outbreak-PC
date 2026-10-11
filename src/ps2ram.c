/* Memory the game sees.
 *
 * pc_ram: backing store for the fixed main-RAM addresses the game uses as its own buffers (PS2_RAM(addr) in
 * decomp/include/ps2mem.h). 32 MiB like the PS2.
 *
 * pc_heap: what the game's malloc()/free() hand out (sce.h maps them to pc_game_malloc/pc_game_free). The game
 * takes one 22 MiB block at boot for its own heaps, plus a few small ones.
 *
 * Both live in the program image, so like everything else the game touches they sit below 0x10000000: the
 * game's DMA queue keeps a type in the top 4 bits of an address (main.c checks this at start-up).
 */
#include "log.h"

#include <stdint.h>
#include <string.h>

uint8_t pc_ram[32u * 1024 * 1024] __attribute__((aligned(64)));

#define HEAP_SIZE (40u * 1024 * 1024)
static uint8_t pc_heap[HEAP_SIZE] __attribute__((aligned(64)));

/* First-fit allocator with boundary headers; blocks are multiples of 64 bytes and 64-byte aligned
   (the PS2's malloc gives 8-byte alignment; DMA buffers need 16, and the game aligns its big block itself). */
typedef struct {
    uint32_t size;    /* whole block including this header */
    uint32_t used;
    uint32_t pad[14];
} Block;

static int g_heap_ready;

static void heap_init(void) {
    Block *b = (Block *)pc_heap;
    b->size = HEAP_SIZE;
    b->used = 0;
    g_heap_ready = 1;
}

void *pc_game_malloc(uint32_t size) {
    if (!g_heap_ready) heap_init();
    uint32_t need = (size + sizeof(Block) + 63) & ~63u;
    for (uint32_t off = 0; off < HEAP_SIZE;) {
        Block *b = (Block *)(pc_heap + off);
        if (!b->used && b->size >= need) {
            if (b->size - need >= 2 * sizeof(Block)) {
                Block *rest = (Block *)(pc_heap + off + need);
                rest->size = b->size - need;
                rest->used = 0;
                b->size = need;
            }
            b->used = 1;
            return b + 1;
        }
        off += b->size;
    }
    log_error("memory: the game asked for %u bytes and the heap is full", size);
    return NULL;
}

void pc_game_free(void *p) {
    if (!p) return;
    Block *b = (Block *)p - 1;
    b->used = 0;
    /* merge with following free blocks */
    for (;;) {
        uint8_t *next = (uint8_t *)b + b->size;
        if (next >= pc_heap + HEAP_SIZE || ((Block *)next)->used) break;
        b->size += ((Block *)next)->size;
    }
}

/* Highest address the game can be handed. */
uintptr_t pc_game_memory_end(void) {
    uintptr_t a = (uintptr_t)(pc_heap + HEAP_SIZE), b = (uintptr_t)(pc_ram + sizeof pc_ram);
    return a > b ? a : b;
}
