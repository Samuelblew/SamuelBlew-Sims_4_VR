# dbpf_min.py: leitura e gravação mínimas de pacotes DBPF 2.1 (The Sims 4).
import struct, zlib

def _refpack(d):
    flags = d[0]; i = 2
    size_len = 4 if flags & 0x80 else 3
    i += size_len
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
    if h[:4] != b'DBPF': raise ValueError('nao e DBPF')
    count = struct.unpack_from('<I', h, 36)[0]
    idx_pos = struct.unpack_from('<I', h, 64)[0] or struct.unpack_from('<I', h, 40)[0]
    f.seek(idx_pos)
    flags = struct.unpack('<I', f.read(4))[0]
    names = ['type', 'group', 'insthi']
    const = {}
    for k, nm in enumerate(names):
        if flags & (1 << k): const[nm] = struct.unpack('<I', f.read(4))[0]
    res = []
    for _ in range(count):
        e = {nm: const[nm] if nm in const else struct.unpack('<I', f.read(4))[0] for nm in names}
        e['instlo'], e['pos'], sz, e['usize'] = struct.unpack('<IIII', f.read(16))
        e['csize'] = sz & 0x7FFFFFFF
        e['comp'] = struct.unpack('<HH', f.read(4))[0] if sz & 0x80000000 else 0
        res.append(e)
    return f, res

def read(f, e):
    f.seek(e['pos']); d = f.read(e['csize'])
    if e['comp'] == 0x5A42: return zlib.decompress(d)
    if e['comp'] == 0xFFFF: return _refpack(d)
    return d

def write_package(path, resources):
    """resources: lista de (tipo, grupo, instância64, bytes). Grava comprimido com zlib (0x5A42)."""
    body = bytearray()
    index = bytearray(struct.pack('<I', 0))
    for t, g, inst, data in resources:
        comp = zlib.compress(data, 9)
        off = 96 + len(body)
        body += comp
        index += struct.pack('<IIIIIIIHH', t, g, inst >> 32, inst & 0xFFFFFFFF, off,
                             len(comp) | 0x80000000, len(data), 0x5A42, 1)
    hdr = bytearray(96)
    hdr[0:4] = b'DBPF'
    struct.pack_into('<II', hdr, 4, 2, 1)                   # versão 2.1
    struct.pack_into('<I', hdr, 36, len(resources))          # número de entradas
    struct.pack_into('<I', hdr, 44, len(index))              # tamanho do índice
    struct.pack_into('<I', hdr, 60, 3)                       # versão do índice
    struct.pack_into('<I', hdr, 64, 96 + len(body))          # posição do índice
    with open(path, 'wb') as fo:
        fo.write(hdr); fo.write(body); fo.write(index)
