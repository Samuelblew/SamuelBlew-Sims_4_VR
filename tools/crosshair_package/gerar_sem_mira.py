# gerar_sem_mira.py: cria FPC_SemMira.package, um mod de interface que esconde o cursor virtual do The Sims 4
# (o círculo/"bolinha" da primeira pessoa e os outros ícones que ele mostra: seta, mão, "interagível"...).
# O cursor do mouse do Windows não vem deste filme e continua normal.
#
# O círculo é o quadro "camera_fpc" do cursor virtual (VirtualCursor_fla.Cursor_1) no filme Scaleform
# D30027A724ADC468 do UI.package: sprite 52 -> forma 51. Este script copia o filme do jogo, troca SÓ a
# forma 51 por uma forma vazia (o resto do cursor virtual e o cursor do mouse do Windows não mudam) e
# grava um .package com o mesmo tipo/grupo/instância, que o jogo usa no lugar do original.
#
# Uso: python gerar_sem_mira.py [caminho do UI.package] [pasta de saída]
# Refazer depois de atualizações do jogo (o filme original pode mudar).
import os, struct, sys, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dbpf_min import entries, read, write_package

UI = sys.argv[1] if len(sys.argv) > 1 else r'D:\Games\The Sims 4\Data\Client\UI.package'
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(os.path.abspath(__file__))
T_GFX, INSTANCE = 0x62ECC59A, 0xD30027A724ADC468
SPRITE_CLASS = b'VirtualCursor_fla.Cursor_1'
LABEL = b'camera_fpc'

def header_end(d):
    nbits = d[8] >> 3
    return 8 + (5 + 4 * nbits + 7) // 8 + 4

def tags(d, pos, end):
    out = []
    while pos < end:
        h = struct.unpack_from('<H', d, pos)[0]; code, ln, hl = h >> 6, h & 0x3F, 2
        if ln == 0x3F: ln = struct.unpack_from('<I', d, pos + 2)[0]; hl = 6
        out.append((code, pos, hl, ln)); pos += hl + ln
        if code == 0: break
    return out

def cid(d, t): return struct.unpack_from('<H', d, t[1] + t[2])[0]

def placed_cid(d, t):
    code, pos, hl, ln = t; p = pos + hl
    fl = d[p]; fl2 = d[p + 1] if code == 70 else 0; p += 2 if code == 70 else 1
    p += 2                                                   # depth
    if code == 70 and (fl2 & 0x08 or (fl2 & 0x10 and fl & 0x02)): p = d.index(b'\0', p) + 1
    return struct.unpack_from('<H', d, p)[0] if fl & 0x02 else None

f, es = entries(UI)
e = next(x for x in es if x['type'] == T_GFX and ((x['insthi'] << 32) | x['instlo']) == INSTANCE)
d = read(f, e)
assert d[:3] == b'GFX', 'formato inesperado: %r' % d[:3]
top = tags(d, header_end(d), len(d))

# id do sprite do cursor virtual (SymbolClass)
cursor_id = None
for t in top:
    if t[0] == 76:
        p = t[1] + t[2]; n = struct.unpack_from('<H', d, p)[0]; p += 2
        for _ in range(n):
            c = struct.unpack_from('<H', d, p)[0]; p += 2; q = d.index(b'\0', p)
            if d[p:q] == SPRITE_CLASS: cursor_id = c
            p = q + 1
assert cursor_id is not None, 'cursor virtual nao encontrado'
sprites = {cid(d, t): t for t in top if t[0] == 39}
st = sprites[cursor_id]
inner = tags(d, st[1] + st[2] + 4, st[1] + st[2] + st[3])
# todas as formas usadas dentro do cursor virtual (todos os quadros)
def subtree_shapes(spr_id, seen):
    if spr_id in seen or spr_id not in sprites: return
    seen.add(spr_id)
    t = sprites[spr_id]
    for t2 in tags(d, t[1] + t[2] + 4, t[1] + t[2] + t[3]):
        if t2[0] in (26, 70):
            c = placed_cid(d, t2)
            if c is None: continue
            if c in sprites: subtree_shapes(c, seen)
            else: inner_shapes.add(c)
inner_shapes = set(); inner_sprites = set()
subtree_shapes(cursor_id, inner_sprites)
# uso de cada forma no filme inteiro vs. dentro do cursor virtual
uses_all = {}; uses_in = {}
def count(tlist, inside):
    for t in tlist:
        if t[0] in (26, 70):
            c = placed_cid(d, t)
            if c in inner_shapes:
                uses_all[c] = uses_all.get(c, 0) + 1
                if inside: uses_in[c] = uses_in.get(c, 0) + 1
        elif t[0] == 39:
            count(tags(d, t[1] + t[2] + 4, t[1] + t[2] + t[3]), inside or cid(d, t) in inner_sprites)
count(top, False)
shape_ids = sorted(c for c in inner_shapes if uses_all.get(c, 0) == uses_in.get(c, 0))
skipped = sorted(c for c in inner_shapes if c not in shape_ids)
print('cursor virtual = sprite %d; %d formas exclusivas serao esvaziadas; compartilhadas (mantidas): %s' % (cursor_id, len(shape_ids), skipped))
uses = {c: 1 for c in shape_ids}

out = bytearray(d)
replaced = 0
for t in sorted([t for t in top if t[0] in (2, 22, 32, 83) and cid(d, t) in shape_ids and uses[cid(d, t)] == 1], key=lambda t: -t[1]):
    code, pos, hl, ln = t
    sid = cid(d, t)
    # forma vazia do mesmo tipo: limites zerados, sem preenchimentos/linhas, só o registro de fim
    rect0 = bytes([0x08, 0x00])                              # RECT com nbits=1, tudo zero
    if code == 83:  body = struct.pack('<H', sid) + rect0 + rect0 + b'\x00' + b'\x00\x00\x00\x00'
    else:           body = struct.pack('<H', sid) + rect0 + b'\x00\x00\x00\x00'
    newtag = struct.pack('<HI', (code << 6) | 0x3F, len(body)) + body
    out[pos:pos + hl + ln] = newtag
    replaced += 1
assert replaced > 0, 'nenhuma forma trocada'
struct.pack_into('<I', out, 4, len(out))                    # tamanho do arquivo no cabeçalho
os.makedirs(OUT, exist_ok=True)
pkg = os.path.join(OUT, 'FPC_SemMira.package')
write_package(pkg, [(T_GFX, e['group'], INSTANCE, bytes(out))])
print('gerado:', pkg, '(%d bytes; filme %d -> %d bytes)' % (os.path.getsize(pkg), len(d), len(out)))
