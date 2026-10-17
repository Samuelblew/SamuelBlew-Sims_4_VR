import sys, re, struct
sys.path.insert(0, '.')
import pdata as P
D = P.D
targets = {int(a, 16): a for a in sys.argv[2].split(",")}
n, vs, sva, rs, rp = [s for s in D.secs if s[0] == ".text"][0]
text = D.data[rp:rp + rs]
for m in re.finditer(rb"[\x48\x4c][\x8d\x8b][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", text):
    i = m.start(); va = D.BASE + sva + i
    t = va + 7 + struct.unpack_from("<i", text, i + 3)[0]
    if t in targets:
        f = P.func_of(va)
        print(f"{va:x} uses {t:x} {D.string_at(t)!r:24} in func {f[0]:x}-{f[1]:x}" if f else f"{va:x} uses {t:x} (no pdata)")
for a in sys.argv[3].split(",") if len(sys.argv) > 3 else []:
    f = P.func_of(int(a, 16)); print("HOOK", a, "in func", f"{f[0]:x}-{f[1]:x}" if f else None)
