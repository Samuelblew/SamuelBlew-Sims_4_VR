import sys, glob, os, zlib
sys.path.insert(0, sys.argv[1])
from dbpf_min import entries, read
T_GFX = 0x62ECC59A
pk = glob.glob(r'D:\Games\The Sims 4\**\*.package', recursive=True)
for p in pk:
    try: f, es = entries(p)
    except Exception as e: continue
    for e in es:
        if e['type'] != T_GFX: continue
        try: d = read(f, e)
        except Exception: continue
        if d[:3] == b'CFX': d = d[:8] + zlib.decompress(d[8:])
        hits = [k for k in (b'VirtualCursor', b'camera_fpc', b'Crosshair', b'crosshair', b'Reticle', b'reticle') if k in d]
        if hits:
            print(os.path.relpath(p, r'D:\Games\The Sims 4'), '%016X' % ((e['insthi'] << 32) | e['instlo']), len(d), hits)
