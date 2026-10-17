# Design

## One source, two builds

The decompiled C in `decomp/src/` compiles two ways:
- in Outbreak-decomp, with Capcom's original compiler, into a byte-identical PS2 executable;
- here, with a normal compiler, into native Windows code.

Every function decompiled for the decomp becomes PC code too. Everything the PS2 build gets from Sony's
libraries or hardware gets a replacement in `src/`.

## 32-bit

PS2 pointers are 4 bytes. A 32-bit build keeps every game structure the same size and layout:
- data from game files can be used as loaded;
- structs sent over the network stay byte-identical, which playing with PS2/PCSX2 players on Obsrv needs.

32-bit programs run on all 64-bit Windows versions.

## The game's data: converted at build time

The executable's initialised data (strings, tables, pointers) is not decompiled by hand and never
committed. At build time `tools/gen_data.py` reads **your** executable and overlays (from `decomp/orig/`) and
the symbol names splat found (from `decomp/asm/`). It writes the data as GNU assembly:
- **bytes** stay bytes;
- **pointers become relocations** (`.long D_00224268`), so the linker gives every symbol a native address.

Nothing needs to sit at its PS2 address. (An earlier version mapped PS2 memory at the original addresses;
Windows already uses that range for its own stack and heaps, so that couldn't work.)

A word counts as a pointer when its value is the address of a symbol splat found. Exceptions go in
`config/<game>.json`:
- `raw` ranges are never pointers (VU microcode, kernel-mode code);
- `pointer`/`number` override single addresses.

Numbers that look like addresses but weren't converted are listed in `build/gen/gen_report.txt` for review.

## Overlays

The game loads code overlays (`PROG/*.BIN`) into shared address ranges. Here every overlay is linked into
the program, with its own data. Overlays reuse addresses, so their symbols get a prefix: `game__D_00540100`,
via a generated `names_<overlay>.h` that the overlay's C is compiled with.

When the game "loads" an overlay it still reads the file (into `pc_ram`, unused), then calls the Metrowerks
runtime's `MWo3_Init`; the PC version of that (`src/overlay.c`) puts that overlay's data back to how it was
linked and clears its bss, using the table `tools/gen_data.py` writes (`build/gen/overlays.c`).

Main's code refers to overlay functions by address (`D_540080` is DEMO.BIN's entry task). When several
overlays cover an address, `config/<game>.json` `"owners"` says which one the code means, and the linker
gets an alias (`build/gen/aliases.rsp`).

## Functions that aren't decompiled yet

Every function the C or the data refers to that nobody has decompiled or replaced gets a generated stub.
Calling one stops the program with "the game called func_XXXXXXXX, which isn't decompiled yet", which shows
what to decompile next.

## PS2 library replacements (`src/`)

| PS2 side | PC replacement | Status |
|---|---|---|
| C library (`strcpy`, `sprintf`, …) | the C runtime; `strupr` in `libc_extra.c` | as needed |
| file I/O `sceOpen/Read/Lseek/Close` | `sce_io.c`: `cdrom0:`/`pfs0:` → disc image, `host:` → local folder | done |
| disc image | `iso9660.c` (ISO 9660, files over 4 GB offsets) | done |
| CD/DVD streaming, CRI file system | disc image reads | to do |
| graphics: libgraph double buffering, image uploads, display | `sce_graph.c` + `gs.c` (GS video memory with the real pixel layout, GIF packets, sprites) + `display.c` (SDL2 window, 59.94 Hz) | boot screen works |
| DMA controller, VIF1, GIF paths | `dma.c`: a send runs the whole DMA chain at once (tags, VIF1 DIRECT, GIF packets into the GS model) and then raises the channel's DMA-end interrupt | 2D packets work; VU1 microprograms skipped |
| graphics: textured/3D drawing, VU1 | GS model first, native renderers later | to do |
| IOP: reboot, modules, SIF RPC, HDD | `sce_sif.c`: reboot and module loads succeed, the HDD drivers report "no device" | done for boot |
| EE kernel: interrupt/DMA handlers, semaphores | `kernel.c`: handlers are called where the hardware would raise them (v-blank in `sceGsSyncV`, DMA end after a send) | threads to do |
| hardware registers (timers, GS privileged registers) | `hw.c` (`HW_REG32`/`HW_REG64` in the decomp) | timers, display registers |
| hand-written assembly in the game (copy loops) | `asm.c` | as needed |
| pad, memory card | SDL input; save files | to do |
| sound (IOP driver, SPU2, CRI ADX) | software mixer + ADX decoder | to do |
| movies (CRI Sofdec) | FFmpeg | to do |
| network (DEV9/INET stack, DNAS, KDDI pages) | Winsock + legacy-TLS HTTPS to Obsrv | to do |

## Addresses stay below 0x10000000

The game keeps a type in the top 4 bits of addresses it queues for DMA (and the PS2 itself uses those bits for
uncached and scratchpad access). So every address the game handles on PC must fit in 28 bits:
- the program is linked at a fixed low base (0x400000 on Windows, ASLR off; non-PIE on Linux);
- the game's `malloc` is a heap inside the program image (`src/ps2ram.c`), not the C runtime's;
- `main.c` refuses to start if any of that ends up higher.

## Fixed RAM addresses

Some game code uses fixed main-RAM addresses as buffers (the boot code expands its image at `0x01000000`). The
decomp writes these as `PS2_RAM(0x01000000)` (`decomp/include/ps2mem.h`); on PC that points into `pc_ram`, a
32 MiB block (`src/ps2ram.c`).

## Floating point

The PS2 FPU isn't IEEE: no infinities, NaNs or denormals, and it rounds differently. If online play shows
desyncs, the gameplay code involved will use PS2-accurate float helpers on PC.

## C and C++

The game code is C and stays C: the same files must compile to the original with the PS2 compiler.
The platform layer is C, with C++17 (no exceptions or RTTI, C interfaces) where it pays off:
- `src/texcache.cpp`: decoded textures. The software GS used to decode every texel it sampled
  (swizzled addressing, CLUT lookup, 16-bit expansion); textures used repeatedly are now decoded once
  and reused until their GS memory pages are written (every GS write bumps its 8 KiB page's count).
  Textures rewritten about as often as they're drawn (movie frames) aren't decoded at all. Output is
  identical (`OUTBREAK_NO_TEXCACHE=1` turns it off for comparison); the native renderer will upload
  from the same cache.
- next: the native 3D renderer (VU1 packets to the GPU) and the Obsrv network layer.

The 32-bit Linux build compiles with `-fno-pie` (it is linked `-no-pie`): position-independent i386
code spends a register and a call per function on finding its globals, which cost the software GS
about 8%.
