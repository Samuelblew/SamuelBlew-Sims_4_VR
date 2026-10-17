import sys, re, zlib
sys.path.insert(0, sys.argv[1])
from dbpf_min import entries, read
T_GFX = 0x62ECC59A
f, es = entries(r'D:\Games\The Sims 4\Data\Client\UI.package')
import collections
cnt = collections.Counter()
for e in es:
    if e['type'] != T_GFX: continue
    d = read(f, e)
    if d[:3] == b'CFX': d = d[:8] + zlib.decompress(d[8:])
    if b'VirtualCursor' not in d: continue
    names = sorted(set(m.decode() for m in re.findall(rb'VirtualCursor[A-Za-z0-9_.]*', d)))
    cnt[tuple(names)] += 1
    if b'VirtualCursor_fla' in d: print('%016X' % ((e['insthi'] << 32) | e['instlo']), len(d), names)
for k, v in cnt.most_common(): print(v, k)
