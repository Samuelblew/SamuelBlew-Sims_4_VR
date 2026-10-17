# Pipeline de animação do Sim (cliente): TS4_x64.exe 1.118.257.1020

Base para: corpo congelado (feito na v11), IK dos braços (VR) e desligar o "olhar" da cabeça.
Pseudocódigo em `descompilado/animacao/`. Assinaturas em `assinaturas_AOB.txt`.
Legenda: **[O]** observado · **[I]** inferido.

## 1. Ator de animação

| O quê | Como | |
|---|---|---|
| Ator do Sim | `GetAnimActorById(&simId)` = `0x140892fb0` (o `simId` da FPC está em `Impl+0xf0`) | [O] |
| Componentes do ator | lista `ator+0xb0 .. ator+0xb8`; cada um responde `vf[1](hashDoTipo)` | [O] |
| Lock do ator | `Actor_Lock(&lock, ator, modo)` `0x140892120` / `Actor_Unlock(&lock)` `0x1408926a0` (seção crítica em `ator+0xc8`) | [O] |
| Pose | `Actor_GetPose(ator, 0)` = `0x140893340` | [O] |
| Juntas | `Pose_GetJoints(pose)` = `0x1408c7300`; buffer interno em `pose+0x60` (até 200 juntas) ou externo em `pose+0x38` | [O] |
| Nº de juntas | `uint16` em `pose+0x5e` | [O] |
| Formato da junta | **0x30 bytes:** posição (vec4) · rotação quaternion (vec4) · escala (vec4), **local (relativa ao pai)** | [O] formato · [I] espaço local |
| Osso → índice | `HashName("b__L_Hand__")` = `0x14089a8d0`; `FindBone(rig+0x48, hash)` = `0x14089a6f0` | [O] |
| Osso no mundo | `ator->vf[0x48](ator, índice, &transformação)` (usado pela FPC para os olhos) | [O] |

## 2. Ordem dos "scene processors" (por frame, por ator)

Registrados em `0x14093c7b0`, nesta ordem [O]; a ordem de execução segue o registro [I]:

| # | Processador | Process (slot 1) | Observação |
|---|---|---|---|
| 1 | BeginUpdate | — | |
| 2 | AnimatedPortal | | portas animadas |
| 3 | Awareness | `0x1408aca20` | |
| 4 | (vtable `0x141fab738`) | | desconhecido |
| 5 | EndUpdate | — | |
| 6 | **EvaluateControllers** | `0x1418ed330` | **avalia a animação** (árvore de nós: Clip, Blend, Layers, IK, LookAt…) |
| 7 | ApplyRootMotion | `0x1418ed780` | |
| 8 | FocusScoring | `0x1408a7d30` | escolhe para onde olhar |
| 9 | Terrain | | alinhamento ao chão |
| 10 | **ApplyBoneDeltas** | `0x1418ec6a0` | soma deltas por junta (sliders do CAS) |
| 11 | LipSync | `0x1408be600` | |
| 12 | **IkTargetOverrides** | `0x1418ef2e0` | reescreve juntas a partir de parâmetros do ator (tipos `0x1a`, `0x1e`…) |
| 13 | RootPoseIk | | |
| 14 | (vtable `0x141c64328`) | | desconhecido |
| 15 | **FocusPosing** | `0x1408a7c70` | **vira a cabeça para o foco** (o "pescoço" da FPC original) |
| 16 | **LimbPoseIk** | `0x1418f02b0` | IK de membros |
| 17 | ApplyBoneMultipliers | `0x1418ec820` | |
| 18 | (vtable `0x141fab920`) | | desconhecido |
| 19 | **EndPosingState** | `0x1418ed280` | **finaliza a pose**; roda para todo ator |

Categorias de ator recebem subconjuntos (`"Sim"`, `"Object"`, `"Door"`, `"Procedural"`, `"Creature"`). Os processadores 1, 5–7, 18 e 19 não aparecem nas listas de categoria; devem rodar para todos [I].

## 3. Ponto de entrada escolhido: antes do `EndPosingState`

O hook `hk_EndPosing` (v11) roda antes da finalização, só para o ator do nosso Sim, com o lock do ator:
- **corpo congelado:** foto das juntas ao entrar no modo livre, reaplicada todo frame. ✅ implementado
- **IK dos braços (VR):** reescrever `UpperArm`, `Forearm` e `Hand` (+ `ForearmTwist`/`ShoulderTwist`) depois da animação.
- **cabeça sem "olhar":** opcional, pular o `FocusPosing` só para o nosso Sim.

## 4. IK dos braços: como fazer (plano B confirmado como viável)

O `IkTargetOverrides` do jogo depende de parâmetros enviados pela simulação, então é mais simples calcular o IK na DLL:

1. **Ombro no mundo:** `ator->vf[0x48]` para `b__L_UpperArm__` (ou cinemática direta pela cadeia `ROOT > ROOT_bind > Spine0 > Spine1 > Spine2 > Clavicle`, com as juntas locais da pose).
2. **Alvo:** pose do controle (VR), convertida para o espaço do ombro; limitada a 0,61 m (0,321 + 0,287, do `.blend`).
3. **Cotovelo:** lei dos cossenos; plano do cotovelo orientado pelo `ArmExportPole` (atrás e abaixo).
4. **Escrita:** rotações locais de `UpperArm` e `Forearm` (quaternions) na pose; rotação da `Hand` = controle; ~50% da torção para `ForearmTwist`.
5. **Dedos:** curvar `Index/Mid/Ring/Pinky 0–2` pelo gatilho/grip.

Protótipo no monitor (sem VR): mão direita apontando para onde a câmera olha.

## 5. Ainda não confirmado

| Item | Como confirmar |
|---|---|
| O `EndPosingState` recebe mesmo o ator do nosso Sim | log `pose: EndPosingState recebeu o ator do nosso Sim` (v11) |
| As juntas estão em espaço local | o congelamento funcionar sem o corpo "explodir" já indica; o IK confirma |
| Ordem de execução = ordem de registro | log de chamadas por frame, se precisar |

## 6. V6 implementada (v18): o Sim segue o VR

Código: `exemplo_hook/xr_input.inl` (controles OpenXR) e `exemplo_hook/vr_rig.inl` (IK).

- **Osso -> índice:** `FindBone(rig+0x48, hash)` confirmado no disassembly: `rcx` = esqueleto, `edx` = hash; procura numa tabela de 13 baldes (hash na entrada `+0x5c`, índice em `+0x50`); `0xffffffff` = não achou. Entradas do esqueleto: 0x70 bytes (`FUN_14089a840` = contagem). [O]
- **Mundo do osso:** `ator->vf[0x48](ator, idx, &Xf)` devolve 0x30 bytes alinhados: posição · quaternion (x,y,z,w) · escala. [O]
- **Threads:** a thread principal lê o mundo dos pais fixos (Spine2, Clavicles) e os alvos; o `EndPosingState` faz FK com as juntas locais do frame e escreve as rotações locais.
- **Direções locais medidas nas juntas** (sem supor eixos): dedos = posição local de `Mid0`; polegar = `Index0 - Pinky0`; frente da cabeça = `CAS_Glasses - meio(EyeAreas)`; esquerda = `L_EyeArea - R_EyeArea`.
- **Cotovelo:** o eixo da dobra é aprendido da própria animação (no espaço do UpperArm) e levado ao plano escolhido pelo polo (baixo + fora + trás).
- **A confirmar no teste:** sinal da curvatura dos dedos (`vr_dedos_sinal`) e o ângulo da mão no controle (`vr_mao_inclinacao`).

## 7. Correção (v20): o ponto de escrita certo é o Pose_Commit

- `Actor_GetPose` devolve a **pose de trabalho** em `ator+0x380` (objeto de 0x3d170 bytes, 200 juntas em `+0x60`). [O]
- O processador **#18** (vtable `0x141fab920`, Process `0x1418ed9f0`) pega o lock do ator (modo 0xa) e chama **`Pose_Commit` = `0x140894080(ator, pose)`**, que copia as juntas para a **pose definitiva** `ator+0xf0` (vetor de juntas de 0x30 bytes). [O]
- Depois, o **`EndPosingState`** chama `0x140893ca0`, que **solta a pose de trabalho** (`ator+0x380 = 0`). [O]
- Então tudo o que era escrito no `EndPosingState` (congelar corpo até a v19, rig VR da v18/v19) **era descartado**. Agora o hook fica no `Pose_Commit`, antes da cópia.
- A rotação devolvida por `ator->vf[0x48]` **não** está na convenção das juntas locais (o ombro saía 14 cm acima/abaixo da clavícula). O rig usa só as **posições** do `vf[0x48]` e monta as rotações pela FK desde `b__ROOT__` (ROOT > ROOT_bind > Spine0 > Spine1 > Spine2), encaixando o modelo no mundo pelas posições das clavículas, do pescoço e do Spine2.
- Assinatura: `Pose_Commit 140894080  48 85 D2 0F 84 C8 00 00 00 48 89 5C 24 10 57 48 83 EC 20 48 8B 01 48 8B FA 48 8B D9`.
