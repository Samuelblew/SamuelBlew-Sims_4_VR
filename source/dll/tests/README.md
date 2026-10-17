# Offline tests

These tests run on the desktop, with no game and no headset.

| Test | Build | What it checks |
|---|---|---|
| `teste_menu.cpp` | `build_teste_menu.bat` | Renders the VR radial menu and the wrist watch to `menu_teste.png` and `relogio_teste.png`, and checks token substitution. |
| `teste_config.cpp` | `build_teste_config.bat` | Renders every tab of the settings menu to `config_aba0..6.png`. It also checks that each value survives a save and reload through the ini, that no ini key is repeated, and that clicks on switches, choice lists and sliders work. |
| `teste_rig.cpp` | `build_teste.bat` | Tests the arm, head and leg IK on the real Sim skeleton. |

`teste_rig.cpp` needs `esqueleto_teste.h`, the bind pose of the TS4 adult rig. That data comes from the game,
so it is **not included** in this repository. Generate it from your own copy, for example by exporting the
rig with `tools/reverse_engineering/dump_rig.py`, in this format:

```cpp
struct SkB { const char* name; int parent; float p[3]; float q[4]; };   // local position and rotation (x, y, z, w), Y up
static const SkB kSk[] = { { "b__ROOT__", -1, { ... }, { ... } }, /* ... one entry per bone, parents first ... */ };
static const int kSkN = /* bone count */;
```
