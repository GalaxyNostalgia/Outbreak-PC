"""protocheck.py: find functions declared with different ABI shapes across the C (count of args,
float vs int per arg, float vs int vs void return) -- they would break the PC build silently."""
import re, glob, sys
from collections import defaultdict
roots = sys.argv[1:] or ['/home/claude/outbreak-decomp/src', '/home/claude/outbreak-decomp/include', '/home/claude/outbreak-pc/src']
files = []
for r in roots:
    files += glob.glob(r + '/**/*.c', recursive=True) + glob.glob(r + '/**/*.h', recursive=True)
pat = re.compile(r'(?m)^(?:static\s+|extern\s+)?([A-Za-z_][\w \*]*?)\b(func_[0-9A-F]{8}|[a-z]\w*__func_[0-9A-F]{8})\s*\(([^;{)]*)\)\s*[;{]')
def kind(t):
    t = t.strip()
    if not t or t == 'void': return None
    if t == '...': return '...'
    t = re.sub(r'\b(const|volatile|struct|unsigned|signed)\b', '', t)
    if '*' in t or '[' in t: return 'i'
    base = t.split()[0] if t.split() else t
    if base in ('f32', 'float'): return 'f'
    if base in ('f64', 'double'): return 'd'
    if base in ('s64', 'u64', 'long'): return 'l'
    return 'i'
sigs = defaultdict(list)
for f in files:
    try: s = open(f, errors='replace').read()
    except: continue
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    for m in pat.finditer(s):
        ret, name, params = m.groups()
        if 'typedef' in ret or '(' in ret: continue
        ret = ret.strip()
        rk = 'v' if ret.split()[-1:] == ['void'] and '*' not in ret else kind(ret)
        ps = []
        for p in params.split(','):
            p = p.strip()
            if not p or p == 'void': continue
            if '(' in p: ps.append('i'); continue
            # drop the parameter name
            q = re.sub(r'\s*\b\w+\s*(\[[^\]]*\])?$', lambda mm: (' *' if mm.group(1) else ''), p) if re.search(r'[\w\]]\s*$', p) and len(p.replace('*',' ').split()) > 1 else p
            ps.append(kind(q))
        name = re.sub(r'^\w+__', '', name)
        addr = int(name[5:], 16)
        mo = re.search(r'SLPM_654\.28/(\w+)/', f)
        ovl = mo.group(1) if mo else 'pc'
        key = name if addr < 0x300000 or ovl in ('pc',) else ovl + ':' + name
        isdef = s[m.end()-1] == '{'
        sigs[key].append((rk, tuple(ps), f.replace('/home/claude/', ''), isdef))
bad = 0
def dangerous(lst):
    defs = [(r, p) for r, p, _, d in lst if d]
    for r, p, _, d in lst:
        for r2, p2, _, d2 in lst:
            if ('f' in (r, r2) or 'd' in (r, r2) or 'l' in (r, r2)) and r != r2 and 'v' not in (r, r2): return True
            n = min(len(p), len(p2))
            if any(a != b and (a in 'fdl' or b in 'fdl') for a, b in zip(p[:n], p2[:n]) if a and b): return True
    # a caller passing fewer args than the definition takes
    for dr, dp in defs:
        for r, p, _, d in lst:
            if not d and len(p) < len(dp) and '...' not in dp: return True
    return False
for name, lst in sorted(sigs.items()):
    shapes = {(r, p) for r, p, _, _ in lst}
    if len(shapes) > 1 and dangerous(lst):
        bad += 1
        print(name)
        seen = set()
        for r, p, f, d in lst:
            if (r, p, f) in seen: continue
            seen.add((r, p, f))
            print(f"    {r} {''.join(x or '-' for x in p)}  {f}{'  (def)' if d else ''}")
print(f"{bad} functions with differing shapes", file=sys.stderr)
