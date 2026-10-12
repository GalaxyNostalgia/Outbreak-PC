/* The Metrowerks overlay runtime, PC side.
 *
 * On a PS2 the game reads an overlay file into memory at the address it was linked for and calls
 * MWo3_Init, which clears its bss and runs its static initialisers. Here every overlay is already
 * in the program with its own symbols, so MWo3_Init puts its data back to how it was linked and
 * clears its bss: the same state as a fresh load. The file the game read stays in pc_ram, unused.
 */
#include "overlay.h"
#include "log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void pc_overlays_init(void) {
    for (PcOverlay *o = pc_overlays; o->name; o++) {
        size_t n = (size_t)(o->data_end - o->data);
        o->initial = malloc(n ? n : 1);
        if (o->initial) memcpy(o->initial, o->data, n);
    }
}

void MWo3_Init(void *image, int32_t size) {
    const uint32_t *h = image;
    uint32_t id = h[1];

    for (PcOverlay *o = pc_overlays; o->name; o++) {
        if (o->id != id) continue;
        if (o->initial) memcpy(o->data, o->initial, (size_t)(o->data_end - o->data));
        memset(o->bss, 0, (size_t)(o->bss_end - o->bss));
        log_info("overlay: %s started", o->name);
        return;
    }
    log_error("overlay: id %u (%.16s) isn't linked into this build; add it to config/<game>.json \"overlays\"",
              id, (const char *)image + 0x20);
}
