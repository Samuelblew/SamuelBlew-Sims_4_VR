import sys, re, struct
sys.argv = [sys.argv[0], sys.argv[1]] + sys.argv[2:]
sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
import tsdis as D
targets = {int(a, 16) for a in sys.argv[2].split(",")}
n, vs, sva, rs, rp = [s for s in D.secs if s[0] == ".text"][0]
text = D.data[rp:rp + rs]
pat = re.compile(rb"[\x48\x4c][\x8d\x8b\x89\xc7][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", re.S)
for m in pat.finditer(text):
    i = m.start()
    disp = struct.unpack_from("<i", text, i + 3)[0]
    va = D.BASE + sva + i
    t = va + 7 + disp
    if t in targets:
        print(f"{va:x} -> {t:x}")
