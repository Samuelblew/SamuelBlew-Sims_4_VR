# Parser mínimo de SWF/GFX: tags de topo e de sprites, SymbolClass, PlaceObject.
import struct, sys

TAGN = {0:'End',1:'ShowFrame',2:'DefineShape',4:'PlaceObject',5:'RemoveObject',9:'SetBackgroundColor',22:'DefineShape2',
        26:'PlaceObject2',28:'RemoveObject2',32:'DefineShape3',39:'DefineSprite',43:'FrameLabel',70:'PlaceObject3',
        76:'SymbolClass',82:'DoABC',83:'DefineShape4',86:'DefineSceneAndFrameLabelData',56:'ExportAssets',57:'ImportAssets',
        71:'ImportAssets2',34:'DefineButton2',37:'DefineEditText',11:'DefineText',33:'DefineText2',1000:'GFX_ExporterInfo',
        1001:'GFX_DefineExternalImage',1002:'GFX_FontTextureInfo',1003:'GFX_DefineExternalGradient',1004:'GFX_DefineGradientMap',
        1005:'GFX_DefineCompactedFont',1006:'GFX_DefineExternalSound',1007:'GFX_DefineExternalStreamSound',1008:'GFX_DefineSubImage',
        1009:'GFX_DefineExternalImage2'}

def header_end(d):
    nbits = d[8] >> 3
    rect_bytes = (5 + 4 * nbits + 7) // 8
    return 8 + rect_bytes + 4

def tags(d, pos, end):
    out = []
    while pos < end:
        h = struct.unpack_from('<H', d, pos)[0]; code = h >> 6; ln = h & 0x3F; hl = 2
        if ln == 0x3F: ln = struct.unpack_from('<I', d, pos + 2)[0]; hl = 6
        out.append((code, pos, hl, ln))
        pos += hl + ln
        if code == 0: break
    return out

def symbolclass(d, t):
    code, pos, hl, ln = t
    p = pos + hl; n = struct.unpack_from('<H', d, p)[0]; p += 2; res = []
    for _ in range(n):
        cid = struct.unpack_from('<H', d, p)[0]; p += 2
        e = d.index(b'\0', p); res.append((cid, d[p:e].decode('latin1'))); p = e + 1
    return res

def char_id(d, t):
    code, pos, hl, ln = t
    return struct.unpack_from('<H', d, pos + hl)[0]

def place_info(d, t):
    code, pos, hl, ln = t
    p = pos + hl
    if code == 26:
        fl = d[p]; p += 1; fl2 = 0
    elif code == 70:
        fl = d[p]; fl2 = d[p + 1]; p += 2
    else:
        return None
    depth = struct.unpack_from('<H', d, p)[0]; p += 2
    if code == 70 and (fl2 & 0x08 or (fl2 & 0x10 and fl & 0x02)):
        e = d.index(b'\0', p); p = e + 1            # class name
    cid = None
    if fl & 0x02: cid = struct.unpack_from('<H', d, p)[0]
    return dict(depth=depth, flags=fl, flags2=fl2, cid=cid)

if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    top = tags(d, header_end(d), len(d))
    print(len(top), 'tags de topo')
