# fpc_walk.py v9: lado Python do modo FPS (The Sims 4, Python 3.7).
#
# O fpc_rig.dll (nativo) é dono da câmera e anda suave no cliente. Este mod é a
# autoridade da simulação: valida cada passo no navmesh e move o Sim junto.
#
# Comandos (enviados pela DLL):
#   fpc.move <x> <z> <fx> <fz>   leva o Sim até (x, z), virado para (fx, fz), se o caminho estiver livre
#   fpc.face <fx> <fz>           vira o Sim para (fx, fz); também atualiza o estado livre/ocupado
#   fpc.cam <x> <z>              sonda: a câmera (cabeça no VR) está atravessando uma parede?
#   fpc.walk <dx> <dz> <correr>  locomoção pela ROTA do jogo (o Sim anda/corre com animação, contorna objetos,
#                                atravessa portas e escadas). dx = dz = 0 para.
#
# Sinais de volta (via client_cheat, interceptados pela DLL; o jogo nunca os vê):
#   camera.fpc.idletime -1000   Sim livre (câmera do jogador, WASD ativo)
#   camera.fpc.idletime -1001   Sim ocupado com uma interação (câmera segue o corpo)
#   camera.fpc.idletime -1002   passo bloqueado (parede/objeto)
#   camera.fpc.idletime -1003   câmera atrás de parede/objeto (a linha Sim -> câmera cruza algo)
#   camera.fpc.idletime -1004   câmera livre de novo
#   camera.fpc.idletime -1005   a roda de opções (pie menu) foi criada para o Sim ativo
#   camera.fpc.idletime -1006   uma opção da roda foi escolhida
#   camera.fpc.idletime -1007   menu VR pronto: opções gravadas em fpc_menu.txt (Fase 3)
#   camera.fpc.idletime -1009   objeto agarrado: id em fpc_grab.txt (Fase 5)
#   camera.fpc.idletime -1010   nada para agarrar
#   camera.fpc.idletime -1011   raycast do laser pronto: fpc_hit.txt (id tipo x y z)
#
# Baseado em sims.teleport_instantly e sims.test_raytest (server_commands/sim_commands.py).
# NÃO TESTADO no jogo.

import os
import time

import routing
import services
import services.terrain_service
import sims4.commands
import sims4.math
from objects.terrain import GoHereSuperInteraction, TerrainPoint
from server.pick_info import PickInfo, PickType

SIGNAL_FREE, SIGNAL_BUSY, SIGNAL_BLOCKED = -1000, -1001, -1002
SIGNAL_CAM_BLOCKED, SIGNAL_CAM_CLEAR = -1003, -1004
SIGNAL_PIE_OPEN, SIGNAL_PIE_SELECT = -1005, -1006
SIGNAL_MENU_READY = -1007
SIGNAL_GRAB_OK, SIGNAL_GRAB_NONE = -1009, -1010
SIGNAL_RAY = -1011
_vr_menu = {'on': False, 'ref': None, 'conn': None, 'expect': 0.0}   # menu VR (só para a roda pedida pelo laser)
# Interações invisíveis que são o "parado à toa" padrão: não contam como ocupado (o corpo pode congelar).
IDLE_NAME_PARTS = ('stand_passive', 'standpassive', 'idle')
MAX_STEP = 0.8            # metros: passos maiores que isso são recusados (anti-teleporte)
FACE_MIN_ANGLE = 0.017    # radianos (~1°): a DLL manda giros pequenos e contínuos
BLOCKED_SIGNAL_EVERY = 0.2

_LOG_PATH = None
_last_busy = {}           # connection -> bool
_last_blocked_signal = 0.0
_last_running = {}
_last_cam = {}            # connection -> bool (câmera bloqueada)


def _log(msg):
    global _LOG_PATH
    try:
        if _LOG_PATH is None:
            here = os.path.dirname(os.path.abspath(__file__))
            if here.lower().endswith('.ts4script'):
                here = os.path.dirname(here)
            _LOG_PATH = os.path.join(here, 'fpc_walk.log')
        with open(_LOG_PATH, 'a') as f:
            f.write('[{:.3f}] {}\n'.format(time.time(), msg))
    except Exception:
        pass


def _active_sim(connection):
    client = services.client_manager().get(connection)
    return client.active_sim if client is not None else None


def _signal(code, connection):
    sims4.commands.client_cheat('camera.fpc.idletime {}'.format(code), connection)


_our_gohere_ids = set()          # ids das interações "Ir até aqui" pedidas pelo fpc.walk


_walk_last = [0.0]               # hora do último pedido de rota (fallback se o id não vier)


def _is_ours(si):
    if si is None:
        return False
    if getattr(si, 'id', None) in _our_gohere_ids:
        return True
    return isinstance(si, GoHereSuperInteraction) and time.time() - _walk_last[0] < 3.0


def _is_busy(sim):
    posture = sim.posture
    if posture is None or not posture.mobile:          # sentado, deitado etc.
        return True, 'postura {}'.format(posture)
    # Só conta como ocupado o que aparece na fila da interface (visible). Interações
    # invisíveis, como stand_Passive (ficar parado à toa), rodam o tempo todo.
    running = sim.queue.running
    if _is_ours(running):
        return False, '(andando pela rota do fpc.walk)'
    if running is not None:
        name = type(running).__name__.lower()
        if getattr(running, 'visible', True) or not any(p in name for p in IDLE_NAME_PARTS):
            return True, 'rodando {}'.format(running)
    for si in sim.queue.queued_super_interactions_gen():
        if _is_ours(si):
            continue
        if getattr(si, 'visible', True):
            return True, 'na fila {}'.format(si)
    return False, '' if running is None else '(ignorando invisivel {})'.format(type(running).__name__)


def _update_busy(sim, connection):
    busy, why = _is_busy(sim)
    if _last_busy.get(connection) != busy:
        _last_busy[connection] = busy
        _signal(SIGNAL_BUSY if busy else SIGNAL_FREE, connection)
        _log('estado: {} {}'.format('OCUPADO' if busy else 'livre', why))
    return busy


def _yaw_quat(fx, fz):
    return sims4.math.angle_to_yaw_quaternion(sims4.math.vector3_angle(sims4.math.Vector3(fx, 0.0, fz)))


def _ground_y(x, z, surface, fallback):
    try:
        terrain = services.terrain_service.terrain_object()
        return terrain.get_routing_surface_height_at(x, z, surface)
    except Exception:
        return fallback


def _path_clear(sim, start, end, surface):
    a = routing.Location(start, sims4.math.Quaternion.ZERO(), surface)
    b = routing.Location(end, sims4.math.Quaternion.ZERO(), surface)
    if routing.ray_test_verbose(a, b, sim.routing_context) != routing.RAYCAST_HIT_TYPE_NONE:
        return False
    return routing.test_point_placement_in_navmesh(surface, end)


PORTAL_SEARCH_RADIUS = 1.5    # metros entre o Sim e a entrada do portal
PORTAL_MIN_DOT = 0.35         # a travessia precisa estar na direção do olhar (~70°)
PORTAL_COOLDOWN = 1.5         # s entre travessias
_last_portal = 0.0


def _portal_ends(obj):
    # Pares (entrada, saída) de todos os portais do objeto (porta, escada...), nos dois sentidos.
    comp = getattr(obj, 'portal_component', None)
    if comp is None:
        return
    for inst in comp.get_portal_instances():
        for a, b in (('there_entry', 'there_exit'), ('back_entry', 'back_exit')):
            entry, exit_ = getattr(inst, a, None), getattr(inst, b, None)
            if entry is not None and exit_ is not None:
                yield entry, exit_


def _go_here(sim, position, surface, connection):
    client = services.client_manager().get(connection)
    zone = services.current_zone()
    point = TerrainPoint(sims4.math.Location(sims4.math.Transform(position), surface))
    pick = PickInfo(pick_type=PickType.PICK_TERRAIN, target=point, location=position,
                    routing_surface=surface, lot_id=getattr(zone.lot, 'lot_id', 0), level=surface.secondary_id)
    context = client.create_interaction_context(sim, pick=pick)
    for aop in point.potential_interactions(context):
        if issubclass(aop.affordance, GoHereSuperInteraction):
            return aop.test_and_execute(context)
    return None


def _try_portal(sim, fx, fz, connection):
    global _last_portal
    now = time.time()
    if now - _last_portal < PORTAL_COOLDOWN:
        return False
    pos, level = sim.position, sim.level
    best = None
    for obj in services.object_manager().values():
        op = getattr(obj, 'position', None)
        if op is None or abs(op.x - pos.x) > 4.0 or abs(op.z - pos.z) > 4.0:
            continue                                         # filtro barato antes do resto
        for entry, exit_ in _portal_ends(obj):
            ep, xp = entry.position, exit_.position
            if entry.routing_surface.secondary_id != level:
                continue
            d = ((ep.x - pos.x) ** 2 + (ep.z - pos.z) ** 2) ** 0.5
            if d > PORTAL_SEARCH_RADIUS:
                continue
            vx, vz = xp.x - pos.x, xp.z - pos.z
            vl = (vx * vx + vz * vz) ** 0.5
            if vl < 1e-3 or (vx * fx + vz * fz) / vl < PORTAL_MIN_DOT:
                continue
            if best is None or d < best[0]:
                best = (d, obj, exit_)
    if best is None:
        return False
    _, obj, exit_ = best
    result = _go_here(sim, exit_.position, exit_.routing_surface, connection)
    _log('portal {} -> saida {} nivel {}: {}'.format(type(obj).__name__, exit_.position,
                                                     exit_.routing_surface.secondary_id, result))
    if result:
        _last_portal = now
        _last_busy.pop(connection, None)                     # força reenviar o estado (vai ficar ocupado)
        _update_busy(sim, connection)
        return True
    return False


@sims4.commands.Command('fpc.body', command_type=sims4.commands.CommandType.Live)
def fpc_body(visible: int = 1, _connection=None):
    # Esconde/mostra o corpo do Sim ativo. Usa opacidade (e não visibility), porque esconder
    # via visibility chama camera.cancel_focus e mexeria na câmera.
    try:
        sim = _active_sim(_connection)
        if sim is None:
            return False
        sim.opacity = 1.0 if visible else 0.0
        _log('corpo {}'.format('visivel' if visible else 'escondido'))
        return True
    except Exception as e:
        _log('body: erro {!r}'.format(e))
    return False


@sims4.commands.Command('fpc.sync', command_type=sims4.commands.CommandType.Live)
def fpc_sync(_connection=None):
    # A DLL pede isso ao ligar o modo FPS: reenvia o estado mesmo que não tenha mudado.
    sim = _active_sim(_connection)
    _last_busy.pop(_connection, None)
    _last_cam.pop(_connection, None)
    if sim is not None:
        # Recuperação: a v8 escondia o corpo pela simulação (opacity = 0) e podia deixar o Sim
        # invisível. Agora o corpo é escondido só no cliente; aqui desfazemos o 0 antigo.
        if sim.opacity is not None and sim.opacity == 0.0:
            sim.opacity = 1.0
            _log('opacidade antiga (0) restaurada para 1')
        _update_busy(sim, _connection)
    return True


@sims4.commands.Command('fpc.move', command_type=sims4.commands.CommandType.Live)
def fpc_move(x: float = 0.0, z: float = 0.0, fx: float = 0.0, fz: float = 1.0, _connection=None):
    global _last_blocked_signal
    try:
        sim = _active_sim(_connection)
        if sim is None or _update_busy(sim, _connection):
            return False
        surface = sim.routing_surface
        start = sim.position
        dx, dz = x - start.x, z - start.z
        dist = (dx * dx + dz * dz) ** 0.5
        if dist > MAX_STEP:                                  # limita o passo
            dx, dz = dx / dist * MAX_STEP, dz / dist * MAX_STEP
        # Tenta o passo inteiro; se bater, desliza ao longo da parede (só X ou só Z).
        for sx, sz in ((dx, dz), (dx, 0.0), (0.0, dz)):
            if abs(sx) < 1e-4 and abs(sz) < 1e-4:
                continue
            tx, tz = start.x + sx, start.z + sz
            end = sims4.math.Vector3(tx, _ground_y(tx, tz, surface, start.y), tz)
            if _path_clear(sim, start, end, surface):
                sim.location = sims4.math.Location(sims4.math.Transform(end, _yaw_quat(fx, fz)), surface)
                return True
        # Bloqueado: se for uma porta/escada na direção do olhar, o roteamento do jogo atravessa.
        if _try_portal(sim, fx, fz, _connection):
            return True
        now = time.time()
        if now - _last_blocked_signal > BLOCKED_SIGNAL_EVERY:
            _last_blocked_signal = now
            _signal(SIGNAL_BLOCKED, _connection)
        return False
    except Exception as e:
        _log('move: erro {!r}'.format(e))
    return False


@sims4.commands.Command('fpc.cam', command_type=sims4.commands.CommandType.Live)
def fpc_cam(x: float = 0.0, z: float = 0.0, _connection=None):
    # Linha do Sim até a câmera (só horizontal). Avisa a DLL só quando o estado muda.
    try:
        sim = _active_sim(_connection)
        if sim is None or _update_busy(sim, _connection):
            return False
        surface = sim.routing_surface
        start = sim.position
        dx, dz = x - start.x, z - start.z
        if dx * dx + dz * dz < 0.0025:                       # câmera em cima do corpo: livre
            blocked = False
        else:
            end = sims4.math.Vector3(x, start.y, z)
            a = routing.Location(start, sims4.math.Quaternion.ZERO(), surface)
            b = routing.Location(end, sims4.math.Quaternion.ZERO(), surface)
            blocked = routing.ray_test_verbose(a, b, sim.routing_context) != routing.RAYCAST_HIT_TYPE_NONE
        if _last_cam.get(_connection) != blocked:
            _last_cam[_connection] = blocked
            _signal(SIGNAL_CAM_BLOCKED if blocked else SIGNAL_CAM_CLEAR, _connection)
        return True
    except Exception as e:
        _log('cam: erro {!r}'.format(e))
    return False


WALK_AHEAD = 2.2          # m: alvo da rota à frente (andando)
RUN_AHEAD = 3.5           # m: correndo
WALK_REISSUE = 0.45       # s mínimo entre pedidos de rota
WALK_TURN = 0.35          # rad (~20°): mudou a direção -> rota nova na hora
_walk_state = {}          # connection -> (dir_x, dir_z, t_ultimo_pedido, alvo)


def _cancel_ours(sim):
    try:
        from interactions.interaction_finisher import FinishingType
        sis = []
        if sim.queue.running is not None:
            sis.append(sim.queue.running)
        sis.extend(sim.queue.queued_super_interactions_gen())
        for si in sis:
            if _is_ours(si):
                si.cancel(FinishingType.USER_CANCEL, cancel_reason_msg='fpc.walk: parou')
    except Exception as e:
        _log('walk: erro ao parar {!r}'.format(e))
    _our_gohere_ids.clear()


@sims4.commands.Command('fpc.walk', command_type=sims4.commands.CommandType.Live)
def fpc_walk(dx: float = 0.0, dz: float = 0.0, run: int = 0, _connection=None):
    try:
        sim = _active_sim(_connection)
        if sim is None or _update_busy(sim, _connection):
            return False
        l = (dx * dx + dz * dz) ** 0.5
        if l < 1e-3:                                          # soltou: para
            _walk_state.pop(_connection, None)
            _cancel_ours(sim)
            return True
        dx, dz = dx / l, dz / l
        now = time.time()
        prev = _walk_state.get(_connection)
        pos = sim.position
        if prev is not None:
            pdx, pdz, pt, goal = prev
            turned = (pdx * dx + pdz * dz) < 0.94              # ~20°
            remaining = ((goal.x - pos.x) ** 2 + (goal.z - pos.z) ** 2) ** 0.5
            if not turned and (now - pt < WALK_REISSUE or remaining > 1.2):
                return True                                   # segue a rota atual
        surface = sim.routing_surface
        ahead = RUN_AHEAD if run else WALK_AHEAD
        goal = None
        for k in (1.0, 0.7, 0.45, 0.25):                      # encurta até cair no navmesh livre
            tx, tz = pos.x + dx * ahead * k, pos.z + dz * ahead * k
            cand = sims4.math.Vector3(tx, _ground_y(tx, tz, surface, pos.y), tz)
            if routing.test_point_placement_in_navmesh(surface, cand):
                goal = cand
                break
        if goal is None:
            _signal(SIGNAL_BLOCKED, _connection)
            return False
        result = _go_here(sim, goal, surface, _connection)
        si = getattr(result, 'interaction', None)
        if si is not None:
            _our_gohere_ids.add(si.id)
            if len(_our_gohere_ids) > 16:
                _our_gohere_ids.pop()
        _walk_state[_connection] = (dx, dz, now, goal)
        _walk_last[0] = now
        return bool(result)
    except Exception as e:
        _log('walk: erro {!r}'.format(e))
    return False


@sims4.commands.Command('fpc.face', command_type=sims4.commands.CommandType.Live)
def fpc_face(fx: float = 0.0, fz: float = 1.0, _connection=None):
    try:
        sim = _active_sim(_connection)
        if sim is None or _update_busy(sim, _connection):
            return False
        want = sims4.math.vector3_angle(sims4.math.Vector3(fx, 0.0, fz))
        have = sims4.math.yaw_quaternion_to_angle(sim.orientation)
        if sims4.math.angle_abs_difference(want, have) < FACE_MIN_ANGLE:
            return True
        sim.location = sims4.math.Location(sims4.math.Transform(sim.position, _yaw_quat(fx, fz)),
                                           sim.routing_surface)
        return True
    except Exception as e:
        _log('face: erro {!r}'.format(e))
    return False


def _menu_path():
    here = os.path.dirname(os.path.abspath(__file__))
    if here.lower().endswith('.ts4script'):
        here = os.path.dirname(here)
    return os.path.join(here, 'fpc_menu.txt')


def _tok(t):
    # Um token do texto (LocalizedStringToken) -> "s:texto", "h:hash" (a DLL traduz) ou "n:número".
    try:
        if t.first_name or t.last_name:
            return 's:' + ('{} {}'.format(t.first_name, t.last_name)).strip()
        if t.raw_text:
            return 's:' + t.raw_text
        if t.HasField('text_string') and t.text_string.hash:
            return 'h:%x' % t.text_string.hash
        if t.catalog_name_key:
            return 'h:%x' % t.catalog_name_key
        if t.HasField('number'):
            return 'n:%g' % t.number
    except Exception:
        pass
    return 's:'


def _cat_hash(key):
    try:
        import sims4.resources
        mgr = services.get_instance_manager(sims4.resources.Types.PIE_MENU_CATEGORY)
        cat = mgr.get(key) if mgr is not None else None
        dn = getattr(cat, 'display_name', None) if cat is not None else None
        if dn is None:
            return 0
        h = getattr(dn, 'hash', None)
        if h is None:
            h = getattr(dn(), 'hash', 0)
        return int(h or 0)
    except Exception:
        return 0


def _write_vr_menu(msg, reference_id):
    # Uma linha por opção: id|hash|desabilitada|categoria|hash_da_categoria|prioridade|tok;tok;...
    lines = ['ref|{}'.format(reference_id)]
    cats = {}
    for it in msg.items:
        ck = int(getattr(it, 'category_key', 0) or 0)
        if ck and ck not in cats:
            cats[ck] = _cat_hash(ck)
        toks = ';'.join(_tok(t).replace('|', '/').replace(';', ',').replace('\n', ' ') for t in it.loc_string.tokens)
        dis = 1 if it.HasField('disabled_text') else 0
        lines.append('{}|{:x}|{}|{}|{:x}|{}|{}'.format(int(it.id), int(it.loc_string.hash), dis, ck, cats.get(ck, 0),
                                                     int(getattr(it, 'pie_menu_priority', 0) or 0), toks))
    if len(lines) <= 1:
        return False
    with open(_menu_path(), 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines))
    _log('menu VR: {} opcoes gravadas'.format(len(lines) - 1))
    return True


@sims4.commands.Command('fpc.vrmenu', command_type=sims4.commands.CommandType.Live)
def fpc_vrmenu(on: int = 1, _connection=None):
    _vr_menu['on'] = bool(on)
    _vr_menu['conn'] = _connection
    return True


@sims4.commands.Command('fpc.select', command_type=sims4.commands.CommandType.Live)
def fpc_select(choice_id: int = 0, _connection=None):
    # Escolha no menu VR: o mesmo caminho do jogo (select_choice com a referência da roda).
    try:
        import server_commands.interaction_commands as ic
        ref, menu = _vr_menu.get('ref'), _vr_menu.get('menu')
        client = services.client_manager().get(_connection) if _connection is not None else None
        if client is None:
            client = services.client_manager().get_first_client()
        if choice_id and ref is not None and client is not None:
            if client.choice_menu is not menu and menu is not None:
                # a roda vazia que o cliente recebeu pode ter limpado o menu do servidor: devolve o nosso
                _log('menu VR: menu do servidor era {!r}, restaurando'.format(client.choice_menu))
                try:
                    client.choice_menu = menu
                except Exception as e2:
                    _log('menu VR: nao deu para restaurar {!r}'.format(e2))
            ok = client.select_interaction(choice_id, ref)
            _log('menu VR: escolhida a opcao {} (revisao {}) -> {!r}'.format(choice_id, ref, ok))
            _vr_menu['menu'] = None
            return True
    except Exception as e:
        _log('menu VR: erro ao escolher {!r}'.format(e))
    return False


def _mods_file(name):
    here = os.path.dirname(os.path.abspath(__file__))
    if here.lower().endswith('.ts4script'):
        here = os.path.dirname(here)
    return os.path.join(here, name)


# ---------------------------------------------------------------- Fase 4: painel de relógio (estado do Sim)
MOTIVES = (('hunger', 'Fome'), ('energy', 'Energia'), ('bladder', 'Bexiga'), ('hygiene', 'Higiene'),
           ('social', 'Social'), ('fun', 'Diversao'))


@sims4.commands.Command('fpc.status', command_type=sims4.commands.CommandType.Live)
def fpc_status(_connection=None):
    # Grava fpc_status.txt (UTF-8): chave=valor por linha. A DLL desenha o painel do pulso.
    try:
        sim = _active_sim(_connection)
        if sim is None:
            return False
        out = []
        out.append('nome={} {}'.format(sim.first_name, sim.last_name))
        try:
            mood = sim.get_mood()
            dn = getattr(mood, 'display_name', None) if mood is not None else None
            h = getattr(dn, 'hash', None) if dn is not None else None
            if h is None and dn is not None:
                h = getattr(dn(), 'hash', 0)
            out.append('humor_hash={:x}'.format(int(h or 0)))
            out.append('humor_intensidade={}'.format(int(sim.get_mood_intensity() or 0)))
        except Exception:
            pass
        try:
            now = services.time_service().sim_now
            out.append('hora={:02d}:{:02d}'.format(int(now.hour()), int(now.minute())))
            out.append('dia={}'.format(int(now.day())))
        except Exception:
            pass
        try:
            out.append('dinheiro={}'.format(int(sim.family_funds.money)))
        except Exception:
            pass
        found = {}
        try:
            for stat in sim.commodity_tracker:
                name = type(stat).__name__.lower()
                for key, label in MOTIVES:
                    if name.startswith('motive_') and key in name and key not in found:
                        lo, hi = stat.min_value, stat.max_value
                        v = (stat.get_value() - lo) / float(hi - lo) if hi > lo else 0.0
                        found[key] = max(0.0, min(1.0, v))
        except Exception as e:
            _log('status: necessidades {!r}'.format(e))
        for key, label in MOTIVES:
            if key in found:
                out.append('nec_{}={:.3f}'.format(label, found[key]))
        try:
            run = sim.queue.running
            if run is not None and getattr(run, 'visible', True):
                out.append('fazendo={}'.format(type(run).__name__))
        except Exception:
            pass
        with open(_mods_file('fpc_status.txt'), 'w', encoding='utf-8') as f:
            f.write('\n'.join(out))
        return True
    except Exception as e:
        _log('status: erro {!r}'.format(e))
    return False


# ---------------------------------------------------------------- Fase 5: agarrar objetos
GRAB_RADIUS = 0.30
_held = {}               # connection -> (obj_id, transform original, superficie)


def _grabbable(obj):
    if obj is None or getattr(obj, 'is_sim', False):
        return False
    try:
        from objects.components.types import CARRYABLE_COMPONENT
        if obj.has_component(CARRYABLE_COMPONENT):
            return True
    except Exception:
        pass
    try:
        fp = obj.footprint_polygon
        if fp is None:
            return True
        return abs(fp.area()) < 0.20                       # objetos pequenos (decoração, pratos, livros...)
    except Exception:
        return False


@sims4.commands.Command('fpc.grabobj', command_type=sims4.commands.CommandType.Live)
def fpc_grabobj(x: float = 0.0, y: float = 0.0, z: float = 0.0, _connection=None):
    try:
        sim = _active_sim(_connection)
        if sim is None:
            return False
        best, bd = None, GRAB_RADIUS
        for obj in services.object_manager().values():
            p = getattr(obj, 'position', None)
            if p is None or abs(p.x - x) > 1.0 or abs(p.z - z) > 1.0:
                continue
            d = ((p.x - x) ** 2 + (p.y - y) ** 2 + (p.z - z) ** 2) ** 0.5
            if d < bd and _grabbable(obj):
                best, bd = obj, d
        if best is None:
            _signal(SIGNAL_GRAB_NONE, _connection)
            return False
        _held[_connection] = (best.id, best.transform, best.routing_surface)
        with open(_mods_file('fpc_grab.txt'), 'w', encoding='utf-8') as f:
            f.write('{}'.format(best.id))
        _log('agarrar: objeto {} a {:.2f} m'.format(type(best).__name__, bd))
        _signal(SIGNAL_GRAB_OK, _connection)
        return True
    except Exception as e:
        _log('agarrar: erro {!r}'.format(e))
    return False


@sims4.commands.Command('fpc.dropobj', command_type=sims4.commands.CommandType.Live)
def fpc_dropobj(x: float = 0.0, y: float = 0.0, z: float = 0.0, qy: float = 0.0, qw: float = 1.0, _connection=None):
    # Soltou: o objeto cai no chão do andar do Sim, no ponto (x, z), com o giro em volta do eixo vertical.
    try:
        held = _held.pop(_connection, None)
        if held is None:
            return False
        obj = services.object_manager().get(held[0])
        sim = _active_sim(_connection)
        if obj is None or sim is None:
            return False
        surface = sim.routing_surface
        gy = _ground_y(x, z, surface, sim.position.y)
        pos = sims4.math.Vector3(x, gy, z)                    # cai no chão do andar (sem física: pousa direto)
        orient = sims4.math.Quaternion(0.0, qy, 0.0, qw)
        try:
            obj.location = sims4.math.Location(sims4.math.Transform(pos, orient), surface)
        except Exception as e:
            _log('soltar: posicao recusada ({!r}), voltando'.format(e))
            obj.location = sims4.math.Location(held[1], held[2])
        return True
    except Exception as e:
        _log('soltar: erro {!r}'.format(e))
    return False


# ---------------------------------------------------------------- Fase 3b: raycast do laser (próprio)
SIM_RADIUS, SIM_HEIGHT = 0.32, 1.85
OBJ_HEIGHT = 1.1           # altura do prisma sobre o "pé" (footprint) dos objetos
SMALL_RADIUS = 0.18        # objetos sem footprint (decoração, coisas em cima de mesas)
_ray_ref = [7000000]


def _dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _ray_cylinder(o, d, c, r, y0, y1):
    # raio x cilindro vertical (centro c, raio r, de y0 a y1); devolve t ou None
    ox, oz = o[0] - c[0], o[2] - c[2]
    a = d[0] * d[0] + d[2] * d[2]
    best = None
    if a > 1e-8:
        b = 2 * (ox * d[0] + oz * d[2]); cc = ox * ox + oz * oz - r * r
        disc = b * b - 4 * a * cc
        if disc >= 0:
            for t in ((-b - disc ** 0.5) / (2 * a), (-b + disc ** 0.5) / (2 * a)):
                y = o[1] + d[1] * t
                if t > 0 and y0 <= y <= y1:
                    best = t if best is None else min(best, t)
                    break
    for yp in (y1, y0):                                   # tampas
        if abs(d[1]) > 1e-6:
            t = (yp - o[1]) / d[1]
            if t > 0:
                px, pz = o[0] + d[0] * t - c[0], o[2] + d[2] * t - c[2]
                if px * px + pz * pz <= r * r:
                    best = t if best is None else min(best, t)
    return best


def _inside(poly, x, z):
    n = len(poly); inside = False; j = n - 1
    for i in range(n):
        xi, zi = poly[i]; xj, zj = poly[j]
        if ((zi > z) != (zj > z)) and (x < (xj - xi) * (z - zi) / ((zj - zi) or 1e-9) + xi):
            inside = not inside
        j = i
    return inside


def _ray_prism(o, d, poly, y0, y1, tmax):
    # raio x prisma (polígono no chão, de y0 a y1): marcha fina + refinamento (polígonos pequenos)
    steps = 160; prev = None
    for k in range(steps + 1):
        t = tmax * k / steps
        x, y, z = o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t
        hit = y0 <= y <= y1 and _inside(poly, x, z)
        if hit:
            if prev is None:
                return t
            lo, hi = prev, t
            for _ in range(10):
                m = (lo + hi) / 2
                xm, ym, zm = o[0] + d[0] * m, o[1] + d[1] * m, o[2] + d[2] * m
                if y0 <= ym <= y1 and _inside(poly, xm, zm): hi = m
                else: lo = m
            return hi
        prev = t
    return None


def _poly_of(obj):
    try:
        fp = obj.footprint_polygon
        if fp is None:
            return None
        pts = []
        for v in fp:
            pts.append((v.x, v.z if hasattr(v, 'z') else v.y))
        return pts if len(pts) >= 3 else None
    except Exception:
        return None


def _ray_pick(sim, o, d, maxd=10.0):
    best, bt, btype = None, maxd, 'nada'
    for obj in services.object_manager().values():
        if obj is sim:
            continue
        p = getattr(obj, 'position', None)
        if p is None:
            continue
        v = (p.x - o[0], p.y - o[1], p.z - o[2])
        t0 = _dot(v, d)
        if t0 < -1.0 or t0 > maxd + 1.5:
            continue
        perp = (v[0] - d[0] * t0, v[1] - d[1] * t0, v[2] - d[2] * t0)
        if _dot(perp, perp) > 6.25:                           # mais de 2,5 m do raio: nem testa
            continue
        t = None
        try:
            if getattr(obj, 'is_sim', False):
                t = _ray_cylinder(o, d, (p.x, p.y, p.z), SIM_RADIUS, p.y, p.y + SIM_HEIGHT); kind = 'sim'
            else:
                poly = _poly_of(obj)
                if poly is not None and getattr(obj, 'parent', None) is None:
                    t = _ray_prism(o, d, poly, p.y - 0.02, p.y + OBJ_HEIGHT, min(bt, maxd)); kind = 'objeto'
                else:
                    t = _ray_cylinder(o, d, (p.x, p.y, p.z), SMALL_RADIUS, p.y - 0.05, p.y + 0.35); kind = 'objeto'
        except Exception:
            t = None
        if t is not None and 0 < t < bt:
            best, bt, btype = obj, t, kind
    # chão do andar do Sim
    if abs(d[1]) > 1e-4:
        tf = (sim.position.y - o[1]) / d[1]
        if 0 < tf < bt:
            return None, tf, 'chao'
    return best, bt, btype


@sims4.commands.Command('fpc.ray', command_type=sims4.commands.CommandType.Live)
def fpc_ray(ox: float = 0.0, oy: float = 0.0, oz: float = 0.0, dx: float = 0.0, dy: float = 0.0, dz: float = 1.0, _connection=None):
    try:
        sim = _active_sim(_connection)
        if sim is None:
            return False
        l = (dx * dx + dy * dy + dz * dz) ** 0.5 or 1.0
        o, d = (ox, oy, oz), (dx / l, dy / l, dz / l)
        obj, t, kind = _ray_pick(sim, o, d)
        hx, hy, hz = o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t
        oid = obj.id if obj is not None else 0
        with open(_mods_file('fpc_hit.txt'), 'w', encoding='utf-8') as f:
            f.write('{} {} {:.4f} {:.4f} {:.4f}'.format(oid, kind, hx, hy, hz))
        _signal(SIGNAL_RAY, _connection)
        return True
    except Exception as e:
        _log('ray: erro {!r}'.format(e))
    return False


@sims4.commands.Command('fpc.choices', command_type=sims4.commands.CommandType.Live)
def fpc_choices(target_id: int = 0, kind: str = 'objeto', x: float = 0.0, y: float = 0.0, z: float = 0.0, _connection=None):
    # Gatilho do laser num alvo: o servidor monta as opções desse alvo (o mesmo generate_choices do clique),
    # e a roda que sai vai para o menu VR.
    try:
        import server_commands.interaction_commands as ic
        from server.pick_info import PickType
        sim = _active_sim(_connection)
        if sim is None:
            return False
        if kind == 'sim':
            pt = PickType.PICK_SIM
        elif kind == 'chao':
            pt = PickType.PICK_TERRAIN
            target_id = services.terrain_service.terrain_object().id
        else:
            pt = PickType.PICK_OBJECT
        zone = services.current_zone()
        lot_id = getattr(zone.lot, 'lot_id', 0)
        _ray_ref[0] += 1
        _vr_menu['expect'] = time.time()
        ic.generate_choices(target_id, pt, x, y, z, lot_id, sim.level, False, False, False, _ray_ref[0],
                            _connection=_connection)
        return True
    except Exception as e:
        _log('choices: erro {!r}'.format(e))
    return False


def _install_pie_hooks():
    # create_pie_menu_message é global de server_commands.interaction_commands (generate_choices a busca
    # na hora); Client.select_interaction é método de classe. Os dois podem ser embrulhados com segurança.
    try:
        import server_commands.interaction_commands as ic
        import server.client as sc
        if not getattr(ic.create_pie_menu_message, '_fpc', False):
            orig = ic.create_pie_menu_message
            def create_pie_menu_message(sim, choice_menu, reference_id, *args, **kwargs):
                msg = orig(sim, choice_menu, reference_id, *args, **kwargs)
                try:
                    client = services.client_manager().get_first_client()
                    if client is not None and client.active_sim is sim:
                        laser = time.time() - _vr_menu.get('expect', 0.0) < 3.0
                        if _vr_menu['on'] and laser and msg is not None and _write_vr_menu(msg, reference_id):
                            _vr_menu['expect'] = 0.0
                            # select_interaction exige a revisão do menu do servidor (msg.server_reference_id),
                            # não a referência do cliente
                            _vr_menu['ref'] = choice_menu.revision
                            _vr_menu['menu'] = choice_menu
                            del msg.items[:]                     # a roda do Scaleform não aparece (o menu é nosso)
                            _signal(SIGNAL_MENU_READY, client.id)
                        else:
                            _signal(SIGNAL_PIE_OPEN, client.id)
                except Exception as e:
                    _log('pie: erro no aviso {!r}'.format(e))
                return msg
            create_pie_menu_message._fpc = True
            ic.create_pie_menu_message = create_pie_menu_message
        if not getattr(sc.Client.select_interaction, '_fpc', False):
            orig_sel = sc.Client.select_interaction
            def select_interaction(self, choice_id, *args, **kwargs):
                try:
                    _signal(SIGNAL_PIE_SELECT, self.id)
                except Exception:
                    pass
                return orig_sel(self, choice_id, *args, **kwargs)
            select_interaction._fpc = True
            sc.Client.select_interaction = select_interaction
        _log('pie: avisos da roda instalados')
    except Exception as e:
        _log('pie: nao consegui instalar os avisos da roda: {!r}'.format(e))


_install_pie_hooks()
_log('fpc_walk v9 carregado (menu VR escolhe com a revisao do servidor)')
