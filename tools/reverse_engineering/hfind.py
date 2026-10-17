import sys, struct, zlib
sys.path.insert(0, '.'); sys.argv = ['x', 'D:/Games/The Sims 4/Game/Bin/TS4_x64.exe']
import tsdis as D
n, vs, sva, rs, rp = [s for s in D.secs if s[0] == ".text"][0]
text = D.data[rp:rp + rs]
def fnv(s, a, bits=32):
    if bits == 32: h, p, m = 0x811c9dc5, 0x01000193, 0xffffffff
    else: h, p, m = 0xcbf29ce484222325, 0x100000001b3, (1 << 64) - 1
    for c in s:
        if a: h ^= c; h = (h * p) & m
        else: h = (h * p) & m; h ^= c
    return h
for w in ['headlineeffects', 'fullscreen', 'resetsim', 'help', 'fps', 'testingcheats', 'bb.moveobjects', 'cas.fulleditmode']:
    for v in (w, w.upper()):
        b = v.encode()
        for name, h in [('fnv1', fnv(b, 0)), ('fnv1a', fnv(b, 1)), ('crc32', zlib.crc32(b)), ('fnv1-utf16', fnv(v.encode('utf-16le'), 0)), ('fnv1a-utf16', fnv(v.encode('utf-16le'), 1))]:
            pat = b'\xba' + struct.pack('<I', h)  # mov edx, imm32
            i = text.find(pat)
            any_i = text.find(struct.pack('<I', h))
            if any_i >= 0:
                print(v, name, hex(h), 'mov_edx' if i >= 0 else 'imm', hex(D.BASE + sva + (i if i >= 0 else any_i)))
