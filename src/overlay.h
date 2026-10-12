#ifndef OVERLAY_H
#define OVERLAY_H

/* The game's code overlays (the .BIN files in PROG) on PC: all of them are linked into the program
   (tools/gen_data.py lists them in build/gen/overlays.c), so "loading" one only resets its data. */
typedef struct {
    const char *name;
    unsigned id;                    /* the MWo3 header's overlay id */
    unsigned load;                  /* PS2 address it runs at */
    char *data, *data_end;          /* its initialised data in this program */
    char *bss, *bss_end;
    char *initial;                  /* copy of the data as linked, made at start-up */
} PcOverlay;

extern PcOverlay pc_overlays[];     /* ends with an entry whose name is NULL */

void pc_overlays_init(void);

#endif
