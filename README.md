# Biohazard Outbreak PC

A native Windows version of **Biohazard Outbreak** (and later **File 2**), built from the matching
decompilation in [Outbreak-decomp](https://github.com/GalaxyNostalgia/Outbreak-decomp). The goal: the full game
on PC, playable online on the [Obsrv](https://www.obsrv.org/) servers with PS2 and PCSX2 players, with the
English fan translation.

> [!IMPORTANT]
> No game code or data is in this repository. You build it from your own disc: the game's data is converted
> from your copy on your machine during the build.

## Status

**Early.** The program opens your disc image and runs the game's own decompiled `main()` natively, then stops
at the first function that isn't decompiled yet:

```
[info] starting the game: main(1, {"cdrom0:\SLPM_654.28;1"})
[error] the game called func_001A85C0, which isn't decompiled yet
```

Each function decompiled in Outbreak-decomp moves that line further. Next up: the system init
(`func_001A85C0`), which brings in graphics, input and the I/O processor replacements.

## Building on Windows

1. Install [MSYS2](https://www.msys2.org/) and [Python 3](https://www.python.org/). In the **MSYS2 MINGW32**
   shell:
   ```sh
   pacman -S git mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-ninja
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

From Linux you can cross-compile the same `.exe`: `sudo apt install gcc-mingw-w64-i686`, then
`cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake`. A 32-bit Linux build also works
(`-DCMAKE_C_FLAGS=-m32 -DCMAKE_ASM_FLAGS=-m32`), but Windows is the main platform.

## How it works

See [docs/DESIGN.md](docs/DESIGN.md). In short: it's a **32-bit** program, so game structures and network
packets keep the PS2's layout. The game's data is converted at build time into assembly with native pointers
(`tools/gen_data.py`), and the PS2 libraries the game calls get PC replacements (`src/`).

## Layout

| | |
|---|---|
| `decomp/` | Outbreak-decomp (submodule): the decompiled game C and the tools that split your disc |
| `src/` | the PC platform layer: disc image reader, Sony file I/O, C library extras, more to come |
| `tools/gen_data.py` | converts your executable's data into linkable assembly; writes stubs for functions not decompiled yet |
| `config/f1.json` | how `gen_data.py` treats File 1's data (VU microcode ranges, pointer overrides) |
| `cmake/mingw-i686.cmake` | toolchain file for cross-compiling the Windows build |

## Credits

The decomp and its contributors; ghostline and the Obsrv team for the online revival; REfan988 & Silent Hell
for the English translation. Not affiliated with Capcom or Sony.
