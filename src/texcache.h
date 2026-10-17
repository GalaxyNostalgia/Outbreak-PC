/* Decoded-texture cache: GS textures (swizzled, palettized, 16-bit...) decoded once to RGBA8888 and
 * reused until the GS memory under them changes. The software GS samples these instead of decoding
 * every texel; the native renderer will upload them. C++ inside (texcache.cpp), C interface here.
 */
#ifndef TEXCACHE_H
#define TEXCACHE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Everything a decoded texel depends on. */
typedef struct {
    uint32_t tbp, tbw, psm, tw, th;     /* the texture (TEX0) */
    uint32_t cbp, cpsm, csa;            /* its CLUT, CSM1 (CSM2 textures aren't cached) */
    uint64_t texa;                      /* alpha expansion of 16/24-bit colours */
} TexKey;

/* Counts writes per 8 KiB page of GS memory (512 pages); the GS bumps it on every write. */
extern uint32_t g_tc_page_gen[512];

/* Decodes the whole texture into out (tw * th texels, row-major) for tc_get. */
typedef void (*TexDecodeFn)(void *ctx, uint32_t *out);

/* The decoded texture for key: from the cache when the pages it was decoded from (texture and
 * CLUT, conservatively) haven't been written since, else decoded now. NULL when the texture is too
 * big to cache (the caller samples the GS directly). Valid until the next tc_get or tc_reset. */
const uint32_t *tc_get(const TexKey *key, TexDecodeFn decode, void *ctx);

void tc_reset(void);

#ifdef __cplusplus
}
#endif

#endif
