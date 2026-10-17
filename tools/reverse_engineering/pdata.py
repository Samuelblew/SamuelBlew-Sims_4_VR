import sys, struct, bisect
sys.path.insert(0, '.'); sys.argv = sys.argv[:1] + ['D:/Games/The Sims 4/Game/Bin/TS4_x64.exe'] + sys.argv[1:]
import tsdis as D
n, vs, sva, rs, rp = [s for s in D.secs if s[0] == ".pdata"][0]
F = []
for i in range(0, vs, 12):
    b, e, u = struct.unpack_from("<III", D.data, rp + i)
    if b: F.append((b + D.BASE, e + D.BASE))
F.sort()
starts = [f[0] for f in F]
def func_of(va):
    i = bisect.bisect_right(starts, va) - 1
    return F[i] if i >= 0 and F[i][0] <= va < F[i][1] else None
