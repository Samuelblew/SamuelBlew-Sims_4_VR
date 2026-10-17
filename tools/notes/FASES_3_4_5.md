# Fases 3, 4 e 5 (v43)

## Fase 3: menu radial 3D próprio (`vr_menu.inl`, Python `fpc.vrmenu` / `fpc.select`)

| Peça | Engenharia reversa / desenho |
|---|---|
| Opções | Python embrulha `interaction_commands.create_pie_menu_message(sim, choice_menu, reference_id, ...)`; com o menu VR ligado grava `Mods/fpc_menu.txt` (id, hash do texto, desabilitada, categoria, hash da categoria, prioridade, tokens), esvazia `msg.items` (a roda do Scaleform não aparece) e avisa `-1007` |
| Campos | descritor `InteractionOps.proto`: `PieMenuItem{id, loc_string, category_key, disabled_text, pie_menu_priority, ...}`, `PieMenuCreate{items, client_reference_id, server_reference_id}` |
| Texto | serviço de tradução do jogo: `FUN_140254ec0()` (getter, achado pela chamada em `+0x31` do formatador `0x140d8a5f0`) + `FUN_140257050(svc, hash, &eastl_string, tokens, n, 0)`; tratador ActionScript `GetLocalizedString` = `0x140d22620` |
| Tokens | `{N.SimFirstName}`, `{N.SimLastName}`, `{N.SimName}`, `{N.String}`, `{N.Number}`; `{M..}{F..}` → variante M |
| Desenho | GDI+ 1024×1024, categorias → submenus, 8 por página + "Mais…", centro = título/voltar, desabilitadas em cinza |
| Escolha | laser + gatilho → `fpc.select <id>` → `select_choice(id, reference_id)` do jogo; B volta/fecha |

## Fase 4: relógio de pulso (`vr_status.inl`, Python `fpc.status`)

Python a cada 1 s grava `Mods/fpc_status.txt`: nome, humor (hash → traduzido pelo jogo), hora, dia, dinheiro (`family_funds.money`), necessidades (`commodity_tracker`, `motive_*`, normalizadas 0..1). Painel GDI+ 640×400 no pulso esquerdo, visível quando as costas da mão (grip +X) estão viradas para o rosto e você olha para ele.

## Fase 5: agarrar objetos (`vr_objgrab.inl`, Python `fpc.grabobj` / `fpc.dropobj`)

Grip sem Sim perto → `fpc.grabobj x y z` → Python escolhe o objeto carregável/pequeno mais próximo (≤ 0,30 m) → `-1009` + id em `Mods/fpc_grab.txt`. O cliente acha o objeto pelo gerenciador de objetos do cliente (mesmo caminho do nosso Sim) e o posiciona na mão a cada frame (`Loc_Begin/SetTransform/Commit`, Fase 1); as posições do servidor para ele são ignoradas enquanto estiver na mão. Soltar → `fpc.dropobj` → o servidor pousa no chão do andar, com o giro vertical.

## Testes fora do jogo

- `teste/teste_menu.cpp`: tokens ("Conversar com Bella", "Ele tem 3 amigos"), botões/acerto/submenu, imagens `menu_teste.png` e `relogio_teste.png`.
- `teste/teste_rig.cpp`: braços, cabeça, dedos, corpo deslizando, agarrar Sims, pernas/agachar/inclinar.

## Não feito

- Fase 1, parte 2 (controlador de câmera totalmente próprio, fora da primeira pessoa do jogo): hoje a câmera ainda é a da primeira pessoa do jogo, sobrescrita pelo mod.
- Física de objetos (o objeto solto pousa direto no chão; não cai nem fica em cima de mesas).

## Fase 3b (v44): seleção própria pelo laser (hover + clique)

- **Raycast próprio** (Python `fpc.ray`, ~15/s): raio 3D da mão contra Sims (cilindro r 0,32 m × 1,85 m), móveis (prisma sobre o `footprint_polygon`, 1,1 m), objetos sem footprint ou em cima de outros (cilindro r 0,18 m) e o chão do andar. Devolve id, tipo e ponto de impacto (`Mods/fpc_hit.txt`, aviso `-1011`). Testado fora do jogo com Python 3.7.
- **Brilho de hover nativo**: o tratador `MouseOver` da interface (`0x14022d580`) faz `FUN_14094d550(0x11 acende / 0x12 apaga, objeto_do_cliente, estilo)`; estilo 2 = MouseOver. A DLL faz o mesmo no alvo do raycast. O raio desenhado termina no ponto de impacto.
- **Clique**: gatilho → `fpc.choices id tipo x y z` → `interaction_commands.generate_choices(target_id, PickType, x, y, z, lot_id, level, ..., reference_id)` do servidor → a roda vai para o menu VR.
- **Telefone e cliques de mouse** não viram menu VR (só a roda pedida pelo laser nos últimos 3 s).
- Removido o caminho antigo: câmera do jogo no laser e clique pelo cursor virtual. Em VR o cursor virtual fica estacionado fora da tela (o jogo não escolhe nada pelo centro).
