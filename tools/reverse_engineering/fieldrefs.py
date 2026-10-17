# Find instructions referencing [reg + OFFSET] inside an address range; report containing function.
import sys, re
sys.path.insert(0, '.')
import pdata as P
D = P.D
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
md = Cs(CS_ARCH_X86, CS_MODE_64)
lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
offs = [o.lower() for o in sys.argv[4].split(',')]
seen = set()
for f in P.F:
    if not (lo <= f[0] < hi): continue
    o = D.off(f[0])
    for ins in md.disasm(D.data[o:o + (f[1] - f[0])], f[0]):
        for off in offs:
            if re.search(r'\+ ' + off + r'\]', ins.op_str):
                print(f'{ins.address:x} in {f[0]:x}: {ins.mnemonic} {ins.op_str}')
