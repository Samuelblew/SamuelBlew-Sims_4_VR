# Interface (UI) do TS4 em VR

Legenda: **[O]** observado no código · **[I]** inferido · **[T]** a confirmar no teste.

## Como o jogo desenha a interface

| O quê | Onde | |
|---|---|---|
| Interface = Scaleform (GFx), filmes em `Data/Client/UI.package` (tipo `62ECC59A`) | 243 filmes | [O] |
| Renderizador próprio da EA: `Scaleform::Render::Sims4::HAL`, vtable `0x141db50a8` | RTTI | [O] |
| `BeginScene` (vtable+9) `0x141617910` · `EndScene` (+10) `0x14161a020` · `beginDisplay` (+11) · `PushRenderTarget` (+24) · `PopRenderTarget` (+25) | strings de perfilador | [O] |
| O HAL usa o **contexto do motor** em `HAL+0x1830*8`; `ctx+0x1d0` é um **ID3D11DeviceContext** (vtable +0x160 = RSSetViewports, +400 = ClearRenderTargetView, +0x1a8 = ClearDepthStencilView), chamado **na mesma thread** | `Ctx_SetRenderTarget 0x1411fb900`, `Ctx_Clear 0x1411f3b40` | [O] |
| O motor aplica o alvo de desenho só na hora de desenhar (flag `ctx+0xe8`) | `0x1411fb900` | [O] |

## O círculo do centro (primeira pessoa)

É o quadro `camera_fpc` do cursor virtual (`VirtualCursor_fla.Cursor_1`, sprite 81) no filme
`D30027A724ADC468`: sprite 52 → forma 51. Removido pelo mod `ui_mod/FPC_SemMira.package`
(gerado por `ui_mod/gerar_sem_mira.py`). Confirmado no jogo: saiu.

## Interface separada (v29)

`exemplo_hook/xr_ui.inl`:
1. Hook em `HAL::BeginScene` / `EndScene`. Entre os dois, todo `OMSetRenderTargets` daquele contexto
   para um alvo de **tela cheia** vira a nossa textura `R8G8B8A8` transparente (+ profundidade/estêncil
   própria, o Scaleform usa estêncil nas máscaras). Alvos menores (filtros do Scaleform) passam direto.
2. Só durante o estéreo síncrono: as imagens dos olhos ficam sem interface. Fora dele (menus, modo
   painel), a interface volta a ser desenhada normalmente na imagem.
3. A textura vira uma camada quad do OpenXR presa à mão esquerda (grip esquerdo olhando para a mão).
4. O laser (mão direita) é outra camada quad; o acerto no painel vira (u, v) → pixel da janela →
   `SetCursorPos` + cliques (`vr_laser.inl`).

**A confirmar no teste:** se o alfa da interface sai certo na textura (depende do blend do Scaleform),
e se o `OMGetRenderTargets` no `BeginScene` já devolve o alvo da cena (log `ui: BeginScene do Scaleform`).

## Cursor, seleção e roda de opções (v30)

- O jogo **não usa entrada raw** (sem `RegisterRawInputDevices`); importa `SetCursorPos`, `ClipCursor`, `ShowCursor`, `GetCursorPos`. Na primeira pessoa ele recentraliza o cursor e escolhe/destaca o objeto **sob o cursor** (por isso o brilho só acontece no centro). [O] imports · [I] comportamento
- No handler de mouse da FPC (`0x14020c950`), o **botão direito** liga/desliga o "olhar com o mouse" (`Impl+0x1f1`). [O] → o laser não usa botão direito.
- Laser (`vr_laser.inl`): grip direito mostra; enquanto ele controla o cursor, `SetCursorPos`/`ClipCursor` do jogo são ignorados (hook em user32). No mundo (só com INSERT), o cursor vai para a projeção do ponto apontado na câmera central do jogo; o jogo destaca e seleciona normalmente.
- Roda de opções: o cliente pede `interactions.choices <alvo> <pick> x y z ...` e a escolha volta como `interactions.select ...` (`server_commands/interaction_commands.py`: `generate_choices`, `select_choice`). Os dois passam pela função de comandos do cliente (`ServerCmd`), interceptada.
- A roda é interface 2D desenhada onde o objeto aparece na tela. Em VR, a textura da interface vai para um **plano fixo no mundo que reproduz o frustum da câmera do jogo** no momento do clique (pose da cabeça + tangentes de `cam+0x14/0x18/0x1f0/0x1f4` e multiplicadores `cam+0x270`), a 1,2 m: cada pixel fica na direção do ponto do mundo que representa.

## Revisão (v32): por que a roda e a seleção não funcionavam, e o desenho novo

- **A seleção nunca chegou ao jogo.** Na primeira pessoa a escolha/brilho é pelo **centro da câmera do jogo** (cursor virtual `camera_fpc`, `WorldSelectVirtualCursorValidCheck` no filme do cursor). Mover o cursor do Windows não muda nada: o clique do laser selecionava o centro (vazio), e o jogo nunca pediu a roda.
- **`interactions.choices` não passa pelo `ServerCmd`.** Quem monta o comando é o ActionScript (filme `F335C695884B7E64`: `ClosePieMenu`, `modControl/modAlt/modShift`, `interactions.choices`, `interactions.select`), por outro caminho.
- **A interface é um filme só** (`UIContainer.gfx`): HUD, balões (`widgets.ObjectBoundElements.elements.SimBalloon`) e roda (`widgets.Gameplay.PieMenu`) são desenhados juntos; não dá para separá-los por filme no HAL.
- **Desenho novo:**
  1. A câmera do jogo (não a dos olhos) vai para o raio do laser quando ele está ativo (grip direito + INSERT): o brilho e a seleção do próprio jogo seguem o laser, sem paralaxe.
  2. "Camada do mundo": a textura da interface num plano que reproduz a câmera do jogo (pose + frustum) a 2 m, só a região central. Balões ficam sobre os Sims, a roda aparece na ponta do laser.
  3. A roda é avisada pelo Python: `interaction_commands.create_pie_menu_message` (global do módulo) → `-1005`; `Client.select_interaction` → `-1006`. Com a roda aberta a câmera do jogo trava e o laser move o cursor sobre a roda.
- **Congelamento:** ocupado = qualquer interação rodando que não seja o idle padrão (antes só as visíveis); com o corpo congelado, se quadril/tronco da animação de baixo se afastarem muito da foto por 0,25 s, o corpo é solto até a animação assentar.

## Cursor virtual (v34): a "bolinha" e o ponto de seleção

- Classe nativa `VirtualCursor@ClientApp`. `FUN_140154080(vc, x, y)` grava a posição em `vc+0xb0/+0xb4` e, se não travado (`vc+0x17c == 0`), manda um "mouse movido" (evento `0x915aa439`) ao jogo. `VC_LockSet 0x140153a10` salva a posição em `vc+0x180/+0x184` ao travar e volta para ela ao destravar (chama `ActionScript VirtualCursorLockSet`). `FUN_140d248f0` chama `InputUtils.SetVirtualCursorPosition` no ActionScript. [O]
- `Canvas@ClientApp` (vtable `0x141b22860`, entrada 38 = `0x1401431a0`) trava/destrava o mouse; o cursor virtual fica em `canvas+0xf0`; conversão cliente→interface: `x*canvas+0xb4 + canvas+0xbc` (se `canvas+0x9c`). [O]
- Causa da bolinha presa: o laser movia o cursor do Windows, o cursor virtual ia junto, e o jogo salvava/voltava para essa posição fora do centro. Correção: em VR, fora do controle do laser o cursor virtual fica no centro (hook em `FUN_140154080`), e o `LockSet` salva/volta para o centro.
- Mod de interface: agora esvazia as 41 formas exclusivas do cursor virtual (todos os quadros), não só `camera_fpc`.
