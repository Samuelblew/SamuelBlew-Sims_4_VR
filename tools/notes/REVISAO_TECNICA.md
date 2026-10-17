# Revisão técnica do TS4 VR / primeira pessoa (v36)

Base: TS4_x64.exe 1.118.257.1020 decompilado (Ghidra, sem símbolos) + Python do jogo (bytecode 3.7).
Não existe código-fonte do jogo disponível; tudo abaixo vem do binário, dos filmes Scaleform e do Python.

Legenda: **Motor** = usa o mecanismo do próprio jogo · **Contorno** = funciona, mas por fora do jogo.

| Subsistema | Como está | Tipo | Evidência |
|---|---|---|---|
| Câmera FPC | hook em `FPS_Update`/`SetPose`; âncora no `b__Spine2__` via `camera.fpc.set_joint` do jogo | Motor | MAPA_CAMERA.md |
| Estéreo | `R_RenderView` chama `FrameA/B` duas vezes, um olho cada, com pose/FOV do OpenXR | Motor | RENDER_V3.md |
| Head tracking / recentrar | OpenXR → câmera; yaw do corpo pelo mouse/analógico | Motor | |
| Controles | OpenXR actions (perfil Touch) | Motor | xr_input.inl |
| Braços/mãos/cabeça (IK) | escrita nas juntas antes do `Pose_Commit` (`0x140894080`), FK pela raiz | Motor | testado fora do jogo (teste/teste_rig.cpp) |
| **Locomoção** | **v36: rota do jogo** (`fpc.walk` → "Ir até aqui" com alvo à frente); o cliente anima a rota | Motor | antes: teleporte em passos (Contorno) |
| Portas/escadas | o roteamento do jogo atravessa portais | Motor | antes: detecção manual de portais |
| **Animação do corpo** | **v36: natural** (sem foto congelada); VR sobrepõe só braços/cabeça | Motor | antes: foto congelada (Contorno, quebrava) |
| Ocupado/livre | Python: interação rodando ≠ idle padrão; o nosso "Ir até aqui" não conta | Motor | fpc_walk.py |
| Interface separada | `Scaleform::Render::Sims4::HAL` BeginScene/EndScene → alvo próprio | Motor | UI_VR.md |
| Interface no mundo | textura num plano que reproduz a câmera do jogo (pose + frustum) | Motor (mapeamento exato) | |
| Telinha na mão | camada quad OpenXR presa ao grip | Motor | |
| Seleção pelo laser | câmera do jogo no raio do laser; escolha pelo centro como o jogo faz | Motor | |
| **Clique no mundo** | **v36: `VC_Click` do cursor virtual** (Canvas vtable+0x240 → `FUN_140153380`) | Motor | antes: `SendInput` |
| Cursor virtual | hooks `FUN_140154080` (posição) e `VC_LockSet`; centro com grip, fora da tela sem grip | Motor (estado do jogo) | |
| Roda de opções | avisada pelo Python (`create_pie_menu_message`, `Client.select_interaction`) | Motor | |
| Clique na interface (telinha/roda) | cursor do Windows + `SendInput` | **Contorno** | próximo item |
| Sonda de parede (cabeça) | Python `routing.ray_test_verbose` Sim → câmera | Motor (navmesh) | |
| Mira/ícones do cursor virtual | mod de interface (`FPC_SemMira.package`, 41 formas vazias) | Mod de dados | |

## Próximos passos (em ordem)

1. **Clique na interface pelo Scaleform**: injetar eventos de mouse direto no filme (`GFx::Movie::HandleEvent` /
   `NotifyMouseState` do `S4MovieImpl`, vtable `0x141cb5a48`) em vez do cursor do Windows.
2. **Correr pela rota**: pedido de estilo de andar (walkstyle) do jogo em vez de só alvo mais longe.
3. **Virar o corpo parado**: hoje `fpc.face` (giro instantâneo acima de ~50° em VR); trocar pela transição de
   giro do jogo (rota curta de orientação).
4. **Corpo seguir a cabeça (agachar/inclinar)**: IK de coluna/quadril com a altura e o deslocamento do headset.
5. **Interface no mundo**: separar HUD de elementos presos a objetos dentro do `UIContainer.gfx`
   (hoje é por região da tela).

## O que cada log mostra

- `fpc_rig.log` (pasta Bin): `camera do jogo: ...`, `roda de opcoes: ...`, `laser: clique ...`, `cursor virtual: ...`, `rig: ...`.
- `fpc_walk.log` (pasta Mods): `estado: ...`, `walk: ...`, `pie: ...`.

## Agarrar outros Sims (v39), modelo HIGGS / PLANCK

Referência: `Mods Ref/HIGGS 1.10.10` (`higgs_vr.ini`) e `Mods Ref/PLANCK 0.8.2` (`activeragdoll.ini`), Skyrim VR.

| HIGGS / PLANCK (Skyrim, Havok) | TS4 (sem física de ragdoll para Sims) |
|---|---|
| `NearCastRadius/Distance`: esfera a partir da palma escolhe o rigidbody | esfera da palma (`agarrar_raio` 0,14 m) até mão/antebraço/cabeça dos outros Sims; ossos pelo `vf[0x48]` |
| restrição com motor mole; `grabConstraint*TauActor` para NPCs vivos | restrição cinemática: deslocamento mão→osso guardado no agarrar, alvo com mola (`agarrar_mola` 0,12 s), IK no membro dele |
| ragdoll ativo: mistura animação ↔ física (`blendIn`) | mistura animação ↔ pose agarrada (entrada/saída 0,25 s), escrita no `Pose_Commit` do outro ator |
| quebra quando puxa demais | solta se a mão passar de 1,1 m do ombro dele; o braço dele nunca passa do alcance |
| vibração ao agarrar/soltar | `xrApplyHapticFeedback` (ação `output/haptic`) |
| prioridade sobre o gatilho/grip normal | perto de alguém, o grip agarra em vez de abrir laser/telinha |
| agressividade/falas (PLANCK) | não feito ainda: próximo passo seria uma reação social pelo Python após X s agarrando |

Testado fora do jogo (`teste/teste_rig.cpp`): o pulso do outro Sim segue a mão (erro 0,1 mm) e para no alcance do braço (0,608 m).
Só visual (cliente): a simulação continua mandando no outro Sim (posição, interações).
