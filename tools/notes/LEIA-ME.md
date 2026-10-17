# The Sims 4: como funciona a câmera em primeira pessoa (FPC)

Alvo: `D:\Games\The Sims 4\Game\Bin\TS4_x64.exe`, versão **1.118.257.1020**.
Todos os endereços abaixo valem **só para esta versão** (base da imagem `0x140000000`).
Depois de uma atualização do jogo, refaça a busca com os scripts de `ferramentas/`.

Legenda: **[observado]** = visto diretamente no binário; **[inferido]** = conclusão
a partir do código, ainda não testada no jogo; **[desconhecido]** = não resolvido.

---

## 1. Resumo

A lógica da primeira pessoa é **toda nativa (C++) dentro do `TS4_x64.exe`**. Os
arquivos Python (`simulation.zip`) só cuidam de foco, tremor da câmera e paredes
levantadas. **[observado]**

O "balanço" da câmera vem de duas fontes:

1. **Posição**: a câmera fica presa a um osso do Sim (normalmente `b__Head__`).
   Quando a cabeça balança, a câmera balança junto.
2. **Rotação**: a cada frame, a rotação da câmera é puxada para a média da
   orientação dos ossos `b__L_Eye__` e `b__R_Eye__`, com um peso global
   (**`eye_weight`, padrão 0.5**). **[observado]**

O jogo já traz uma opção para isso (`FPDisableCameraBob`) e **8 comandos de
console escondidos** para configurar a câmera em primeira pessoa.

---

## 2. O que testar primeiro (sem hook)

### 2a. Opção `fpdisablecamerabob` no `Options.ini`

O jogo lê estas chaves da configuração **[observado, função `0x14020fcb0`]**:

| Chave no `Options.ini` | Padrão | Efeito no código |
|---|---|---|
| `fpcameraspeed` | 100 | velocidade do mouse em FPC |
| `fpinvertverticalrotation` | 0 | inverte o eixo vertical |
| `fpinverthorizontalrotation` | 0 | inverte o eixo horizontal |
| **`fpdisablecamerabob`** | **0** | grava um bool no controlador da câmera (offset `+0xb0`) |

Uma cópia antiga em `D:\Major Old Windows Files\Electronic Arts\The Sims 4\Options.ini`
tem `fpdisablecamerabob = 0`. **Feche o jogo, mude para `1` e teste.**
O `Options.ini` atual não foi encontrado em `Documentos\Electronic Arts\The Sims 4\`.
O jogo cria esse arquivo na primeira execução.

**[desconhecido]**: não localizei o trecho que *lê* esse bool durante o frame,
então não sei exatamente quanto do balanço ele remove.

### 2b. Comandos de console `camera.fpc.*`

Os nomes são registrados como hash **FNV-1 de 32 bits em minúsculas**. Esse
algoritmo foi confirmado com cheats conhecidos, como `headlineeffects` =
`0x7acc9a71`. Os nomes abaixo foram recuperados por força bruta contra os
hashes. **[observado: hashes e handlers; inferido: grafia exata]**

| Comando | Hash | Uso (texto de ajuda do jogo) | Handler |
|---|---|---|---|
| `camera.fpc.attach` | `0xa1495548` | `SimId \| (ActorId BoneName OffsetX OffsetY OffsetZ UpX UpY UpZ ForwardX ForwardY ForwardZ)` | `0x1402187d0` |
| `camera.fpc.set_joint` | `0xfded9b84` | `(joint_name \| joint_idx) [OffsetX OffsetY OffsetZ UpX UpY UpZ ForwardX ForwardY ForwardZ]` | `0x1402181b0` |
| `camera.fpc.smoothing` | `0x3dcf6317` | `(off/low/medium/high/default) \| (numPosSamples numDirSamples numUpSamples)` | `0x140218980` |
| `camera.fpc.lookat` | `0x82ef323d` | `[bone name] [objectId]` | `0x1402185e0` |
| `camera.fpc.idletime` | `0x6204169a` | `param val` → parâmetro 0 | `0x140218540` |
| **`camera.fpc.eye_weight`** | `0x4551bb8d` | `valor` → parâmetro 1 (**peso dos olhos**) | `0x140218330` |
| `camera.fpc.simple` | `0xb85125d9` | `(true\|false)` | `0x140217fa0` |
| **nome desconhecido** | `0x538584aa` | `max_dist out_speed in_speed fov_zoom_ratio fov_zoom_max` (padrão `400 10 200 0.1 50`) | `0x1402183d0` |

Sugestões para testar no console (Ctrl+Shift+C), já em primeira pessoa:

```
testingcheats true
camera.fpc.eye_weight 0
camera.fpc.smoothing high
camera.fpc.set_joint b__Spine2__
```

- `eye_weight 0`: no código, com peso ≤ 0 o bloco inteiro que segue os olhos é
  **pulado** (`0x14020ee40`). Isso deve tirar o "chacoalhar" da rotação. **[inferido]**
- `set_joint b__Spine2__`: prende a câmera ao peito em vez da cabeça. Pode
  precisar de offset para cima (`OffsetY`). **[inferido]**

**[desconhecido]**: não testei se o console de cheats aceita esses `ClientCommands`
no jogo final, nem se `testingcheats` é obrigatório.

---

## 3. Como o código funciona

### Comandos → `SetFpcParam(câmera, índice, valor)` em `0x14020de20` **[observado]**

Tabela de saltos em `0x14020dec0`. Cada índice grava um float global:

| Índice | Comando | Global (RVA) | Endereço |
|---|---|---|---|
| 0 | `idletime` | multiplica e vai para o objeto `+0x1a0` | — |
| **1** | **`eye_weight`** | `0x23f6c14` | **`0x1423f6c14`** (padrão **0.5**) |
| 2 | foco `out_speed` | `0x23f6c34` | `0x1423f6c34` |
| 3 | foco `in_speed` | `0x23f6c38` | `0x1423f6c38` |
| 4 | foco `fov_zoom_ratio` | `0x23f6c28` | `0x1423f6c28` |
| 5 | foco `fov_zoom_max` | `0x23f6c2c` | `0x1423f6c2c` |
| 6 | foco `max_dist` | `0x23f6c30` | `0x1423f6c30` |

### `camera.fpc.simple` → `0x14020e3c0` **[observado]**

- `true`: `eye_weight = 1.0`, flag `0x1424cd2c0 = 1`, suavização `+0x24c = 15`, `+0x258 = 300`.
- `false`: `eye_weight = 0.5`, flag `= 0`, `+0x270 = 120`, `+0x24c = 12`, `+0x258 = 120`, `+0x264 = 200`.

### Escolha do osso → `0x14020e960` **[observado]**

- Padrão: `b__Head__` com referência `b__Spine3__`, mais um offset por idade/espécie
  (constantes em `0x1424cd450`/`0x1424cd460` e `0x141b40ed0`–`0x141b40f1c`).
- Caso especial (tipo == 1 e subtipo != 1): `b__CAS_Glasses__` com referência
  `b__Spine2__`. **[inferido]** Provavelmente é o caso humano/adulto; o significado
  exato dos tipos é **[desconhecido]**.

### Por frame → `0x14020f690` (o seu `eye_wrap_B`) **[observado]**

1. `0x14020ee40`: copia a transformação do osso (`0x14020f290`) e, se
   `eye_weight > 0`, mistura a rotação com a média de `L_Eye`/`R_Eye`:
   `rot += (rotOlhos − rot) × eye_weight`.
2. `0x14020fb70` / `0x14020f9c0`: suavização (objetos `+0x190` / `+0x1a0`).
3. `0x14020b770`: filtro com os parâmetros de suavização (`+0x210`).
4. FOV/zoom: `fov = fov_zoom_ratio × (+0xa0)`, limitado por `fov_zoom_max`.

---

## 4. Sobre o seu `DD_FPC_Hook.dll` (Fase 5.1)

O seu log mostra:

- `OK retarget L_Eye->Spine2 lea=14020F22D`: essa instrução é o `lea` do texto
  `"b__L_Eye__"` dentro de `0x14020ee40`. Redirecionar para `Spine2` só muda **de
  onde vem a rotação misturada**, não a posição. A posição continua vindo do osso
  de fixação (`b__Head__`), por isso o balanço não some todo. **[inferido]**
- `hook oneshot @ 14020BC80` e `hook eye_wrap_B @ 14020F690` caem exatamente nas
  funções descritas acima.

Abordagem mais simples e estável para um hook: em vez de patchear código,
**escreva floats diretamente nos globais** a cada frame (ou uma vez):

```c
// base = GetModuleHandle(NULL)  (0x140000000 sem ASLR)
*(float*)(base + 0x23f6c14) = 0.0f;   // eye_weight: desliga a mistura com os olhos
```

Para a posição, use o caminho do próprio jogo: `camera.fpc.set_joint` (função
`0x1402181b0`, que chama `0x14020fe10`/`0x14020fec0`/`0x14020fd70`) com um osso
mais estável (`b__Spine2__`) e um `OffsetY` para subir até a altura dos olhos.

---

## 5. Pendências

| Pergunta | Situação |
|---|---|
| Onde a lógica da FPC fica | ✅ respondida (nativa, `0x14020b000`–`0x140219000`) |
| Comandos de console da FPC | ✅ 7 de 8 nomes; o comando de foco (`0x538584aa`) tem nome **desconhecido** (testado até 6 letras depois de `camera.fpc.`) |
| O que é `eye_weight` e o padrão | ✅ global `0x1423f6c14`, padrão 0.5 |
| O que `fpdisablecamerabob` desliga exatamente | ⚠️ parcial: sei onde é lido, não onde é usado |
| Os comandos funcionam no jogo final | ❓ não testado |

## Arquivos nesta pasta

- `descompilado/`: pseudocódigo do Ghidra 12.1.4 (sem auto-análise; nomes
  `FUN_`/`DAT_` são do Ghidra, os nomes dos arquivos são meus).
- `python_original/`: os `.pyc` da câmera extraídos de `simulation.zip`.
- `strings_camera.txt`: todos os textos relacionados à câmera, com endereço.
- `ferramentas/`: scripts usados (Python 3.11 + `capstone`, script Ghidra
  `DecompileList.java`). Eles só leem o executável; não modificam nada.
