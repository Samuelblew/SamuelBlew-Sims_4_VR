# List all RTTI classes matching a regex, find their vtables (via COL), print slots.
import sys, struct, re
sys.path.insert(0, '.'); sys.argv = sys.argv[:1] + ['D:/Games/The Sims 4/Game/Bin/TS4_x64.exe'] + sys.argv[1:]
import tsdis as D
B = D.BASE
rx = re.compile(sys.argv[2].encode())
rd = [s for s in D.secs if s[0] == '.rdata'][0]; dt = [s for s in D.secs if s[0] == '.data'][0]
tx = [s for s in D.secs if s[0] == '.text'][0]
def q(va): return struct.unpack_from('<Q', D.data, D.off(va))[0]
def is_code(v): return B + tx[2] <= v < B + tx[2] + tx[1]
# Type descriptors live in .data: name ".?AV...@@"
tds = {}
for sec in (dt, rd):
    n, vs, sva, rs, rp = sec
    blob = D.data[rp:rp + rs]
    for m in re.finditer(rb'\.\?A[VU][^\0]{3,200}?@@\0', blob):
        if rx.search(m.group(0)):
            tds[B + sva + m.start() - 16] = m.group(0)[:-1].decode('latin1')
# COLs in .rdata: signature=1, ..., pTypeDescriptor RVA at +12, pSelf RVA at +20
n, vs, sva, rs, rp = rd; blob = D.data[rp:rp + rs]
rv = {td - B: name for td, name in tds.items()}
out = {}
for i in range(0, rs - 24, 4):
    if struct.unpack_from('<I', blob, i)[0] != 1: continue
    tdr = struct.unpack_from('<I', blob, i + 12)[0]
    if tdr in rv and struct.unpack_from('<I', blob, i + 20)[0] == sva + i:
        col = B + sva + i
        off = struct.unpack_from('<I', blob, i + 4)[0]
        j = blob.find(struct.pack('<Q', col))
        while j >= 0:
            vt = B + sva + j + 8; ents = []; e = vt
            while is_code(q(e)): ents.append(q(e)); e += 8
            out[(rv[tdr], off, vt)] = ents
            j = blob.find(struct.pack('<Q', col), j + 8)
for (name, off, vt), ents in sorted(out.items()):
    print(f"{name}  vtable={vt:x} subobj_off={off:#x} slots={len(ents)}")
    if len(sys.argv) > 3: print('   ' + ' '.join(f'{k}:{v:x}' for k, v in enumerate(ents)))
