# Biohazard Outbreak PC

A native Windows version of **Biohazard Outbreak** (and later **File 2**), built from the matching
decompilation in [Outbreak-decomp](https://github.com/GalaxyNostalgia/Outbreak-decomp). The goal: the full game
on PC, playable online on the [Obsrv](https://www.obsrv.org/) servers with PS2 and PCSX2 players, with the
English fan translation.

> [!IMPORTANT]
> No game code or data is in this repository. You build it from your own disc: the game's data is converted
> from your copy on your machine during the build.

## Status

**Early.** The program opens your disc and runs the game's own decompiled code natively, drawing through a
software model of the PS2's Graphics Synthesizer (textures, CLUTs, blending, the GS memory layouts) in a
window. The boot runs through: memory card check (slot 1 is a folder of save files, `--saves DIR`), the
logos, the online-play setup check, then DEMO.BIN: the title screen, the opening movie and the attract
movies, which play through the game's own Sofdec upload code (MPEG-2 video decoded with a patched
pl_mpeg). When it reaches code that isn't decompiled yet it stops and says which function:

```
[info] starting the game: main(1, {"cdrom0:\SLPM_654.28;1"})
[info] sdr: Connected to TamsoftSoundDriver: Version 2.0
[info] adx: partition 10: \NETBIO00.DAT;1, 13 files
...
[info] sofdec: playing file 1 of partition 3 (512x448, 29.97 fps)
```

Each function decompiled in Outbreak-decomp gets the game further. Sound (ADX, the Tamsoft driver), the 3D
scenes, the menus behind the title, online play and File 2 are still to do.

The disc can be an ISO image or a folder with the disc's files copied into it (like PCSX2's host file
system, or the QuickLoading set-up).

## Building on Windows

1. Install [MSYS2](https://www.msys2.org/) and [Python 3](https://www.python.org/). In the **MSYS2 MINGW32**
   shell:
   ```sh
   pacman -S git mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-ninja mingw-w64-i686-SDL2
   git clone --recursive https://github.com/GalaxyNostalgia/Outbreak-PC.git
   cd Outbreak-PC
   ```
2. Prepare the decomp with **your own** Japanese disc image (Biohazard Outbreak, SLPM-65428):
   ```sh
   cd decomp
   python -m pip install -r requirements.txt
   mkdir -p orig/SLPM_654.28 && cp "/c/path/to/your/Biohazard Outbreak.iso" orig/SLPM_654.28/
   python configure.py f1
   cd ..
   ```
3. Build and run:
   ```sh
   cmake -S . -B build -G Ninja -DOUTBREAK_GAME=f1
   cmake --build build
   build/outbreak-f1.exe --test "/c/path/to/your/Biohazard Outbreak.iso"   # checks the decompiled code
   build/outbreak-f1.exe "/c/path/to/your/Biohazard Outbreak.iso"
   ```

From Linux you can cross-compile the same `.exe`: `sudo apt install gcc-mingw-w64-i686 g++-mingw-w64-i686`, unpack SDL2's
`SDL2-devel-2.30.9-mingw.tar.gz`, then
`cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake -DSDL2_DIR=<SDL2>/i686-w64-mingw32/lib/cmake/SDL2`.
SDL2 is linked statically, so the result is a single `.exe`. Without SDL2 the build is headless:
`--dump-frames DIR` writes each new picture to a file. A 32-bit Linux build also works
(`-DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_ASM_FLAGS=-m32`, with `gcc-multilib g++-multilib`), but Windows is the main platform.

## How it works

See [docs/DESIGN.md](docs/DESIGN.md). In short: it's a **32-bit** program, so game structures and network
packets keep the PS2's layout. The game's data is converted at build time into assembly with native pointers
(`tools/gen_data.py`), and the PS2 libraries the game calls get PC replacements (`src/`).

## Layout

| | |
|---|---|
| `decomp/` | Outbreak-decomp (submodule): the decompiled game C and the tools that split your disc |
| `src/` | the PC platform layer: disc image reader, Sony file I/O, graphics (`gs.c` GS model, `sce_graph.c` Sony graphics library, `display.c` window) |
| `tools/gen_data.py` | converts your executable's data into linkable assembly; writes stubs for functions not decompiled yet |
| `third_party/pl_mpeg` | MPEG video decoder (MIT), extended for the MPEG-2 video in the game's Sofdec movies |
| `config/f1.json` | how `gen_data.py` treats File 1's data (VU microcode ranges, pointer overrides) |
| `cmake/mingw-i686.cmake` | toolchain file for cross-compiling the Windows build |

## Credits

The decomp and its contributors; ghostline and the Obsrv team for the online revival; REfan988 & Silent Hell
for the English translation. Not affiliated with Capcom or Sony.
