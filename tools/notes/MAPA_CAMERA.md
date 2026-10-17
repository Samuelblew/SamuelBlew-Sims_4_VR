# Mapa do sistema de câmera: TS4_x64.exe 1.118.257.1020

Base da imagem: `0x140000000`. **RVA = endereço − 0x140000000.**
As assinaturas de bytes (para achar as funções em outras versões) estão em
`assinaturas_AOB.txt`.

Legenda: **[O]** observado no binário · **[I]** inferido do código, não testado no jogo · **[?]** desconhecido.

---

## 1. Hierarquia de classes (RTTI) [O]

```
CameraControllerService      (serviço do cliente; nome "CameraController")
└─ CameraControllerManager   vtable 0x141b2ba58
     +0x20  modo ativo (0 = Main, 1 = FPS)                          [I]
     +0x30  CameraControllerMain*                                   [O]
     +0x38  CameraControllerFPS*                                    [O]

CameraController (base abstrata, 26 slots)   vtable 0x141b3ee60
├─ CameraControllerMain          0x141b29158   (câmera normal do jogo)
├─ CameraControllerFPS           0x141b40b50   (primeira pessoa)
│    └─ CameraControllerFPSImpl  0x141b40cc8   (lógica, em FPS+0x28)
├─ CameraControllerCAS           0x141b3fe28
├─ CameraControllerPhotography   0x141b416d8
└─ CameraControllerVideo         0x141b3d650
RemoveHeadFixupLOD               0x141b40c80   (esconde a cabeça em FPC)
```

### Campos comuns de `CameraController` [O]
| Offset | Conteúdo |
|---|---|
| `+0x18` | **ponteiro para a câmera do engine** (ver §3) |
| `+0x20` | FOV (float; slot 15 lê, slot 14 grava e reaplica) |
| `+0x24` | flag (slot 6 grava) |
| `+0x28` | (FPS) ponteiro para `CameraControllerFPSImpl` |

### Slots da vtable de `CameraControllerFPS` [O]
| Slot | Offset | Função | O que faz |
|---|---|---|---|
| 4 | `+0x20` | `0x14020cd00` | carrega as opções `FP*` do `Options.ini` |
| 5 | `+0x28` | `0x14020d770` | ativa e escolhe o Sim |
| 7 | `+0x38` | **`0x14020f5d0`** | **Update por frame** `(this, float dt?, char)` |
| 8 | `+0x40` | `0x14020d730` | desativa e solta o osso |
| 12 | `+0x60` | `0x14020c5c0` | GetTarget = `pos + forward × distância` |
| 13 | `+0x68` | `0x14020c5b0` | GetPosition = `Impl+0x70` |
| 25 | `+0xc8` | `0x140202300` | aplica o FOV na câmera do engine |

---

## 2. `CameraControllerFPSImpl` (em `FPS+0x28`)

| Offset | Conteúdo | |
|---|---|---|
| `+0x70` | **posição final** da câmera (vec4) | [O] |
| `+0x80` | vetor **up** | [O] |
| `+0x90` | vetor **forward** | [O] |
| `+0xa0` | distância de foco (usada no FOV/zoom) | [O] |
| `+0xb0…` | subobjeto de fixação: `+0x40` = id do Sim, `+0x4c/+0x50` = osso, `+0x60/+0x70/+0x80` = offset, forward e up locais | [O] |
| `+0xf0` | id do objeto/Sim seguido | [O] |
| `+0x130` | orientação de referência para a mistura com os olhos | [O] |
| `+0x190`, `+0x1a0`, `+0x210` | filtros de suavização (posição, direção, up) | [I] |

### Fluxo por frame [O]

```
CameraControllerFPS::Update  0x14020f5d0   (vtable slot 7)
 ├ 0x141164b00(cam, nearFarParams)               define near/far
 ├ Impl::UpdatePose  0x14020f690(impl, dt, flag, cam)
 │   ├ 0x14020ee40  EyeBlend
 │   │    ├ 0x14020f290  AttachTransform: osso + offset → +0x70/+0x80/+0x90
 │   │    └ se eye_weight > 0: forward += (média L_Eye/R_Eye − forward) × eye_weight
 │   ├ 0x14020fb70  suavização (+0x190)
 │   ├ ★ 0x1411647a0  EngineCamera::SetPose(cam, &pos, &up, &fwd)  → chama Commit
 │   ├ 0x14020f9c0  suavização (+0x1a0)
 │   ├ (simple mode) força up = 0x141afc200
 │   ├ 0x14020b770  filtro (+0x210)
 │   └ FOV: fov = fov_zoom_ratio × distância, limitado por fov_zoom_max
 ├ 0x141164cd0 / 0x141164c40   vetores auxiliares (+0x260 / +0x240)
 └ ★ 0x141163ee0  EngineCamera::Commit(cam, 1, NULL)
```

---

## 3. Câmera do engine (o objeto em `controller+0x18`)

| Offset | Conteúdo | |
|---|---|---|
| `+0x14` `+0x18` `+0x1c` | parâmetros do FOV (gravados por `SetFOV`) | [O] |
| `+0x20` | aspect ratio | [O] |
| `+0x24` `+0x28` | near / far (interpolados pela distância do alvo) | [O] |
| `+0x30` `+0x40` `+0x50` | base de rotação da câmera no mundo | [O] gravados por `SetPose` · [I] ordem dos eixos |
| **`+0x60`** | **posição da câmera no mundo** | [O] |
| **`+0x70…+0xac`** | **matriz de projeção** | [I] |
| **`+0xf0…+0x12c`** | **matriz de view** | [O] |
| `+0x130…+0x16c` | inversa (view×proj ou view) | [I] |
| **`+0x170…+0x1ac`** | **view × projeção** | [O] |
| `+0x1b0…+0x1ec` | outra inversa | [I] |
| `+0x200` | alvo (look-at) | [O] |
| `+0x290` | subsistema de culling/visibilidade | [I] |
| `+0x610` | planos do frustum | [I] |
| `+0x6b0…+0x6e8` | parâmetros de near/far por distância | [O] |

### Funções da câmera do engine [O]

| Função | Endereço | Assinatura |
|---|---|---|
| **SetPose** | `0x1411647a0` | `void(cam, const vec4* pos, const vec4* up, const vec4* fwd)` |
| **Commit** | `0x141163ee0` | `void(cam, char recalcNearFar, const mat4* viewOverride /*NULL = usa a pose*/)` |
| Monta a projeção | `0x141163670` | `void(cam)` (chamada dentro da Commit) |
| SetFOV | `0x141164bc0` | `void(cam, float fovGraus?)` |
| SetNearFarParams | `0x141164b00` | `void(cam, const params*)` |

A **Commit** é genérica: uns 40 lugares a chamam (todas as câmeras, mais
subsistemas do renderer como sombras, reflexos e thumbnails). **Qualquer hook
nela precisa filtrar pelo ponteiro da câmera.**

---

## 4. Parâmetros globais da FPC [O]

| Global | Endereço | Padrão | Quem grava |
|---|---|---|---|
| `eye_weight` | `0x1423f6c14` | 0.5 | `camera.fpc.eye_weight`, `camera.fpc.simple` (1.0 / 0.5) |
| `fov_zoom_ratio` | `0x1423f6c28` | 0.1 | comando de foco |
| `fov_zoom_max` | `0x1423f6c2c` | 50 | comando de foco |
| `max_dist` | `0x1423f6c30` | 400 | comando de foco |
| `out_speed` | `0x1423f6c34` | 10 | comando de foco |
| `in_speed` | `0x1423f6c38` | 200 | comando de foco |
| simple mode | `0x1424cd2c0` | 0 | `camera.fpc.simple` |
| localizador de serviços | `0x1424fe708` | — | lido por `0x140b3bc50` |

`FPS_SetParam(ctrl, índice, valor)` em `0x14020de20` grava esses globais:
0 = idletime, 1 = eye_weight, 2 = out_speed, 3 = in_speed, 4 = fov_zoom_ratio,
5 = fov_zoom_max, 6 = max_dist.

---

## 5. Ainda não mapeado [?]

| Item | Por que importa | Como achar |
|---|---|---|
| Função que desenha a cena | render duplo para VR | xrefs para os campos `+0x170`/`+0x0f0` da câmera principal, depois da análise completa do Ghidra |
| `Present` / swapchain | enviar os quadros ao headset | não precisa de mapa: hook padrão do `IDXGISwapChain::Present` (D3D11) |
| UI 2D | virar painel flutuante em VR | descobrir onde a UI é composta por cima da cena |
| API de ossos | âncora do corpo (pélvis) | `0x140892fb0(&simId)` → objeto → `vf[0x20]` → rig; índice do osso por `0x14089a8d0("nome")` e `0x14089a6f0`; pose do osso por `vf[0x48](obj, idx, &out)` [I] |
| Leitura de `fpdisablecamerabob` por frame | saber o que ele remove | xrefs de `controller+0xb0` |
