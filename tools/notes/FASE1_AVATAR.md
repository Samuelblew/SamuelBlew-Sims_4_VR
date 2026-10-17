# Fase 1: o Sim do jogador posicionado pelo cliente (v40)

## Como o cliente posiciona objetos (engenharia reversa)

| Função | Endereço | O que faz | |
|---|---|---|---|
| `Op_SetLocation` | `0x1401f4cd0` | tratador da operação de rede "SetLocation" (servidor → cliente); cria/lê a mensagem `SetLocation@Network@Sims4@EA` | [O] |
| `GetComponent(obj, tipo)` | `0x140939280` | componente pelo tipo (máscara em `obj+0x10`, tabela em `obj+0x28`) | [O] |
| tipo do componente de localização | lido em `0x1401f4d1b` (`mov edx,[rip+X]`) | `DAT_1424fb990` | [O] |
| `Loc_Begin(comp)` | `0x1409412e0` | `comp+0x90 |= 1` (abre o lote) | [O] |
| `Loc_SetTransform(comp, xf)` | `0x140942ae0` | `xf = {quat x,y,z,w ; pos x,y,z,w}` → `comp+0x10` (quat), `comp+0x30` (pos); marca mudanças | [O] |
| `Loc_Commit(comp)` | `0x1409421a0` | fecha o lote; se mudou, evento 3 (`FUN_14094d550`) → ator/cena | [O] |
| também no tratador | `FUN_140942910` (pai/junta), `FUN_140942c50` (superfície de rota), `FUN_140942da0`, `FUN_140941f00` | anexos, andar/nível | [O] |

Quaternion padrão em `0x141b0bba0` = (0, 0, 0, 1) → ordem x, y, z, w. Giro: `(0, sin(a/2), 0, cos(a/2))`, `a = atan2(fx, fz)` (mesma convenção de `sims4.math.vector3_angle`).

## Desenho

- Modo FPS livre: a cada quadro, `Begin → SetTransform → Commit` no componente do nosso Sim, com a posição da câmera (origem) e o giro suave do corpo. Altura sempre a do servidor (chão/andar).
- O servidor continua recebendo `fpc.move` (navmesh, paredes, posição da simulação).
- Hook em `Op_SetLocation` + `Loc_SetTransform`: para o nosso Sim, a posição que chega do servidor vira só referência (`g_serverPos`); um salto maior que `avatar_ressinc` (escada, porta, o jogo movendo o Sim) é aceito e a câmera vai junto.
- Coleira e recuo na parede medem pela posição do servidor (o corpo visível é do cliente).
- Saindo do modo livre (interação), o Sim volta para a posição do servidor e o jogo retoma.

## Próximo (Fase 2)

Pernas: com o corpo deslizando no cliente, falta animação de passos. Plano: IK de corpo inteiro com passos procedurais (pés plantados, passo quando o corpo se afasta), guiado pela velocidade do avatar.
