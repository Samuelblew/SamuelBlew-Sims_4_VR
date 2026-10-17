import itertools
H = {0xa1495548:'Mode',0xfded9b84:'AttachJoint',0x3dcf6317:'SmoothingParams',0x82ef323d:'LookatTarget',0x6204169a:'IdleTimeParam',0x538584aa:'FocusParams',0x4551bb8d:'EyeWeightParam',0xb85125d9:'SimpleMode'}
def fnv1(s):
    h = 0x811c9dc5
    for c in s.encode():
        h = (h * 0x01000193) & 0xffffffff; h ^= c
    return h
def variants(parts):
    out = set()
    for sep in ['', '_', '.']:
        out.add(sep.join(parts))
    return out
pre_parts = [[], ['fps'], ['fpc'], ['fp'], ['fps','cam'], ['fps','camera'], ['fpc','camera'], ['fpc','cam'], ['camera'], ['cam'], ['first','person'], ['firstperson'], ['camera','fps'], ['cam','fps'], ['camera','fpc'], ['fpv'], ['set','fps','camera'], ['fps','camera','set']]
word_parts = [
 ['mode'],['set','mode'],['enable'],['on'],['attach'],['set','camera','mode'],['camera','mode'],
 ['attach','joint'],['joint'],['set','joint'],['set','attach','joint'],['attach','bone'],['bone'],['set','bone'],
 ['smoothing'],['smoothing','params'],['smooth'],['set','smoothing'],['set','smoothing','params'],['smoothing','param'],
 ['lookat'],['look','at'],['lookat','target'],['look','at','target'],['set','lookat'],['set','lookat','target'],['target'],
 ['idle','time'],['idletime'],['idle','time','param'],['set','idle','time'],['idle'],['param'],['set','param'],['set'],['params'],['set','params'],
 ['eye','weight'],['eyeweight'],['eye','weight','param'],['set','eye','weight'],['eye'],
 ['simple'],['simple','mode'],['set','simple','mode'],['set','simple'],['simplemode'],
 ['focus'],['focus','params'],['set','focus'],['set','focus','params'],['focus','param'],['zoom'],
]
found = {}
for p in pre_parts:
    for w in word_parts:
        for psep in ['', '_', '.', ' ']:
            pv = variants(p) if p else {''}
            for a in pv:
                for b in variants(w):
                    for c in ([a + psep + b] if a else [b]):
                        h = fnv1(c.lower())
                        if h in H: found.setdefault(H[h], set()).add(c.lower())
for k in H.values(): print(k.ljust(16), sorted(found.get(k, [])))
