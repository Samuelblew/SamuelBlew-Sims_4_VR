# Lê o .blend aberto e exporta armaduras (ossos, hierarquia, pose de repouso) e malhas (grupos de vértices).
# Só leitura: não salva nada no .blend.
import bpy, json, sys

out_path = sys.argv[sys.argv.index('--') + 1]
data = {'objects': [], 'armatures': []}

for ob in bpy.data.objects:
    entry = {'name': ob.name, 'type': ob.type, 'parent': ob.parent.name if ob.parent else None,
             'location': list(ob.location), 'rotation_euler': list(ob.rotation_euler), 'scale': list(ob.scale)}
    if ob.type == 'MESH':
        me = ob.data
        entry['vertices'] = len(me.vertices)
        entry['vertex_groups'] = [g.name for g in ob.vertex_groups]
        entry['modifiers'] = [(m.type, getattr(getattr(m, 'object', None), 'name', None)) for m in ob.modifiers]
    data['objects'].append(entry)
    if ob.type == 'ARMATURE':
        arm = {'object': ob.name, 'matrix_world': [list(r) for r in ob.matrix_world], 'bones': []}
        for b in ob.data.bones:
            arm['bones'].append({
                'name': b.name,
                'parent': b.parent.name if b.parent else None,
                'head': [round(v, 5) for v in b.head_local],
                'tail': [round(v, 5) for v in b.tail_local],
                'length': round(b.length, 5),
                'matrix_local': [[round(v, 5) for v in r] for r in b.matrix_local],
                'use_deform': b.use_deform,
                'children': [c.name for c in b.children],
            })
        pose = ob.pose
        arm['constraints'] = {pb.name: [(c.type, getattr(c, 'subtarget', '')) for c in pb.constraints]
                              for pb in pose.bones if pb.constraints}
        data['armatures'].append(arm)

with open(out_path, 'w', encoding='utf-8') as f:
    json.dump(data, f, indent=1, ensure_ascii=False)
print('DUMP_OK', out_path, len(data['objects']), [len(a['bones']) for a in data['armatures']])
