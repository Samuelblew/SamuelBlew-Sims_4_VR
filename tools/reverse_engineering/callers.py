# Find direct call/jmp rel32 callers of target functions; print containing function (via .pdata).
import sys, struct, re
sys.path.insert(0, '.')
import pdata as P
D = P.D
targets = {int(a, 16) for a in sys.argv[2].split(',')}
n, vs, sva, rs, rp = [s for s in D.secs if s[0] == '.text'][0]
text = D.data[rp:rp + rs]
import array
for m in re.finditer(rb'[\xe8\xe9]', text):
    i = m.start()
    if i + 5 > len(text): break
    va = D.BASE + sva + i
    t = va + 5 + struct.unpack_from('<i', text, i + 1)[0]
    if t in targets:
        f = P.func_of(va)
        print(f"{'call' if text[i]==0xe8 else 'jmp '} {va:x} -> {t:x}  in {f[0]:x}-{f[1]:x}" if f else f"{va:x} -> {t:x} (no pdata)")
