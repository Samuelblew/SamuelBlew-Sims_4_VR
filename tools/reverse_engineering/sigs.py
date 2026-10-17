# Build unique AOB signatures (rel32/rip displacements wildcarded) for given functions.
import sys, re
sys.path.insert(0, '.'); sys.argv = sys.argv[:1] + ['D:/Games/The Sims 4/Game/Bin/TS4_x64.exe'] + sys.argv[1:]
import tsdis as D
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
n, vs, sva, rs, rp = [s for s in D.secs if s[0] == '.text'][0]
text = D.data[rp:rp + rs]
def pattern(va, nbytes):
    o = D.off(va); pat = []
    for ins in md.disasm(D.data[o:o + nbytes + 16], va):
        b = list(ins.bytes); mask = [True] * len(b)
        # wildcard displacement / immediate rel bytes
        if ins.disp_size and ('rip' in ins.op_str or True) and ins.disp_offset:
            if 'rip' in ins.op_str:
                for k in range(ins.disp_offset, ins.disp_offset + ins.disp_size): mask[k] = False
        if ins.mnemonic in ('call', 'jmp') or ins.mnemonic.startswith('j'):
            if ins.imm_size and ins.imm_offset:
                for k in range(ins.imm_offset, ins.imm_offset + ins.imm_size): mask[k] = False
        pat += [(x if m else None) for x, m in zip(b, mask)]
        if len(pat) >= nbytes: break
    return pat
def to_re(pat): return re.compile(b''.join(re.escape(bytes([x])) if x is not None else b'.' for x in pat), re.S)
def fmt(pat): return ' '.join(f'{x:02X}' if x is not None else '??' for x in pat)
for item in sys.argv[2].split(','):
    a, name = item.split(':'); va = int(a, 16)
    for nb in (16, 24, 32, 48, 64, 96):
        pat = pattern(va, nb); hits = [m.start() for m in to_re(pat).finditer(text)]
        if len(hits) == 1: break
    ok = 'UNICA' if len(hits) == 1 else f'{len(hits)} hits'
    print(f'{name:30} {va:x}  [{ok}]  {fmt(pat)}')
