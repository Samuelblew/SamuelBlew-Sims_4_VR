# Menu de configurações do VR (v49)

**Abrir/fechar:** `DEL` no teclado (configurável), ou **segurar** o botão de menu `≡` do controle esquerdo
por 0,6 s (um toque rápido continua alternando a tela virtual). Também fecha com `Esc`, com `B` ou no `✕`.

- **No headset:** o painel aparece na sua frente. O laser da mão direita aponta, o gatilho clica e arrasta
  os sliders, e o analógico rola a lista. Não precisa segurar o grip.
- **No monitor:** o painel aparece por cima da imagem do jogo e é usado com o mouse (a roda rola a lista).
- **Com o menu aberto:** o jogo não recebe teclas, cliques nem botões do controle. Cada mudança vale na
  hora e é gravada no `fpc_rig.ini`. "Restaurar padrões da aba" volta aos valores de fábrica.

Referências pesquisadas: UEVR (menu no Insert, métodos de render, perfis por jogo, bindings), VRPerfKit
(FSR/NIS/CAS, `renderScale`, `sharpness`, presets da AMD 77/67/59/50 %), Skyrim VR (giro suave/em
degraus, ângulo, velocidade, altura, mão principal, filtro de FOV) e guias de conforto (vinheta, modo
sentado).

## Abas

| Aba | Opções (chave no ini) |
|---|---|
| **Imagem** | resolução atual (informativo); resolução do jogo por preset (`vr_preset`): Janela atual / Nativo 100 % / Ultra qualidade 77 % / Qualidade 67 % / Equilibrado 59 % / Desempenho 50 % / Personalizado; resolução personalizada (`vr_resolucao_jogo`); upscaler (`vr_upscaler`): Desligado / Bilinear / FSR 1.0 / CAS; resolução no headset (`vr_supersampling`); nitidez (`vr_nitidez`); FOV simétrico (`vr_fov_simetrico`) |
| **Mundo e câmera** | escala do mundo (`vr_escala_mundo`); separação dos olhos (`vr_ipd_escala`); altura da vista (`vr_altura`); recentrar agora; rastrear a cabeça; giro máximo da cabeça do Sim; distância e largura da tela virtual |
| **Conforto** | giro suave/em degraus; ângulo do degrau; velocidade do giro; vinheta em movimento (`vr_vinheta`): Desligada / Leve / Média / Forte; zona morta; atualizações de movimento por segundo; rapidez do corpo |
| **Interface** | painel da mão (largura, altura, ângulo para abrir); interface no mundo e a distância dela; tamanho da roda VR; relógio; raio do laser; mão do laser; cursor e tamanho dele; tamanho deste menu (`config_tamanho`) |
| **Corpo e mãos** | braços (não / fora de interações / sempre); dedos; cabeça; pernas; agachar; inclinar; posição e inclinação das mãos; passos; agarrar Sims (alcance, moleza); agarrar objetos; avatar no cliente e distância de ressincronizar |
| **Controles VR** | para cada ação, qual botão físico faz o quê (`botao_laser`, `botao_selecionar`, `botao_voltar`, `botao_recentrar`, `botao_painel`, `botao_correr`, `botao_menu`): clique e aperte o botão. Também tem trocar analógicos (`trocar_analogicos`) |
| **Teclado** | modo primeira pessoa, abrir as configurações (`tecla_config`), recentrar, tela virtual, esconder o corpo, correr, teste dos braços: clique e aperte a tecla (Esc cancela) |

## Como funciona por dentro

- **Upscaling (`xr_upscale.inl`, `fsr/`):** antes, cada olho recebia uma cópia direta do backbuffer
  (1920×1009, o tamanho da janela), quando o Quest 2 pede 2080×2096 por olho. Agora a imagem do olho
  tem a resolução do headset vezes o supersampling e é reconstruída na GPU:
  - **FSR 1.0:** EASU seguido de RCAS, código oficial da AMD (licença MIT), compilado pelo `fxc` no
    `build.bat`.
  - **CAS:** uma passada só.
  - **Bilinear:** filtro simples.

  O estado de pipeline do jogo não é mexido (`ID3DDeviceContextState` próprio). Se algo falhar, o mod
  volta sozinho para a cópia direta.
- **Resolução do jogo:** é o tamanho da área interna da janela. O mod redimensiona a janela, liberando
  tamanhos maiores que a tela pelo `WM_GETMINMAXINFO`, e o jogo passa a desenhar nesse tamanho. Para
  isso o jogo precisa estar em modo Janela. "Janela atual" não mexe e devolve o tamanho original.
- **Remapeamento (`xr_input.inl`):** cada entrada lógica que o mod lê recebe um botão físico. O rig dos
  dedos e o próprio menu leem os botões físicos.
- **Vinheta (`xr_vignette.inl`):** é uma camada OpenXR presa à cabeça, com alfa radial. Só aparece com o
  movimento do analógico (andar ou girar), nunca com o movimento da cabeça.
- **IPD e altura:** valem só para a imagem do jogo. A camada continua enviando a pose real ao runtime.
- **Teste offline:** `teste/build_teste_config.bat` gera `teste_config.exe`. Ele desenha
  `config_aba0..6.png` e confere a ida e volta dos valores no ini, as chaves repetidas e os cliques.
