# decomp.py <outdir> <va:label> ... : decompila funções avulsas com o projeto Ghidra local (sem análise completa).
import sys, os, subprocess, struct, bisect
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
out = sys.argv[1]; items = sys.argv[2:]
sys.argv = ['x', 'D:/Games/The Sims 4/Game/Bin/TS4_x64.exe']
import tsdis as D
n, vs, sva, rs, rp = [s for s in D.secs if s[0] == '.pdata'][0]
F = []
for i in range(0, vs, 12):
    b, e, u = struct.unpack_from('<III', D.data, rp + i)
    if b: F.append((b + D.BASE, e + D.BASE))
F.sort(); starts = [f[0] for f in F]
args = []
for it in items:
    va, label = it.split(':') if ':' in it else (it, it)
    v = int(va, 16)
    i = bisect.bisect_right(starts, v) - 1
    end = F[i][1] if i >= 0 and F[i][0] == v else v + 0x400
    # funções com vários blocos .pdata (encadeados): estende enquanto o próximo começa onde este acaba
    while i + 1 < len(F) and F[i + 1][0] == end: i += 1; end = F[i][1]
    args.append('%x:%x:%s' % (v, end, label))
cmd = ['C:/tools/ghidra_12.1.4_PUBLIC/support/analyzeHeadless.bat', os.path.join(here, 'gproj'), 'ts4',
       '-process', 'TS4_x64.exe', '-noanalysis', '-readOnly', '-scriptPath', os.path.join(here, 'ghidra_scripts'),
       '-postScript', 'DecompileList.java', out] + args
r = subprocess.run(cmd, capture_output=True, text=True)
print('\n'.join(l for l in (r.stdout + r.stderr).splitlines() if 'ERROR' in l or 'Exception' in l)[-2000:])
for a in args: print('ok', a)
