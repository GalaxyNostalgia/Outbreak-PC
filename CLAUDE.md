# Working on Outbreak-PC (notes for Claude sessions)

A native PC port (not a recompiler) of Biohazard Outbreak File 1 (SLPM-65428), built from the
matching decompilation in Outbreak-decomp (submodule `decomp/`). Goals, in order: File 1 running
on Windows, then rendering/audio, multiplayer on the Obsrv fan servers, the English patch, File 2.

## Rules

- No game code, data or disassembly in either repo (a pre-commit firewall checks). The user's own
  disc data stays on their machine and in the session's workspace only. Test builds made with
  converted disc data are for the user alone.
- Third-party code must be permissively licensed; both repos are CC0.
- Commits: author `Claude <noreply@anthropic.com>` (`git -c user.name=Claude -c
  user.email=noreply@anthropic.com commit ...`), ending with the session's Co-Authored-By /
  Claude-Session trailers. Never use the user's e-mail as author.
- Before committing decomp work: `python3 -I tools/build.py` must report every target OK. Then
  bump this repo's submodule (`cd decomp && git fetch && git checkout origin/main`).
- Language: the game code is C (it has to compile to the original with the PS2 compiler). The
  PC-only platform layer is C plus C++17 where it pays off (no exceptions/RTTI; `extern "C"`
  interfaces): `src/texcache.cpp` now; the native 3D renderer and the network layer are meant to
  be C++. Measure before and after (`OUTBREAK_NO_TEXCACHE=1` style switches; compare dumped
  frames for identical output).

## Starting a fresh session

1. Clone both repos side by side in `/home/claude` (`outbreak-pc` with `--recursive`, and
   `outbreak-decomp`), plus `https://github.com/matt-kempster/m2c` next to them.
2. Packages: `apt install binutils-mips-linux-gnu gcc-multilib g++-multilib ninja-build cmake gdb`;
   `pip install --break-system-packages -r outbreak-decomp/requirements.txt`.
3. Bring back what git can't hold: the user's machine (linked through the desktop app) keeps a
   resume pack, `resume.tar.gz.partNN` in `_decomp_input/resume/` of their connected game folder
   (memory has the exact place). Stage the parts and run
   `sh outbreak-pc/tools/dev/resume_unpack.sh <staged dir>`: it restores `outbreak-decomp/orig`,
   `outbreak-decomp/tools/compilers` (decomp.me isn't reachable from the workspace) and
   `~/outbreak-work` (the sparse disc `f1disc`, its `f1disc.have.json`, saves).
4. `cd outbreak-decomp && python configure.py f1 f2` (splits the binaries into asm; needs orig/),
   then check `python3 -I tools/build.py`.
5. PC build: `cmake -S outbreak-pc -B outbreak-pc/build/linux32 -G Ninja -DOUTBREAK_GAME=f1
   -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_ASM_FLAGS=-m32
   -DOUTBREAK_DECOMP_DIR=/home/claude/outbreak-decomp`.

Before a session ends (or when the user asks), refresh the pack: `sh tools/dev/resume_pack.sh`,
then write `~/outbreak-work/pack/*` to the same `_decomp_input/resume/` folder on the user's
machine (same names, so they replace the old ones).

## The loop

```
cd outbreak-pc
INPUT=START@1300,CIRCLE@1700,CIRCLE@1900,CIRCLE@2100,CIRCLE@2300,CIRCLE@2500,CIRCLE@2700,START@2900,START@3000,START@3100 \
  T=150 N=6 sh tools/dev/next.sh
```

builds, runs the game headless with those pad presses (frame@button) and prints the first function
the game called that isn't decompiled, with an m2c draft (`decomp/tools/draft.py`). Decompile it
in the decomp repo (`tools/try.py f1.<ovl> file.c` compares with the original; helpers in
`outbreak-decomp/tools/dev/`), and repeat. If it prints `NEED DISC DATA`, the run read disc parts
the sparse copy lacks: on the user's machine run `tools/dev/fetch_ranges.py <iso> req.txt blobN.gz`
(put the request lines in req.txt; generous sizes save round trips), stage `blobN.gz` and
`blobN.gz.idx`, and `sh tools/dev/apply.sh <staged dir> blobN`. A crash right after new data
usually means more data is needed, not a bug.

Matching is preferred but progress matters more: give a function one or two real attempts, then
wrap it with `tools/dev/nonmatch.py file.c func "why"` (the PC build compiles NON_MATCHING code).

## MWCC (mwccps2) habits that matter

- Leaf functions defined earlier in the file let callers keep values in `a`/`t` registers across
  the call; other callees force `s` registers. Seen not to happen for a non-static callee in some
  cases (game/roomsets.c func_0067B020): if the original's callers keep values across a call to a
  leaf whose callers are all nearby, it was probably static there; it can only be made static once
  every caller is in C (asm callers need the global symbol).
- `field + (u32)ptr` vs `ptr + field`: which is loaded first follows the field's signedness
  (an `s32` offset field gave the original's order in anim.c and keyframe.c).
- Evaluation order of call arguments follows the prototype's parameter order, so it can tell a
  float parameter's position (keyframe.c: `f32 frame` first).
- Globals of 8 bytes or less are `$gp`-relative when declared with a size; declare them unsized
  (`extern T x[];`) or as a bigger struct to get `lui`/absolute addressing. Struct fields give a
  fresh `lui` per access; a local pointer gets CSE'd.
- Declaration order picks registers and stack slots (`tools/dev/declperm.py` tries the orders).
- Callees mask their own `u8`/`u16` parameters; callers don't mask a value already of that type,
  but do when narrowing — so the prototype a caller sees changes its code.
- Message buffers on the stack often have padding the original declared (frame size tells).
- A void-looking function whose early `return` jumps past `ld ra` may really return a value from
  that point and fall off the end elsewhere.
- Read the raw asm (with nops) before trusting an m2c draft; m2c drops `t0`-`t3` arguments.
- Branch layout tells the control flow: a test whose then-block sits *after* the else-block is a
  one-case `switch` (`switch (x) { case 3: ...; break; default: ... }`); `case` compares come
  out in reverse of the listing; `default:` listed first puts its code right after the compares;
  an empty `case 0: break;` before a second switch gives an explicit `beqz` to it.
- A `struct` field access beats `FIELD(p, T, off)`: the macro's address arithmetic gets CSE'd
  into a register (`addiu a1,s0,off`). Add the field to the header instead.
- A function reached only through a table of function pointers, placed right after another
  one's tail call, is merged into it by splat: add `func_X = 0xX; // type:func` to
  `config/<ELF>/<ovl>.symbol_addrs.txt`, move `asm/<ELF>/<ovl>` (and `en/<ovl>`) away and rerun
  `configure.py` (needs `pip install -r requirements.txt`).

- A local whose address is passed on: a `u16` sits at the end of its 4-byte stack slot (sp+2);
  the original's at the slot start was an `s32` read back as `u16` (collision.c / charmove.c
  func_0062DAE0). `Vec3` (struct) locals copy with `ld`/`sd`; `f32[3]` ones word by word.
- A list walk whose every path ends in `b` to the loop test with `lw next` in its delay slot:
  write `z = z->next;` at the end of each branch, an `else { z = z->next; }` included (scriptops.c
  func_005518B0, func_0065E880, func_00660A70). Seen as "nop in the delay slot" otherwise.
- A compare done into `$at` (`slti at,...; beqz at`): the original's `if` assigns in *both*
  branches, even when the then-assignment is redundant (`if (x < 0x1000) { r = -1; } else
  { r = 2; }` with r already -1: single/partner.c func_0082B870).
- `b` to a shared `return 0` with a `nop` in its delay slot: the original returns 0 once at the end
  and its branches `break` out of the switch to it (game/charmove.c func_006337C0); a one-case test
  that branches over a `b` is a one-case `switch`.
- Unsolved, seen often: the original leaves a `nop` in a branch's delay slot where ours fills it; and `if (x) return 1; return 0;` that ours folds into `sltu`.
  Don't spend long on these: NON_MATCHING.
- An explicit `& 0xFF` / `& 0xFFFF` keeps a mask that a cast folds away: `(s16)f & 0xFFFF` gave
  the original's sign-extend-then-mask (charmove.c ANG), `(a & 0xFF) != (b & 0xFF)` its extra
  `andi` on a loaded byte (scene.c func_00680AB0).
- A pointer added to an offset field: `(u8 *)(f->ofs + (s32)ptr)` can be the only order that
  matches (mapdata.c func_006A7BE0).
- A call whose argument count looks short: check `t0`-`t3` in the caller (func_006A6140 takes a
  fifth argument its caller sets in a delay slot).
- A PC crash reading game tables: check the words gen_data relocated. Tables of `u16` pairs can
  look like pointers (0x0071xxxx); add them to `number` in `config/f1.json`.

## Where things are

- `config/f1.json`: how gen_data treats the game's data — `owners` (which overlay an address
  means), `pointer`/`number` overrides, `sdata` (each overlay's part of the main .sdata).
- `src/`: the platform layer (disc/ISO reader, Sony I/O, GS model, display, audio, movie stubs).
- Decomp: `src/SLPM_654.28/<overlay>/*.c`, headers in `include/`, docs in `docs/`.
