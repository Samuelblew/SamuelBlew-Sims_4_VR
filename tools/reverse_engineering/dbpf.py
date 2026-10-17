# Leitor mínimo de DBPF 2.1 (pacotes do TS4): lista recursos, descomprime (zlib / refpack) e procura texto.
import struct, sys, zlib, collections

def refpack(d):
    # descompressão RefPack (EA) - formato do TS4 (flags no byte 0, 0xFB no byte 1)
    i = 0
    flags = d[0]; i = 2
    size_len = 4 if flags & 0x80 else 3
    out_len = int.from_bytes(d[i:i + size_len], 'big'); i += size_len
    out = bytearray()
    n = len(d)
    while i < n:
        b0 = d[i]
        if b0 < 0x80:
            b1 = d[i + 1]; i += 2
            plain = b0 & 3; cnt = ((b0 >> 2) & 7) + 3; off = ((b0 & 0x60) << 3) + b1 + 1
        elif b0 < 0xC0:
            b1, b2 = d[i + 1], d[i + 2]; i += 3
            plain = b1 >> 6; cnt = (b0 & 0x3F) + 4; off = ((b1 & 0x3F) << 8) + b2 + 1
        elif b0 < 0xE0:
            b1, b2, b3 = d[i + 1], d[i + 2], d[i + 3]; i += 4
            plain = b0 & 3; cnt = ((b0 & 0x0C) << 6) + b3 + 5; off = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif b0 < 0xFC:
            plain = ((b0 & 0x1F) << 2) + 4; cnt = 0; off = 0; i += 1
        else:
            plain = b0 & 3; cnt = 0; off = 0; i += 1
        out += d[i:i + plain]; i += plain
        for _ in range(cnt):
            out.append(out[-off])
        if b0 >= 0xFC:
            break
    return bytes(out)

def entries(path):
    f = open(path, 'rb')
    h = f.read(96)
    assert h[:4] == b'DBPF', 'nao e DBPF'
    count = struct.unpack_from('<I', h, 36)[0]
    idx_pos = struct.unpack_from('<I', h, 64)[0] or struct.unpack_from('<Q', h, 40)[0]
    f.seek(idx_pos)
    flags = struct.unpack('<I', f.read(4))[0]
    const = {}
    names = ['type', 'group', 'insthi']
    for k, nm in enumerate(names):
        if flags & (1 << k):
            const[nm] = struct.unpack('<I', f.read(4))[0]
    res = []
    for _ in range(count):
        e = {}
        for nm in names:
            e[nm] = const[nm] if nm in const else struct.unpack('<I', f.read(4))[0]
        e['instlo'], e['pos'], sz, e['usize'] = struct.unpack('<IIII', f.read(16))
        e['csize'] = sz & 0x7FFFFFFF
        if sz & 0x80000000:
            e['comp'], e['committed'] = struct.unpack('<HH', f.read(4))
        else:
            e['comp'] = 0
        res.append(e)
    return f, res

def read(f, e):
    f.seek(e['pos']); d = f.read(e['csize'])
    c = e['comp']
    if c == 0x5A42: return zlib.decompress(d)
    if c == 0xFFFF: return refpack(d)
    if c == 0xFFFE: return d   # streamable (raro)
    return d

if __name__ == '__main__':
    f, es = entries(sys.argv[1])
    types = collections.Counter(e['type'] for e in es)
    print(len(es), 'recursos'); [print('  %08X %d' % t) for t in types.most_common(15)]
