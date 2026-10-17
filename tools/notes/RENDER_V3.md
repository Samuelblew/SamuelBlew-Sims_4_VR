# Fase V3: como o TS4 desenha um frame (captura RenderDoc)

Captura: `capturas/ts4_frame12609.rdc` (RenderDoc 1.46, D3D11, janela 1920×1009, primeira pessoa).
Análise: export XML (`renderdoccmd convert -c xml`) + script `analisar_frame.py`.

## 1. Estrutura do frame

O TS4 usa **renderização multithread**: 11 contextos *deferred* gravam listas de comandos em
paralelo, e o contexto principal só as executa em ordem (13 listas). Ordem observada:

| # | Lista | Alvo | Desenhos | O que parece ser |
|---|---|---|---|---|
| 1 | 186361 | 16×16 | 45 | consultas pequenas (luminância/visibilidade) |
| 2 | 186362 | profundidade 1920×1009 (vp 960×504) + 960×504 R32F | ~395 | pré-passe de profundidade + oclusão em meia resolução |
| 3–5 | 186363–65 | sombra 2048×6144 (3 × 2048²) | ~230 cada | mapas de sombra em cascata |
| 6 | 186366 | 64×1 + sombra | ~90 | mais sombras |
| 7 | 186367 | 1024×512 | ~380 | cena auxiliar (reflexos/água) |
| 8 | **186368** | **1920×1009 MSAA 8×** | **~704** | **cena principal** |
| 9–10 | 186369–70 | MSAA + cadeia 1024×512 | ~120 | transparências e pós-processamento (bloom/blur) |
| 11 | 186371 | MSAA, viewport 1920×1080 | 63 | provavelmente a **interface**, desenhada **dentro da mesma imagem** |
| — | contexto principal | `ResolveSubresource` | — | MSAA → imagem final |

## 2. O ponto que decide o estéreo

**Todos os constant buffers** (onde vai a câmera) são escritos **dentro das listas, pelos
contextos deferred**: 49 buffers atualizados, todos `map@def`, em anéis de 64 KB (VS0/PS0).
Nenhum é escrito no contexto principal antes de executar as listas.

Consequência:
- **Não dá** para reexecutar as listas com a câmera do outro olho: a câmera está gravada nelas.
- Estéreo real exige que o **motor grave a cena duas vezes por frame** (chamar o renderizador
  inteiro de novo com a outra câmera). Possível em teoria, mas arriscado (estado por frame,
  anéis de buffer, consultas) e dobra o custo de CPU.
- A interface parece ser desenhada **dentro** da imagem da cena, então separá-la num painel
  também exige mapear mais.

## 3. Decisão

| Caminho | Como | Prós | Contras |
|---|---|---|---|
| **AER** (V4a, recomendado agora) | cada frame do jogo desenha **um olho**, alternando; a câmera já é nossa (`SetPose`) | usa só o que já controlamos; ~170 fps mono → ~85 fps por olho a 1080p | 1 frame de diferença entre os olhos em movimento rápido; metade do fps por olho |
| Estéreo real (V4b, pesquisa) | achar a função que grava a cena e chamá-la 2× por frame | profundidade perfeita | alto risco; CPU ×2; precisa mapear o renderizador |

**Plano:** fazer a V4a (AER) primeiro. Ela dá profundidade de verdade no headset com risco
baixo. Em paralelo, pesquisar a V4b.

## 4. O que a V4a precisa

1. **Câmera por olho:** posição da cabeça ± meia distância interpupilar, alternando a cada frame
   (já temos `SetPose`).
2. **Projeção por olho:** o FOV assimétrico do Quest (`xrLocateViews`) na projeção do jogo
   (hook em `0x141163670`, que monta `cam+0x70`).
3. **Dois swapchains (um por olho)** e uma camada de projeção OpenXR no lugar da tela virtual.
4. **Proporção da janela** perto da do olho do Quest 2 (≈ 1:1, por exemplo 1600×1600).
