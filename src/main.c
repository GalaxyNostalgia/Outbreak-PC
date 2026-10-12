/* Biohazard Outbreak PC: entry point.
 *
 * Opens the user's disc image and calls the game's own main(), decompiled in the
 * decomp repo (decomp/src/<ELF>/main/main.c). The game's data comes from the user's
 * executable, converted at build time (tools/gen_data.py). The program stops at the
 * first function that isn't decompiled yet and names it.
 */
#include "display.h"
#include "iso9660.h"
#include "log.h"
#include "overlay.h"
#include "stubs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GAME_ELF
#error "build with -DGAME_ELF=\"SLPM_654.28\" (CMake sets it from OUTBREAK_GAME)"
#endif

/* decompiled game code */
int func_001A8300(const char *name);           /* disc file size */
void func_001BC780(char *dst, const char *name); /* "cdrom0:\NAME;1" */
int func_001A87F0(int argc, char **argv);      /* the game's main() */
void pc_mc2_set_dir(const char *dir);             /* mc2.c: where memory card saves go */

void pc_unimplemented(const char *name) {
    log_error("the game called %s, which isn't decompiled yet", name);
    fflush(stderr);
    display_hold(name);        /* keep the last picture on screen until the window is closed */
    exit(3);
}

static int self_test(void) {
    static const char *files[] = {"PROG\\SUBMAIN.BIN", "PROG\\GAME.BIN", "PROG\\NETWK.BIN", "SYSTEM.CNF",
                                  "PROG\\EDITOR.BIN"};
    int fails = 0;
    char path[0x100];
    log_info("self-test: decompiled disc functions, running natively");
    func_001BC780(path, "prog\\game.bin");
    log_info("  func_001BC780(\"prog\\\\game.bin\") -> \"%s\"", path);
    if (strcmp(path, "cdrom0:\\PROG\\GAME.BIN;1") != 0) fails++;
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        IsoFile f;
        int want = iso_find(files[i], &f) == 0 ? (int)f.size : 0;
        int got = func_001A8300(files[i]);
        log_info("  func_001A8300(\"%s\") -> %d (disc: %d) %s", files[i], got, want, got == want ? "ok" : "MISMATCH");
        fails += got != want;
    }
    return fails;
}

int main(int argc, char **argv) {
    const char *iso = NULL, *dump = NULL;
    int test = 0, headless = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--test")) test = 1;
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--dump-frames") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "--saves") && i + 1 < argc) pc_mc2_set_dir(argv[++i]);
        else if (argv[i][0] != '-') iso = argv[i];
    }
    if (!iso) {
        fprintf(stderr, "usage: %s [--test] [--headless] [--dump-frames DIR] [--saves DIR] <your %s disc image.iso>\n",
                argv[0], GAME_ELF);
        return 2;
    }
    display_init(headless || test, dump);
    if (sizeof(void *) != 4) {
        log_error("this must be a 32-bit build");
        return 1;
    }
    extern uintptr_t pc_game_memory_end(void);
    extern char D_00231A70[];
    if (pc_game_memory_end() > 0x10000000 || (uintptr_t)D_00231A70 > 0x10000000) {
        log_error("the program was loaded above 0x10000000; it must be linked at a low fixed address");
        return 1;
    }
    if (iso_open(iso) != 0) return 1;
    IsoFile elf;
    if (iso_find(GAME_ELF, &elf) != 0) {
        log_error("%s isn't on this disc: is it the right game?", GAME_ELF);
        return 1;
    }
    if (test) {
        int fails = self_test();
        log_info("self-test %s", fails ? "FAILED" : "passed");
        return fails ? 1 : 0;
    }
    static char boot[] = "cdrom0:\\" GAME_ELF ";1";
    char *game_argv[] = {boot, NULL};
    pc_overlays_init();
    log_info("starting the game: main(1, {\"%s\"})", boot);
    func_001A87F0(1, game_argv);
    return 0;
}
