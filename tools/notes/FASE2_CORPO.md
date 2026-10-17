# Fase 2: corpo inteiro guiado pelo VR (v41)

Código: `exemplo_hook/vr_rig.inl` (`SolveLean`, `SolveLeg`, `SolveLegs`), escrito no `Pose_Commit` do nosso Sim.

## Peças

| Peça | Como | Ajuste no ini |
|---|---|---|
| Encaixe modelo→mundo | lateral = coxas, cima = vertical, raiz pela posição do `ROOT` (imune a agachar/inclinar/deslocar) | |
| Agachar | altura do headset vs. a de quando você recentrou → desce o `b__ROOT_bind__` (mesmo caminho do deslocamento do corpo); a câmera desconta | `vr_agachar` |
| Inclinar | deslocamento horizontal da cabeça em relação ao corpo (até 25 cm) → Spine0 e Spine1 giram metade cada | `vr_inclinar` |
| Andar no quarto | cabeça além de 25 cm do corpo → o corpo (avatar no cliente) é puxado junto e a referência do headset acompanha | |
| Pés plantados | cada pé guarda posição e giro no chão; não se mexe enquanto plantado | |
| Passos | pé longe do ideal (sob o quadril + velocidade × 0,12 s) mais que `passo_distancia`, ou giro > ~35° → passo em arco (`passo_altura`), um pé por vez (correndo, o outro começa na metade) | `passo_distancia`, `passo_altura` |
| IK da perna | coxa→canela→pé, joelho para frente (eixo aprendido da animação), pé com o giro do pé plantado | `vr_pernas` |

## Testes fora do jogo (`teste/teste_rig.cpp`, esqueleto real)

- Andar 1,8 m a 1,8 m/s e parar: 6 passos, tornozelo nunca abaixo de 0,11 m, pés terminam sob o quadril (≤ 4 cm).
- Agachar 30 cm: quadril desce 0,300 m, tornozelos ficam a 0,110 m do chão, coxa mantém 0,484 m.
- Inclinar 20 cm para frente: pescoço vai 0,165 m para frente.
