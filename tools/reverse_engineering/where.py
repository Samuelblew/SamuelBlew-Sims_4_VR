import sys, struct, bisect, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.argv = [sys.argv[0], 'D:/Games/The Sims 4/Game/Bin/TS4_x64.exe'] + sys.argv[1:]
addrs = sys.argv[2:]
import tsdis as D, capstone
n, vs, sva, rs, rp = [s for s in D.secs if s[0] == '.pdata'][0]
F = []
for i in range(0, vs, 12):
    b, e, u = struct.unpack_from('<III', D.data, rp + i)
    if b: F.append((b + D.BASE, e + D.BASE))
F.sort(); starts = [f[0] for f in F]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
def rd(va, n):
    for s in D.secs:
        nm, vsz, sva2, rsz, rp2 = s
        if sva2 + D.BASE <= va < sva2 + D.BASE + vsz: o = rp2 + va - sva2 - D.BASE; return D.data[o:o+n]
for a in addrs:
    v = int(a, 16); i = bisect.bisect_right(starts, v) - 1
    print('== %x in func %x..%x (+%x)' % (v, F[i][0], F[i][1], v - F[i][0]))
    s = max(F[i][0], v - 0x40)
    # disasm from function start to find aligned instruction boundary
    code = rd(F[i][0], v - F[i][0] + 8)
    ins = list(md.disasm(code, F[i][0]))
    for x in ins:
        if x.address >= v - 0x30 and x.address <= v + 2: print('  %x  %s %s' % (x.address, x.mnemonic, x.op_str))
