# Como usar o que foi mapeado

Ordem recomendada: comece pela etapa 0. Cada etapa seguinte depende de confirmar
a anterior **dentro do jogo**. Nada daqui foi testado com o jogo rodando.

---

## Etapa 0: confirmar o mapa sem código (5 minutos)

Já em primeira pessoa (Shift+Tab), abra o console (Ctrl+Shift+C):

```
testingcheats true
camera.fpc.eye_weight 0
```

- **Se a rotação parar de "chacoalhar"**: o mapa da mistura com os olhos está
  certo, e o comando funciona no jogo final.
- **Se nada mudar ou der "comando desconhecido"**: me mande o texto que
  aparece. Os comandos podem estar desativados na versão final; nesse caso, o
  hook da etapa 1 grava o valor direto.

Depois teste `camera.fpc.eye_weight 1` (deve ficar pior) para ter certeza de
que o efeito é real.

---

## Etapa 1: o rig novo no monitor (`exemplo_hook/`)

### O que ele faz
| Problema | Solução no `fpc_rig.dll` |
|---|---|
| A rotação segue os olhos | `eye_weight = 0`, chamando a função do próprio jogo (`FPS_SetParam(ctrl, 1, 0)`) |
| A posição segue a cabeça | Intercepta `EngineCamera::SetPose` **só quando quem chama é a FPC** e troca a posição por uma versão filtrada (vertical bem mais suave que horizontal) |
| Endereços mudam a cada patch | Acha as funções por assinatura (AOB), não por endereço fixo |

### Compilar
```
exemplo_hook\build.bat
```
Gera `exemplo_hook\out\fpc_rig.dll` (já compilei uma vez aqui: compila sem
avisos com o VS 2022 Build Tools; o MinHook 1.3.3 está embutido em `minhook\`).

### Carregar no jogo
1. **Tire o `DD_FPC_Hook.dll`**, para os dois não brigarem: ele intercepta
   `0x14020f690`, que é exatamente a função que chama o `SetPose`.
2. Carregue o `fpc_rig.dll` **do mesmo jeito que você carrega o
   `DD_FPC_Hook.dll` hoje**.
3. Entre em primeira pessoa e saia do jogo.
4. Abra o `fpc_rig.log`. Ele fica na pasta de trabalho do jogo, normalmente
   `Game\Bin`.

### O que conferir no log
```
FPS_Update=00007FF6...  SetPose=00007FF6...  SetParam=00007FF6...   ← nenhum pode ser 0000000000000000
hooks ativos
FPS ctrl=... cam=... dt=0.016...                                    ← dt perto de 1/60 confirma que é segundos
raw=(x y z) filt=(x y z) up=(...) fwd=(...)                         ← a cada 120 frames
```
- **Eixo vertical:** ande com o Sim. O eixo que fica quase constante enquanto ele
  anda em terreno plano é o vertical. O `up=` deve ter esse eixo perto de 1.
  Se não for o Y, mude `g_upAxis` no topo do `.cpp`.
- **Ajuste fino:** `g_smoothVertical` (menor = menos balanço) e
  `g_smoothHorizontal` (menor = mais "flutuante", maior = mais preso à cabeça).

### Se o jogo fechar
Mande o `fpc_rig.log` e o horário. Os pontos mais prováveis de erro são a
convenção de chamada do `FPS_Update` (o segundo argumento ser `float dt` é
**inferido**) e o `FPS_SetParam` receber o controlador, e não o `Impl`.
Para isolar, comente a linha `p_FpsSetParam(...)`.

---

## Etapa 2: âncora no corpo (próximo mapeamento)

O filtro da etapa 1 ainda parte da posição da cabeça. O design final:

```
posição = âncora_do_corpo (pélvis/Spine2, filtrada) + altura_dos_olhos (constante por Sim)
rotação = mouse (depois: headset)
```

Para isso falta mapear a API de ossos. Os pontos de entrada já estão no
`MAPA_CAMERA.md` §5: `0x140892fb0` → objeto → rig → `vf[0x48](obj, idx, &out)`.
Com isso, o `hk_SetPose` passa a montar a posição do zero, sem depender da
cabeça.

---

## Etapa 3: VR (quando as etapas 1 e 2 estiverem estáveis)

| Peça | Onde entra | Situação |
|---|---|---|
| Pose do headset | OpenXR (`XR_KHR_D3D11_enable`; o jogo usa D3D11) → mesma fonte que hoje é o mouse | design |
| View por olho | `EngineCamera::Commit(cam, 1, &viewDoOlho)`. O **3º argumento** substitui a matriz de view, e tudo o que depende dela (inversas, frustum, culling) é recalculado | mapeado [O] |
| Projeção por olho | hook em `0x141163670` (monta a projeção) para gravar a matriz assimétrica do headset em `cam+0x70` | mapeado [I] |
| Envio dos quadros | hook em `IDXGISwapChain::Present` | padrão, não precisa de mapa |
| Render duplo | achar a função que desenha a cena e chamá-la 2× | **não mapeado**; a alternativa simples é Alternate Eye Rendering (um olho por frame) |
| UI | painel flutuante | **não mapeado** |

---

## Cuidados

- **Atualizações do jogo:** se o log mostrar alguma função `0000000000000000`, a
  assinatura quebrou. Refaça com `ferramentas/sigs.py` (ou me peça).
- **Commit é genérica:** uns 40 lugares a chamam, incluindo sombras e reflexos.
  Qualquer hook nela precisa checar `cam == g_fpsCam`.
- **Salve antes de testar.** Hooks de câmera não mexem no save, mas um crash no
  meio de um salvamento pode.
