# Plano do VR: The Sims 4 (fase 2)

**Escopo escolhido:** Quest via Link/Air Link · estéreo real · controles de VR (analógicos) · interface em painel flutuante.
**Hardware:** RTX 3070 · i9-11900KF · 32 GB · app do Quest Link instalado ("Meta Virtual Monitor" presente).
**Base:** o rig de primeira pessoa (`fpc_rig.dll` v9.1 + `fpc_walk.ts4script`), que já funciona.

Legenda: ✅ já temos · 🟢 viável, alta confiança · 🟡 viável, mas precisa mapear · 🔴 fora do alcance (ou não vale a pena)

---

## 1. O que é e o que não é possível

| Item | Situação | Por quê |
|---|---|---|
| Rodar o VR dentro do jogo | 🟢 | O jogo usa **D3D11**; o OpenXR tem `XR_KHR_D3D11_enable`, suportado pelo runtime da Meta |
| Câmera controlada por nós | ✅ | `SetPose` + direção do mouse; rig desacoplado do esqueleto |
| Head tracking 6DoF (cabeça) | 🟢 | Troca a fonte da pose: mouse → headset. O rig já foi pensado para isso |
| Matriz de view por olho | ✅ / 🟢 | `EngineCamera::Commit(cam, 1, &view)` aceita **view própria** e recalcula inversas, frustum e culling |
| Projeção por olho (FOV assimétrico do headset) | 🟡 | Hook em `0x141163670` (monta a projeção) para gravar a matriz do headset em `cam+0x70` |
| **Renderizar a cena 2× por frame** | 🟡 **maior risco** | Falta achar a função que desenha a cena e garantir que ela pode rodar duas vezes (sombras, culling, pós-processamento) |
| Separar a **interface 2D** da cena | 🟡 | A UI é desenhada por cima da cena. Precisamos do ponto entre "cena pronta" e "UI desenhada" para mandar a UI para um painel |
| Andar com o analógico | ✅ / 🟢 | É o mesmo `fpc.move` do WASD, só muda a entrada |
| Virar com o analógico (snap turn ou suave) | 🟢 | Gira o yaw do rig; o corpo já acompanha a câmera |
| Painel flutuante com a interface | 🟢 / 🟡 | Camada "quad" do OpenXR; depende de separar a UI (acima) |
| Clicar em objetos/Sims | 🟡 | É preciso **algum ponteiro**: raio do controle, olhar + botão ou o mouse. Detalhado no §4 |
| Desempenho ≥ 72 fps | 🟡 | ~150 fps hoje em mono → ~75–80 fps estimados em estéreo; a reprojeção (ASW) cobre quedas |
| Mãos/braços do Sim seguindo os controles (IK) | 🟢 provável | Temos escrita na pose antes da finalização (v11) + cadeia do braço do .blend; IK de dois ossos na DLL (§8, PIPELINE_ANIMACAO.md) |
| Pegar objetos com a mão (física) | 🔴 | O TS4 não tem física de objetos; toda interação é animação pronta |
| Câmera durante animações | 🟢 | "Bolha de snap" (§5): a animação roda, a câmera só pula quando sai da bolha |

---

## 2. Arquitetura

```
 ┌──────────── OpenXR (runtime Meta / Quest Link) ────────────┐
 │  pose da cabeça · poses/botões dos controles · swapchains    │
 └───────────────┬──────────────────────────────▲──────────────┘
                 │ pose                          │ imagens (olho E, olho D, painel UI)
 ┌───────────────▼──────────────┐   ┌───────────┴──────────────────────┐
 │ RIG (já existe)              │   │ RENDER ESTÉREO (novo)              │
 │ origem + yaw (analógico)     │──►│ para cada olho:                   │
 │ cabeça = origem + pose HMD   │   │   view/proj do olho → Commit      │
 │ estado livre/ocupado         │   │   desenha a cena → copia p/ swapchain│
 └───────────────┬──────────────┘   │ UI → textura do painel             │
                 │ fpc.move/face     └───────────────────────────────────┘
 ┌───────────────▼──────────────┐
 │ fpc_walk.ts4script (já existe)│  colisão, portas/escadas, livre/ocupado
 └──────────────────────────────┘
```

Peças novas na DLL (`fpc_rig.dll` vira o núcleo do mod de VR):
1. **`xr_session`**: cria a sessão OpenXR com o device D3D11 do próprio jogo (capturado no hook de `D3D11CreateDeviceAndSwapChain`/`Present`).
2. **`IPoseSource`**: `MousePose` (hoje) e `HmdPose` (VR); o rig não muda.
3. **`StereoRenderer`**: o loop por olho (detalhe na fase 3).
4. **`XrInput`**: ações OpenXR → mesmos comandos de hoje (andar, correr, virar, clicar).
5. **`UiPanel`**: textura da interface → camada quad.

---

## 3. Fases (cada uma termina com um critério testável)

| Fase | Entrega | Critério de pronto | Risco |
|---|---|---|---|
| **V0 Infra** | Sessão OpenXR aberta de dentro do jogo; log da pose do headset | O log mostra a pose mudando quando você mexe a cabeça, sem crash | baixo |
| **V1 Head tracking no monitor** | O rig usa a orientação e posição do headset no lugar do mouse; imagem só no monitor | Olhar com a cabeça move a câmera; escala/altura parecem certas | baixo |
| **V2 Tela no headset** | O frame do jogo aparece no headset como uma tela flutuante | Imagem estável no Quest; mostra que a ponte D3D11 → OpenXR funciona | médio |
| **V3 Mapeamento do render** | Captura com **RenderDoc** + Ghidra: função da cena, ponto da UI, passes de sombra e pós-processamento | Sabemos chamar "desenhar a cena" duas vezes. **Ponto de decisão:** se não der, plano B = AER | **alto** |
| **V4 Estéreo** | Dois olhos com view/proj do headset, enviados como camada de projeção | Profundidade correta, sem tremer, ≥ 72 fps ou com ASW estável | alto |
| **V5 Interface** | UI num painel flutuante; ponteiro para usar o painel | Dá para abrir menus, ver necessidades e escolher interações | médio |
| **V6 Controles** | Analógico esq. = andar, dir. = virar (snap 30°/suave), gatilho = clicar, botão = correr | Dá para jogar sem teclado | baixo |
| **V7 Conforto e desempenho** | Modo conforto nas interações; escala de resolução; ajustes | 30 min jogando sem enjoo | médio |

As fases V0–V2 não dependem do mapeamento difícil e já provam a infraestrutura. A V3 é o ponto de decisão do projeto.

---

## 4. Clicar no mundo

Você escolheu andar com os analógicos, mas o TS4 depende de **clicar** em objetos e Sims para abrir o menu de interações. Opções, da mais simples à mais trabalhosa:

| Opção | Como funciona | Esforço |
|---|---|---|
| **Olhar + gatilho** | Um ponto no centro da visão; o gatilho clica onde você olha (o jogo já faz o "pick" a partir da câmera) | baixo |
| **Raio do controle** | Um laser sai do controle direito; o gatilho clica onde ele aponta | médio (precisa gerar o "pick" a partir de um raio que não é a câmera) |
| Mouse | Continua funcionando como hoje | nenhum, mas tira a imersão |

Sugestão: **olhar + gatilho** primeiro (V6), **raio do controle** depois.

---

## 5. Conforto (regras fixas do VR)

1. **Nada move a câmera continuamente além de você.** No estado livre isso já vale (o rig não lê ossos).
2. **Interações (ocupado): "bolha de snap"** (ideia sua). A animação roda normalmente:
   - existe uma **bolha** em volta da cabeça animada do Sim;
   - dentro dela, a câmera **fica parada** e você mexe a cabeça livremente (head tracking);
   - quando a distância entre a sua cabeça (câmera) e a cabeça animada **passa do raio**, a câmera dá um **snap** (pulo instantâneo) para a cabeça animada, e a bolha recentra ali;
   - a **rotação nunca segue a animação**, só o headset.

   Detalhes a calibrar no protótipo:

   | Parâmetro | Valor inicial | Para quê |
   |---|---|---|
   | raio horizontal | 0,30 m | pequeno = segue mais; grande = menos pulos |
   | raio vertical | 0,20 m | sentar/levantar gera um único pulo |
   | intervalo mínimo entre snaps | 0,25 s | evita uma "chuva de pulos" em animações rápidas (dançar) |
   | piscada no snap | 0–80 ms (opcional) | um piscar escuro curto deixa o pulo mais confortável |
   | excesso de pulos | > 3 em 2 s | cai para um modo de reserva (fade, ou câmera parada até a animação acalmar) |

   Vantagens: o movimento contínuo, que é o que causa enjoo, some; você continua vendo a animação; e funciona igual para **portas e escadas**, sem precisar de fade.
   **Dá para testar já no monitor:** no estado "ocupado" do rig atual, a câmera ficaria parada e só pularia ao sair da bolha.
3. **Virar:** snap turn de 30° por padrão (o mais confortável); o giro suave fica como opção.
4. **Escala:** a altura dos olhos vem da calibração que já existe; o mundo precisa parecer do tamanho real (1 unidade do jogo = 1 m, confirmar na V1).

---

## 6. Riscos e plano B

| Risco | Sinal | Plano B |
|---|---|---|
| A cena não pode ser desenhada 2× | Na V3, o render tem estado global que quebra na 2ª passada | **AER**: um olho por frame, alternando (fica pronto já na V2 + V1) |
| Desempenho < 72 fps | Medido na V4 | Resolução por olho menor; desligar sombras/efeitos caros só no VR; deixar o ASW trabalhar |
| A UI não se separa da cena | Na V3, a UI é composta junto com a cena | Painel com o frame inteiro do monitor (UI + cena 2D) ao lado da visão estéreo |
| Atualização do jogo muda endereços | Assinaturas AOB falham no log | Refazer as assinaturas com `ferramentas/sigs.py` (como hoje) |
| Conflito com o Link | Tela preta ou erro de sessão | Testar primeiro só a V0, com o Quest Link ativo e o runtime da Meta como OpenXR ativo |

---

## 7. O que você precisa preparar

1. **Quest Link funcionando** (cabo ou Air Link). ✅ O runtime OpenXR ativo já é o do Meta Horizon Link (`C:\Program Files\Meta Horizon\Support\oculus-runtime\oculus_openxr_64.json`), e o serviço `OVRServer_x64` está rodando. Os logs indicam **Quest 2** via Air Link.
2. Instalar o **RenderDoc** (gratuito, renderdoc.org) para a fase V3. Eu conduzo a análise da captura.
3. Saber qual Quest é (2, 3 ou Pro): muda resolução e taxa de atualização.
4. O **`.blend` com o modelo e o esqueleto** do Sim, para a pesquisa do IK (§8).

---

## 8. Pesquisa: mãos seguindo os controles (e congelar o corpo)

O motor de animação (`Animation@Sims3`) tem estas classes (RTTI no `TS4_x64.exe`):

| Classe | O que sugere |
|---|---|
| `IkEvalNode`, `PoseIkEvalNode`, `RootIkEvalNode`, `RedirectIkEvalNode`, `DisableIkEvalNode` | Nós de IK dentro da avaliação da animação |
| **`LimbPoseIkSceneProcessor`** | IK de **membros** aplicado depois da animação |
| **`IkTargetOverridesSceneProcessor`** | **Sobrescrita de alvos de IK**: o caminho mais direto para "mão vai até o controle" |
| `ApplyBoneDeltasSceneProcessor`, `BoneDeltasActorComponent` | Deltas de osso **por ator**, aplicados por cima da animação |
| `LookAtEvalNode`, `LookAtEvaluationData` | O IK de olhar da cabeça |
| `Skeleton` | O esqueleto em tempo de execução |

**Hipótese:** depois que a animação é avaliada, uma lista de "scene processors" ajusta a pose de cada ator antes do skinning. Se conseguirmos entrar ali, podemos:
1. **mãos:** dar ao IK de membro um alvo = posição do controle (mão esq./dir.), com o cotovelo resolvido pelo próprio solver do jogo;
2. **congelar o corpo:** repetir a última pose (ou uma pose neutra) no estado livre, sem tocar na simulação;
3. **cabeça:** desligar o `LookAt` do Sim seguido (fonte do "pescoço brigando").

**Passos:**
1. Mapear a lista de scene processors e a ordem em que rodam por frame (vtables + quem as chama).
2. Entender o formato dos alvos em `IkTargetOverridesSceneProcessor` (osso, alvo, peso).
3. Protótipo no monitor: a mão direita do Sim aponta para onde a câmera olha (sem VR).
4. Com o VR (V0/V1): alvo = pose real do controle.

**O `.blend` com o modelo e o esqueleto ajuda muito:** nomes e hierarquia dos ossos dos braços e das mãos (o executável só tem 13 nomes de osso como texto; o resto vem do arquivo de rig), comprimentos e pose de repouso, eixos das juntas e limites. Isso define a cadeia do IK (ombro → cotovelo → pulso) e evita adivinhação.

**Resultado da pesquisa (ver PIPELINE_ANIMACAO.md):** o `IkTargetOverrides` do jogo lê alvos de parâmetros enviados pela simulação, então o caminho escolhido é o IK de dois ossos na DLL, escrito na pose antes do `EndPosingState` (o mesmo ponto do corpo congelado, já implementado na v11).

**Riscos:** o IK do jogo pode só aceitar alvos ligados a objetos ("slots") e não posições livres; ou rodar só durante certas animações. Plano B: aplicar a pose dos braços nós mesmos (IK de dois ossos calculado na DLL), escrevendo nos ossos depois da avaliação, pelo mesmo ponto de entrada dos deltas de osso.

---

## 10. Comparação com mods de referência (HIGGS e PLANCK, Skyrim VR)

Os dois são plugins SKSE para o **Skyrim VR**, que já é um jogo VR: o tracking do headset e dos
controles, o estéreo e as mãos vêm do próprio jogo. **HIGGS** acrescenta pegar/puxar/arremessar com
física (Havok); **PLANCK** acrescenta "ragdoll ativo" (corpos reagindo fisicamente).

| Camada | Skyrim VR + mods | Nosso mod (TS4) |
|---|---|---|
| Headset, controles, estéreo | o jogo | nós (OpenXR) |
| Corpo seguindo a cabeça / braços nos controles | mod VRIK | rig desacoplado ✅ · IK dos braços 🟢 |
| Física das mãos e de objetos | HIGGS | 🔴 o TS4 não tem motor de física de objetos |
| Corpos físicos | PLANCK | 🔴 idem |

O nosso alvo de tracking equivale a "Skyrim VR + VRIK". No lugar da física, a interação por toque:
mão no objeto + gatilho = abrir o menu do objeto.

## 11. Como vai ser abrir e jogar

1. Uma vez: runtime OpenXR = Meta Quest Link; `vr=1` no `fpc_rig.ini`.
2. Toda vez: conectar o Quest (Link/Air Link) → abrir o jogo normalmente → a DLL abre a sessão
   OpenXR e a imagem vai para o headset (o monitor mostra uma cópia).
3. No jogo: cabeça = olhar · analógico esq. = andar (botão = correr) · analógico dir. = virar 30° ·
   gatilho = clicar no centro da visão · UI em painel flutuante · botão de recentrar · tecla/botão
   para desligar o VR.
4. A janela do jogo precisa ficar em foco; o ASW do Link segura quedas de fps.

## 9. Ordem sugerida para começar

1. **Bolha de snap no monitor** (rápido, testa a ideia de conforto antes do VR).
2. **Pesquisa do IK / scene processors** (§8): define se mãos e corpo congelado são viáveis.
3. **V0 → V1 → V2** (infraestrutura do VR), em paralelo com a **V3** (captura do RenderDoc). Com a V3 respondida, decidimos estéreo real ou AER e seguimos para V4–V7.
