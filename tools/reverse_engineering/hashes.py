import sys, re, itertools
sys.path.insert(0, '.'); sys.argv = ['x', 'D:/Games/The Sims 4/Game/Bin/TS4_x64.exe']
import tsdis as D
txt = D.dis(0x140217d30, 170, stop_ret=False).splitlines()
H = {}
cur = None
for l in txt:
    m = re.search(r"mov\s+edx, (0x[0-9a-f]+)", l)
    if m: cur = int(m.group(1), 16)
    m = re.search(r"-> (141b42[0-9a-f]+)", l)
    if m and cur: H[cur] = m.group(1); cur = None
names = {'141b42968':'SetFpsCameraMode','141b42aa0':'AttachJoint','141b42b68':'Smoothing','141b42c20':'LookatTarget','141b42cd0':'IdleTimeParam','141b42dd8':'FocusParams','141b42d38':'EyeWeightParam','141b42d68':'SimpleMode'}
for h, v in H.items(): print(hex(h), names[v])
def fnv1(s, a=False):
    h = 0x811c9dc5
    for c in s.encode():
        if a: h ^= c; h = (h * 0x01000193) & 0xffffffff
        else: h = (h * 0x01000193) & 0xffffffff; h ^= c
    return h
pre = ['', 'fps', 'fpc', 'fpscam', 'fpscamera', 'fps_camera', 'camera', 'cam', 'firstperson', 'first_person', 'fp']
sep = ['.', '_', '', ' ']
words = ['mode','setmode','attach','attachjoint','joint','setjoint','attach_joint','smoothing','smooth','setsmoothing','smoothing_params','lookat','look_at','lookattarget','target','setlookat','idletime','idle','idle_time','param','setparam','set_param','eyeweight','eye_weight','eye','seteyeweight','simple','simplemode','simple_mode','setsimplemode','focus','focusparams','focus_params','setfocus','on','enable','toggle','setcameramode','set_fps_camera_mode','setfpscameramode','fpscameramode']
found = {}
for p in pre:
    for s in sep:
        for w in words:
            for cand in {p + s + w if p else w}:
                for c in (cand, cand.lower()):
                    for a in (False, True):
                        h = fnv1(c, a)
                        if h in H: found[h] = (c, 'fnv1a' if a else 'fnv1')
for full in ['SetFpsCameraMode','SetFpsCameraAttachJoint','SetFpsCameraSmoothingParams','SetFpsCameraLookatTarget','SetFpsCameraIdleTimeParam','SetFpsCameraEyeWeightParam','FpsCameraSetSimpleMode','SetFpsCameraFocusParams']:
    for c in (full, full.lower()):
        for a in (False, True):
            if fnv1(c, a) in H: found[fnv1(c, a)] = (c, a)
print('FOUND', found)
