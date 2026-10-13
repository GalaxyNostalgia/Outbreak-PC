#include "display.h"
#include "gs.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_SDL2
#define SDL_MAIN_HANDLED
#include <SDL.h>
static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;
static uint64_t g_next_tick;
#endif

#define MAX_W 1024
#define MAX_H 1024
static uint32_t g_pixels[MAX_W * MAX_H];
static int g_headless = 1, g_frame, g_dumped;
static const char *g_dump_dir;
static uint32_t g_last_hash;

void display_init(int headless, const char *dump_dir) {
#ifdef HAVE_SDL2
    g_headless = headless;
#else
    (void)headless;
    g_headless = 1;
#endif
    g_dump_dir = dump_dir;
}

void display_open(void) {
#ifdef HAVE_SDL2
    if (g_headless || g_win) return;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        log_warn("display: SDL_Init failed (%s); running without a window", SDL_GetError());
        g_headless = 1;
        return;
    }
    g_win = SDL_CreateWindow("Biohazard Outbreak", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 896,
                             SDL_WINDOW_RESIZABLE);
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, 0);
    SDL_RenderSetLogicalSize(g_ren, 640, 448);
    g_next_tick = SDL_GetPerformanceCounter();
    log_info("display: window open");
#endif
}

static uint32_t hash_pixels(int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ g_pixels[i]) * 16777619u;
    return h;
}

static void dump_frame(int w, int h, const char *tag) {
    if (!g_dump_dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s/frame_%05d%s.ppm", g_dump_dir, g_frame, tag);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t rgb[3] = {(uint8_t)(g_pixels[i] >> 16), (uint8_t)(g_pixels[i] >> 8), (uint8_t)g_pixels[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    g_dumped++;
    log_info("display: wrote %s", path);
}

static int grab(int *w, int *h) {
    return gs_read_display(g_pixels, w, h, MAX_W, MAX_H);
}

#ifdef HAVE_SDL2
static void present(int w, int h) {
    if (!g_tex || w != 640 || h != 448) {
        if (g_tex) SDL_DestroyTexture(g_tex);
        g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_RenderSetLogicalSize(g_ren, w, h);
    }
    SDL_UpdateTexture(g_tex, NULL, g_pixels, w * 4);
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
}

void pc_pad_sdl_event(const void *ev);

static void pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        pc_pad_sdl_event(&e);
        if (e.type == SDL_QUIT) {
            log_info("display: window closed");
            exit(0);
        }
    }
}
#endif

void display_vsync(void) {
    int w, h;
    g_frame++;
    if (!grab(&w, &h)) return;
    uint32_t hs = hash_pixels(w * h);
    if (hs != g_last_hash) {
        g_last_hash = hs;
        if (g_headless) dump_frame(w, h, "");
    }
#ifdef HAVE_SDL2
    if (!g_headless && g_win) {
        present(w, h);
        pump();
        uint64_t freq = SDL_GetPerformanceFrequency(), now = SDL_GetPerformanceCounter();
        g_next_tick += (uint64_t)((double)freq * 1001.0 / 60000.0);    /* NTSC field rate */
        if (g_next_tick > now) SDL_Delay((uint32_t)((g_next_tick - now) * 1000 / freq));
        else g_next_tick = now;
    }
#endif
}

void display_hold(const char *why) {
    int w, h;
    if (!grab(&w, &h)) return;
    if (g_headless) {
        dump_frame(w, h, "_final");
        return;
    }
#ifdef HAVE_SDL2
    char title[256];
    snprintf(title, sizeof title, "Biohazard Outbreak - stopped: %s (close to exit)", why);
    SDL_SetWindowTitle(g_win, title);
    for (;;) {
        present(w, h);
        pump();
        SDL_Delay(50);
    }
#endif
}
