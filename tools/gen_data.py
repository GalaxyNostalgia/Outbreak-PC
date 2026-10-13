#!/usr/bin/env python3
"""Turn the game's own data into assembly the PC compiler can link.

    python -I tools/gen_data.py --decomp decomp --game f1 --out build/gen [--underscore]

Reads the user's executable and overlays (decomp/orig/<ELF>/, put there by the decomp's
configure.py) and the symbol names splat found (decomp/asm/<ELF>/), and writes:

  data_<target>.s   the target's .data/.rodata/.bss as i686 GNU assembly. Words that point at
                    a known symbol become `.long symbol+offset`, so the linker gives them native
                    addresses: nothing has to live at its PS2 address.
  names_<target>.h  for overlays: #defines that give the overlay's own symbols a prefix, because
                    overlays share PS2 address ranges (game__D_00540100 vs demo__D_00540100)
  stubs.c           a stub for every function the C or the data refers to that isn't decompiled
                    or implemented by the PC layer; calling one stops with its name
  aliases.rsp       linker options: names the C uses for another overlay's functions (D_540080 in main's
                    C is DEMO.BIN's func_00540080, config/<game>.json "owners")
  overlays.c        the overlays linked in, with where each one's data and bss are, so the PC
                    can reset them when the game "loads" that overlay
  gen_report.txt    what was converted, and which words looked like pointers but weren't used

Pointer rules (config/<game>.json can override per address):
  - inside a "raw" range (VU microcode, kernel patch code): never a pointer
  - the word equals the address of a symbol splat found (D_, func_, named): pointer
  - anything else stays a number; in-range numbers are listed in the report for review
"""
import argparse
import json
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LABEL = re.compile(r"^\s*(glabel|dlabel|jlabel|alabel) (\w+)")
RESERVED = {"_start", "main", "_init", "_fini", "exit", "abort"}
# the game's own heap functions: the PC versions (src/ps2ram.c; sce.h maps the C names the same way)
PC_NAMES = {"malloc": "pc_game_malloc", "free": "pc_game_free"}
# C library functions the game's data points at: the PC's C runtime has them
LIBC = {"memcpy", "memset", "memmove", "memcmp", "strcpy", "strncpy", "strcmp", "strncmp", "strlen", "strcat",
        "strchr", "strrchr", "strstr", "sprintf", "printf", "vsprintf", "atoi", "rand", "srand", "qsort"}
ADDR = re.compile(r"/\* (?:[0-9A-F]+ )?([0-9A-F]{8})(?: [0-9A-F]{8})? \*/")
CDEF = re.compile(r"^[A-Za-z_][\w \t\*]*?\b(\w+)\s*\([^;{}]*\)\s*\{", re.M)


def parse_labels(asm_dir):
    """{address: [(kind, name)]} for every label splat emitted in a target's asm."""
    out = defaultdict(list)
    for f in sorted(asm_dir.rglob("*.s")):
        pending = []
        for line in f.read_text(errors="replace").splitlines():
            m = LABEL.match(line)
            if m:
                pending.append((m.group(1), m.group(2)))
                continue
            if pending:
                a = ADDR.search(line)
                if a:
                    addr = int(a.group(1), 16)
                    for p in pending:
                        out[addr].append(p)
                    pending = []
    return out


class Target:
    def __init__(self, name, prefix, image, data_ranges, bss, text, labels):
        self.name, self.prefix = name, prefix
        self.image = image                # (bytes, vram of byte 0)
        self.data_ranges = data_ranges    # [(vram_start, vram_end)] backed by the file
        self.bss = bss                    # (vram_start, vram_end)
        self.text = text                  # (vram_start, vram_end)
        self.labels = labels

    def owns(self, addr):
        lo = min(self.text[0], self.data_ranges[0][0])
        return lo <= addr < self.bss[1]

    def sym(self, name):
        if not self.prefix and name in PC_NAMES:
            return PC_NAMES[name]
        if name in RESERVED:                 # names the PC C runtime owns
            return f"ps2_{name}"
        return f"{self.prefix}{name}" if self.prefix else name


def load_targets(decomp, game, ver, wanted):
    elf = ver["elf"]
    orig = decomp / "orig" / elf
    out = []
    # main executable: a single PT_LOAD at 0x100000; config/<ELF>/main.yaml says where code ends
    d = (orig / elf).read_bytes()
    phoff, = struct.unpack_from("<I", d, 0x1C)
    _, off, vaddr, _, filesz, memsz = struct.unpack_from("<6I", d, phoff)
    yaml_text = (decomp / "config" / elf / "main.yaml").read_text()
    m = re.search(r"- \[(0x[0-9a-fA-F]+), data, \w+\]", yaml_text)
    data_file_off = int(m.group(1), 16)
    data_start = vaddr + data_file_off - off
    img = d[off:off + filesz]
    out.append(Target("main", "", (img, vaddr), [(data_start, vaddr + filesz)],
                      (vaddr + filesz, vaddr + memsz), (vaddr, data_start),
                      parse_labels(decomp / "asm" / elf / "main")))
    for name, rel in ver.get("overlays", {}).items():
        if name not in wanted:
            continue
        o = (orig / rel).read_bytes()
        _, ovl_id, load, text, data, bss = struct.unpack_from("<4sIIIII", o, 0)
        t0 = load + 0x40
        d0 = t0 + text
        out.append(Target(name, f"{name}__", (o, load), [(d0, d0 + data)], (d0 + data, d0 + data + bss),
                          (t0, d0), parse_labels(decomp / "asm" / elf / name)))
    return out


def c_symbols(src_dir):
    """Names defined and referenced by the decompiled C, per target directory."""
    defined, refs = defaultdict(set), defaultdict(set)
    for f in sorted(src_dir.rglob("*.c")):
        t = f.relative_to(src_dir).parts[0]
        text = f.read_text()
        defined[t] |= set(CDEF.findall(text))
        refs[t] |= set(re.findall(r"\b(func_[0-9A-F]{6,8}|D_[0-9A-F]{6,8})\b", text))
    return defined, refs


def pc_provided(pc_src):
    names = set(PC_NAMES) | LIBC
    for f in pc_src.rglob("*.c"):
        names |= set(CDEF.findall(f.read_text()))
    return names


def is_text(b):
    return 0x20 <= b < 0x7F or b in (9, 10, 13) or b >= 0x80     # ASCII or Shift-JIS bytes


def in_string(img, i):
    """True when the word at img[i] is the tail of a text string, e.g. the "T;1\\0" of "...DRIVE.DAT;1":
    the 4 bytes before it are text and the word is text up to its terminating 0."""
    if i < 4 or not all(is_text(b) for b in img[i - 4:i]) or img[i - 4:i] == b"\xff" * 4:
        return False
    word = img[i:i + 4]
    end = word.find(0)
    return end > 0 and all(is_text(b) for b in word[:end])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--decomp", type=Path, required=True)
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--underscore", action="store_true", help="C symbols carry a leading _ (32-bit Windows)")
    a = ap.parse_args()
    decomp = a.decomp.resolve()
    ver = json.loads((decomp / "config/versions.json").read_text())[a.game]
    cfg = json.loads((ROOT / "config" / f"{a.game}.json").read_text())
    raw = [(int(s, 16), int(e, 16)) for s, e in cfg.get("raw", [])]
    force_ptr = {int(k, 16) for k in cfg.get("pointer", [])}
    force_num = set()
    for k in cfg.get("number", []):           # one word, or [first, end) of a table
        if isinstance(k, list):
            force_num.update(range(int(k[0], 16), int(k[1], 16), 4))
        else:
            force_num.add(int(k, 16))
    elf = ver["elf"]
    src = decomp / "src" / elf
    defined, refs = c_symbols(src) if src.exists() else ({}, {})
    wanted = set(cfg.get("overlays", [])) | (set(defined) - {"main"})
    targets = load_targets(decomp, a.game, ver, wanted)
    provided = pc_provided(ROOT / "src")
    us = "_" if a.underscore else ""
    a.out.mkdir(parents=True, exist_ok=True)
    report = []
    main_t = targets[0]
    stub_needed = set()
    c_defined_all = {n for s in defined.values() for n in s}

    def resolve(addr, here):
        """Symbol for an address seen from target `here`: (name, offset) or None."""
        for t in ([here] + [x for x in targets if x is not here]) if here is not main_t else targets:
            if here is not main_t and t is not here and t is not main_t:
                continue                      # an overlay only sees itself and the main executable
            if t.owns(addr) and addr in t.labels:
                kind, name = t.labels[addr][0]
                return t, kind, name
        return None

    # C in one target that names data owned by another (main code touching the resident submain
    # overlay's variables): the owner also emits the exact name the C uses, unprefixed
    extra = defaultdict(lambda: defaultdict(set))
    code_alias = defaultdict(dict)        # owner target -> {name the C uses: owner's function symbol}
    owners_cfg = cfg.get("owners", {})
    pc_replaced = set(cfg.get("pc_provided", []))   # overlay functions the PC code replaces
    for tname, rs in refs.items():
        here = next((x for x in targets if x.name == tname), main_t)
        for r in rs:
            if r in pc_replaced:
                continue
            if r.startswith("D_") or r.startswith("func_"):
                addr = int(r.split("_")[1], 16)
                if not here.owns(addr):
                    cands = [x for x in targets if x.owns(addr)]
                    if r in owners_cfg:
                        cands = [x for x in targets if x.name == owners_cfg[r]]
                    elif len(cands) > 1:
                        # several overlays share the address: the in-game ones win (config "prefer")
                        pref = [x for p in cfg.get("prefer", []) for x in cands if x.name == p]
                        cands = pref[:1] or cands
                    if not cands:
                        sys.exit(f"{r} (used by {tname}) isn't inside any loaded target; "
                                 f"add its overlay to config/{a.game}.json or use PS2_RAM()")
                    if len(cands) > 1:
                        sys.exit(f"{r} (used by {tname}) is inside {', '.join(c.name for c in cands)}: "
                                 f'say which in config/{a.game}.json "owners"')
                    owner = cands[0]
                    if owner.text[0] <= addr < owner.text[1]:
                        # another overlay's function: an alias to its (prefixed) symbol
                        lab = next((n for k, n in owner.labels.get(addr, []) if k in ("glabel", "alabel")), None)
                        if lab is None:
                            sys.exit(f"{r} (used by {tname}) isn't the start of a function in {owner.name}")
                        if owner.sym(lab) != r:
                            code_alias[owner.name][r] = owner.sym(lab)   # becomes a linker --defsym
                        if lab not in c_defined_all and lab not in provided:
                            stub_needed.add(owner.sym(lab))
                    else:
                        extra[owner.name][addr].add(r)

    for t in targets:
        img, base = t.image
        lines = [f"# generated by tools/gen_data.py from your {elf} ({t.name}); do not commit",
                 ".data", ".balign 64"]
        if t.prefix:
            lines += [f".globl {us}{t.prefix}ovl_data_begin", f"{us}{t.prefix}ovl_data_begin:"]
        # labels needed: every label in the data ranges, plus whatever C references
        want = {addr for addr in t.labels if any(s <= addr < e for s, e in t.data_ranges)}
        for r in refs.get(t.name, set()):
            if r.startswith("D_"):
                want.add(int(r[2:], 16))
        want |= set(extra[t.name])
        bss_labels = sorted({addr for addr in t.labels if t.bss[0] <= addr < t.bss[1]} |
                            {addr for addr in extra[t.name] if t.bss[0] <= addr < t.bss[1]})
        stats = Counter()
        for s, e in t.data_ranges:
            pos = s
            names_at = defaultdict(list)
            for addr in want:
                if s <= addr < e:
                    for kind, name in t.labels.get(addr, [("dlabel", f"D_{addr:08X}")]):
                        names_at[addr].append(name)
                    # C may name a jump table or other label as D_<addr>
                    if f"D_{addr:08X}" in refs.get(t.name, set()) and f"D_{addr:08X}" not in names_at[addr]:
                        names_at[addr].append(f"D_{addr:08X}")
            byte_run = []

            def flush():
                if byte_run:
                    for i in range(0, len(byte_run), 32):
                        lines.append("    .byte " + ",".join(str(b) for b in byte_run[i:i + 32]))
                    byte_run.clear()

            while pos < e:
                if pos in names_at:
                    flush()
                    for n in dict.fromkeys(names_at[pos]):
                        lines += [f".globl {us}{t.sym(n)}", f"{us}{t.sym(n)}:"]
                    for n in sorted(extra[t.name].get(pos, ())):
                        lines += [f".globl {us}{n}", f"{us}{n}:"]
                in_raw = any(rs <= pos < re_ for rs, re_ in raw)
                if pos % 4 == 0 and pos + 4 <= e and not any(p in names_at for p in range(pos + 1, pos + 4)):
                    w, = struct.unpack_from("<I", img, pos - base)
                    target = None
                    if not in_raw and pos not in force_num and (0x100000 <= w < 0x2000000 or pos in force_ptr) \
                            and (pos in force_ptr or pos in names_at or not in_string(img, pos - base)):
                        target = resolve(w, t)
                    if target:
                        tt, kind, name = target
                        if kind in ("glabel", "alabel"):
                            sym = tt.sym(name)
                            if name not in c_defined_all and name not in provided:
                                stub_needed.add(sym)
                            flush()
                            lines.append(f"    .long {us}{sym}")
                            stats["function pointers"] += 1
                        elif kind == "jlabel":
                            flush()
                            lines.append(f"    .long 0  # jump-table entry {name}: only used by asm code")
                            stats["jump-table entries zeroed"] += 1
                        else:
                            flush()
                            lines.append(f"    .long {us}{tt.sym(name)}")
                            stats["data pointers"] += 1
                        pos += 4
                        continue
                    if not in_raw and 0x100000 <= w < 0x900000:
                        stats["in-range numbers left as numbers"] += 1
                        if len(report) < 400:
                            report.append(f"{t.name} 0x{pos:08X}: 0x{w:08X}")
                byte_run.append(img[pos - base])
                pos += 1
            flush()
        if t.prefix:
            lines += [f".globl {us}{t.prefix}ovl_data_end", f"{us}{t.prefix}ovl_data_end:"]
        lines += [".bss", ".balign 64"]
        if t.prefix:
            lines += [f".globl {us}{t.prefix}ovl_bss_begin", f"{us}{t.prefix}ovl_bss_begin:"]
        pos = t.bss[0]
        for addr in bss_labels + [t.bss[1]]:
            if addr > pos:
                lines.append(f"    .space {addr - pos}")
                pos = addr
            if addr < t.bss[1]:
                for kind, n in dict.fromkeys(t.labels.get(addr, [("dlabel", f"D_{addr:08X}")])):
                    lines += [f".globl {us}{t.sym(n)}", f"{us}{t.sym(n)}:"]
                for n in sorted(extra[t.name].get(addr, ())):
                    lines += [f".globl {us}{n}", f"{us}{n}:"]
        if t.prefix:
            lines += [f".globl {us}{t.prefix}ovl_bss_end", f"{us}{t.prefix}ovl_bss_end:"]
        if not a.underscore:                 # ELF targets: data isn't executable
            lines.append('.section .note.GNU-stack,"",@progbits')
        (a.out / f"data_{t.name}.s").write_text("\n".join(lines) + "\n")
        print(f"[{t.name}] {sum(e - s for s, e in t.data_ranges):,} data bytes, {t.bss[1] - t.bss[0]:,} bss bytes; "
              + ", ".join(f"{v} {k}" for k, v in stats.items()))
        if t.prefix:
            names = sorted({n for addr, ls in t.labels.items() for _, n in ls if t.owns(addr)})
            (a.out / f"names_{t.name}.h").write_text(
                f"/* generated: {t.name} overlay symbols get the prefix {t.prefix} on PC */\n"
                + "".join(f"#define {n} {t.prefix}{n}\n" for n in names if re.fullmatch(r"(func|D)_[0-9A-F]{8}", n)))

    # functions the C calls that nobody provides
    for tname, rs in refs.items():
        t = next((x for x in targets if x.name == tname), main_t)
        for r in rs:
            if r.startswith("func_") and len(r) == 13 and r not in c_defined_all and r not in provided:
                addr = int(r[5:], 16)
                owner = t if t.owns(addr) else main_t
                stub_needed.add(owner.sym(r))
    (a.out / "stubs.c").write_text(
        "/* generated by tools/gen_data.py: functions that aren't decompiled yet */\n"
        '#include "stubs.h"\n\n' +
        "".join(f"void {s}(void) {{ pc_unimplemented(\"{s}\"); }}\n" for s in sorted(stub_needed)))
    # names the C uses for another overlay's functions: the same address as that function
    (a.out / "aliases.rsp").write_text("".join(
        f"-Wl,--defsym,{us}{n}={us}{sym}\n" for t in targets for n, sym in sorted(code_alias[t.name].items())))
    ovls = [t for t in targets if t.prefix]
    tab = ["/* generated by tools/gen_data.py: the overlays linked into this build */",
           '#include "overlay.h"', ""]
    for t in ovls:
        p = t.prefix
        tab.append(f"extern char {p}ovl_data_begin[], {p}ovl_data_end[], {p}ovl_bss_begin[], {p}ovl_bss_end[];")
    tab += ["", "PcOverlay pc_overlays[] = {"]
    for t in ovls:
        p = t.prefix
        img, base = t.image
        ovl_id, = struct.unpack_from("<I", img, 4)
        tab.append(f'    {{"{t.name}", {ovl_id}, 0x{base:X}, {p}ovl_data_begin, {p}ovl_data_end, '
                   f"{p}ovl_bss_begin, {p}ovl_bss_end, 0}},")
    tab += ["    {0},", "};", ""]
    (a.out / "overlays.c").write_text("\n".join(tab))
    (a.out / "gen_report.txt").write_text(
        "In-range numbers left as numbers (review: pointers splat didn't name?)\n" + "\n".join(report) + "\n")
    print(f"{len(stub_needed)} stubs; report in {a.out / 'gen_report.txt'}")


if __name__ == "__main__":
    main()
