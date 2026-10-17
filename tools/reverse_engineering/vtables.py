# Find vtables in .rdata containing given function addresses; print RTTI class + full vtable.
import sys, struct
sys.path.insert(0, '.'); sys.argv = sys.argv[:1] + ['D:/Games/The Sims 4/Game/Bin/TS4_x64.exe'] + sys.argv[1:]
import tsdis as D
B = D.BASE
def q(va): return struct.unpack_from('<Q', D.data, D.off(va))[0]
def rtti(vt):
    try:
        col = q(vt - 8) - B
        td = struct.unpack_from('<I', D.data, D.off(B + col + 12))[0]
        o = D.off(B + td + 16); e = D.data.index(b'\0', o)
        return D.data[o:e].decode('latin1')
    except Exception: return None
text = [s for s in D.secs if s[0] == '.text'][0]
tlo, thi = B + text[2], B + text[2] + text[1]
def is_code(v): return tlo <= v < thi
rd = [s for s in D.secs if s[0] == '.rdata'][0]
n, vs, sva, rs, rp = rd
blob = D.data[rp:rp + rs]
targets = [int(a, 16) for a in sys.argv[2].split(',')]
seen = set()
for t in targets:
    pat = struct.pack('<Q', t); i = blob.find(pat)
    while i >= 0:
        va = B + sva + i
        # walk back to vtable start: previous qword is a COL pointer (rdata) not code
        s = va
        while is_code(q(s - 8)): s -= 8
        if s not in seen:
            seen.add(s)
            name = rtti(s)
            ents = []
            e = s
            while is_code(q(e)): ents.append(q(e)); e += 8
            print(f"VTABLE {s:x} {name} slots={len(ents)} (target {t:x} at slot {(va - s)//8})")
            if len(sys.argv) > 3:
                for k, v in enumerate(ents): print(f"   [{k:3}] +{k*8:#05x} {v:x}")
        i = blob.find(pat, i + 8)
