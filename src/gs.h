/* A small model of the PS2's Graphics Synthesizer: 4 MiB of video memory with the GS's
 * own pixel layout, the drawing registers, and the GIF packets the game sends.
 *
 * This is the bring-up renderer: enough to run the game's boot and menus and see what it
 * draws. Native renderers for the 3D scenes come later (docs/DESIGN.md).
 */
#ifndef GS_H
#define GS_H

#include <stdint.h>

void gs_reset(void);
/* Process a GIF packet (GIFtag + data) the game built in memory. Returns the bytes consumed. */
uint32_t gs_gif_packet(const void *packet);
/* GIF data as it comes from DMA (PATH2/PATH3): quadwords, packets may be split anywhere. */
void gs_gif_write(const void *data, uint32_t qwords);
/* Image data following a TRXDIR=0 transfer (GIF IMAGE mode). */
void gs_image_data(const void *src, uint32_t bytes);
/* Privileged display registers (what sceGsPutDispEnv writes). */
void gs_set_display(uint64_t pmode, uint64_t dispfb, uint64_t display);
/* Copy the currently displayed picture as 0xAARRGGBB; returns 0 if nothing is displayed. */
int gs_read_display(uint32_t *out, int *w, int *h, int max_w, int max_h);

#endif
