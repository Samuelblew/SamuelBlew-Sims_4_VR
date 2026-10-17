# Analisa o XML exportado do RenderDoc (captura do TS4) e imprime a estrutura do frame.
# No XML, o conteúdo de cada lista de comandos aparece entre dois chunks ExecuteCommandList
# com o mesmo id (marcador de início e de fim).
import sys, collections
import xml.etree.ElementTree as ET

path = sys.argv[1]
tex = {}        # id -> (w, h, fmt, samples, bind)
buf = {}        # id -> (bytes, bind)
view2res = {}   # view id -> resource id
events = []     # (chunkIndex, ctx, name, info)

def child(el, name):
    for c in el:
        if c.get('name') == name:
            return c
    return None

def deep(el, *names):
    for n in names:
        for c in el.iter():
            if c.get('name') == n:
                return c
    return None

def text(el):
    return el.text if el is not None else None

for _, el in ET.iterparse(path, events=('end',)):
    if el.tag != 'chunk':
        continue
    full = el.get('name'); name = full.split('::')[-1]; idx = int(el.get('chunkIndex'))
    ctx = text(child(el, 'Context'))
    info = None
    if full == 'ID3D11Device::CreateTexture2D':
        d = deep(el, 'Descriptor', 'pDesc'); rid = deep(el, 'pTexture')
        if d is not None and rid is not None:
            f = child(d, 'Format'); b = child(d, 'BindFlags'); sd = child(d, 'SampleDesc')
            tex[rid.text] = (text(child(d, 'Width')), text(child(d, 'Height')),
                             (f.get('string') or '?').replace('DXGI_FORMAT_', '') if f is not None else '?',
                             text(child(sd, 'Count')) if sd is not None else '1',
                             b.get('string') if b is not None else '?')
    elif full == 'ID3D11Device::CreateBuffer':
        d = deep(el, 'pDesc'); rid = deep(el, 'pBuffer')
        if d is not None and rid is not None:
            b = child(d, 'BindFlags')
            buf[rid.text] = (text(child(d, 'ByteWidth')), b.get('string') if b is not None else '?')
    elif name in ('CreateRenderTargetView', 'CreateDepthStencilView', 'CreateShaderResourceView'):
        res = deep(el, 'pResource'); v = deep(el, 'pView')
        if res is not None and v is not None:
            view2res[v.text] = res.text
    elif name == 'OMSetRenderTargets':
        arr = child(el, 'ppRenderTargetViews')
        rtvs = [c.text for c in arr if c.text != '0'] if arr is not None else []
        info = (tuple(rtvs), text(child(el, 'pDepthStencilView')))
    elif name == 'ExecuteCommandList':
        info = text(child(el, 'CommandList'))
    elif name in ('FinishCommandList', 'FinishCommandlist'):
        info = text(deep(el, 'pCommandList'))
    elif name == 'Map':
        mt = child(el, 'MapType')
        info = (text(child(el, 'pResource')), mt.get('string') if mt is not None else '?')
    elif name in ('VSSetConstantBuffers', 'VSSetConstantBuffers1', 'PSSetConstantBuffers', 'PSSetConstantBuffers1'):
        arr = child(el, 'ppConstantBuffers')
        info = (text(child(el, 'StartSlot')), tuple(c.text for c in arr) if arr is not None else ())
    elif name == 'RSSetViewports':
        vp = deep(el, 'pViewports')
        if vp is not None and len(list(vp)):
            info = (text(child(vp[0], 'Width')), text(child(vp[0], 'Height')))
    elif name in ('UpdateSubresource', 'UpdateSubresource1'):
        info = text(deep(el, 'pDstResource'))
    events.append((idx, ctx, name, info))
    el.clear()

events.sort()
ctxcount = collections.Counter(e[1] for e in events if e[1])
IMM = next((e[1] for e in events if e[2] == 'ExecuteCommandList'), None)

def rt_desc(rtvs, dsv):
    out = []
    for r in rtvs:
        t = tex.get(view2res.get(r, ''))
        out.append('%sx%s %s%s' % (t[0], t[1], t[2], (' MSAAx' + t[3]) if t[3] not in ('1', None) else '') if t else 'rtv' + r)
    if dsv and dsv != '0':
        t = tex.get(view2res.get(dsv, ''))
        out.append('DS %sx%s %s%s' % (t[0], t[1], t[2], (' MSAAx' + t[3]) if t[3] not in ('1', None) else '') if t else 'DS?')
    return ', '.join(out) or '(sem alvo)'

DRAWS = {'Draw', 'DrawIndexed', 'DrawIndexedInstanced', 'DrawInstanced', 'Dispatch', 'DrawAuto'}
print('contexto principal =', IMM, '| chunks por contexto:', dict(ctxcount))
print('\n=== LINHA DO TEMPO (ordem de execucao; L = dentro de lista de comandos) ===')
inside = None; draws = 0; vp = None; ctx_now = None
def flush():
    global draws
    if draws:
        print('      %s desenhos: %d  vp=%s' % ('L' if inside else ' ', draws, vp))
    draws = 0
for idx, ctx, name, info in events:
    if name == 'ExecuteCommandList':
        flush()
        if inside == info:
            print('[%d] <- fim da lista %s' % (idx, info)); inside = None
        else:
            inside = info; print('[%d] -> lista %s' % (idx, info))
        continue
    if name == 'OMSetRenderTargets':
        flush()
        print('[%d] %s alvo (ctx %s): %s' % (idx, 'L' if inside else ' ', ctx, rt_desc(*info)))
    elif name == 'RSSetViewports':
        vp = info
    elif name in DRAWS:
        draws += 1
    elif name in ('CopyResource', 'ResolveSubresource', 'CopySubresourceRegion1', 'CopySubresourceRegion',
                  'ClearRenderTargetView', 'ClearDepthStencilView', 'GenerateMips'):
        flush(); print('[%d] %s %s (ctx %s)' % (idx, 'L' if inside else ' ', name, ctx))
flush()

print('\n=== CONSTANT BUFFERS ===')
upd = collections.defaultdict(collections.Counter)
for idx, ctx, name, info in events:
    if name == 'Map' and info and info[0] in buf and 'CONSTANT' in (buf[info[0]][1] or ''):
        upd[info[0]]['map@' + ('imm' if ctx == IMM else 'def')] += 1
    if name in ('UpdateSubresource', 'UpdateSubresource1') and info in buf and 'CONSTANT' in (buf[info][1] or ''):
        upd[info]['upd@' + ('imm' if ctx == IMM else 'def')] += 1
binds = collections.defaultdict(collections.Counter)
for idx, ctx, name, info in events:
    if 'SetConstantBuffers' in name and info:
        slot = int(info[0] or 0)
        for i, b in enumerate(info[1]):
            if b and b != '0':
                binds[b][name[:2] + str(slot + i) + ('@imm' if ctx == IMM else '@def')] += 1
rows = sorted(((sum(c.values()), b, buf[b][0], dict(c), dict(binds[b].most_common(4))) for b, c in upd.items()), reverse=True)
print('total de CBs atualizados:', len(rows))
for r in rows[:30]:
    print(r)
print('\nCBs atualizados SO no contexto principal e usados em listas (candidatos a camera por frame):')
for r in rows:
    if all(k.endswith('@imm') for k in r[3]) and any('@def' in k for k in r[4]):
        print(r)
