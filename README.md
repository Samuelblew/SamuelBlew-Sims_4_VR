# The Sims 4 — First-Person VR Mod

An unofficial mod that turns The Sims 4's first-person camera into a full **OpenXR VR** experience.
It was built and tested with a **Meta Quest 2 over Quest Link** on game version **1.118.257.1020**.

It is the first mod of its kind for The Sims 4: you are inside your Sim. Your head and hands move the
Sim's head and arms, you walk around the lot, and you point at objects and other Sims to interact with them.

> **Status:** personal project / experimental. It hooks the game's code by byte signatures, so a game update
> can break it until the signatures are refreshed (see `tools/notes/assinaturas_AOB.txt`).

## Features

- **Stereo VR rendering:** each eye is rendered separately by the game. Optional upscaling uses
  **AMD FSR 1.0**, **AMD CAS** or bilinear filtering, with supersampling and FSR-style presets
  (77 / 67 / 59 / 50 %).
- **Head and hand tracking drive the Sim:**
  - arms (IK), fingers (trigger and grip), head and torso lean;
  - real crouching;
  - planted feet that take steps on their own.
- **Locomotion:** walk with the left stick, smooth or snap turning with the right stick, optional comfort
  vignette.
- **UI:**
  - the game UI is moved onto a panel on your **left hand**;
  - speech bubbles and labels stay in the world;
  - a laser on the **right hand** selects objects and Sims;
  - a custom 3D **radial menu** for interactions;
  - a **wrist watch** with needs, time and money.
- **Grabbing:** grab other Sims (HIGGS/PLANCK-style soft IK) and small objects.
- **In-game settings menu:** press **DEL**, or hold the left **≡** button. It covers image, world/camera,
  comfort, interface, body, **VR controller remapping** and **keyboard remapping**. Changes apply live and
  are saved to `fpc_rig.ini`.

## How it works

The game has its own first-person camera, but it is a flatscreen feature. The mod is a native DLL that hooks
the game in memory and adds the VR systems the game doesn't have:

1. **Loading:** a small `version.dll` proxy, loaded by the game from its own folder, loads `TS4VR.dll`.
   `TS4VR.dll` finds the functions it needs in `TS4_x64.exe` by byte signatures and hooks them with MinHook.
2. **Rendering:** the mod opens an OpenXR session on the game's own Direct3D 11 device. Every headset frame,
   it makes the game render the scene twice, once from each eye's pose with that eye's off-center
   projection. Each image is upscaled (FSR 1.0 / CAS) into the headset's swapchain.
3. **UI:** the game's Scaleform UI is rendered into a separate texture instead of on top of the scene. It is
   shown on a panel on your left hand, and in-world elements (speech bubbles, labels) are placed back in the
   world.
4. **Body:** every frame, the mod writes the Sim's final pose (arms with IK, head, torso, legs with planted
   feet, crouch) before the game commits it to the renderer. The Sim's position is driven client-side, so
   walking is smooth.
5. **Gameplay:** walking, the laser ray-cast, interactions, the wrist watch data and grabbing objects go
   through a small Python script mod that runs inside the game's simulation. Every action is a real game
   action, so saves and autonomy stay consistent.

For a file-by-file map, see [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Requirements

- The Sims 4 (PC, 64-bit, DirectX 11), a legitimately owned copy.
- A PC VR headset with an **OpenXR** runtime. Tested: Meta Quest 2 with the Oculus/Meta runtime over Link.
- Windows 10/11.

## Install

1. Download this repository: **Code → Download ZIP**.
2. Close the game. Copy everything in `install/Game/Bin/` into your game's `Game/Bin` folder:
   - `version.dll`
   - `TS4VR.dll`
   - `openxr_loader.dll`
   - `fpc_rig.ini`
3. Copy `install/Mods/fpc_walk.ts4script` into `Documents/Electronic Arts/The Sims 4/Mods/`.
4. In the game: **Game Options → Other**. Enable **Enable Custom Content and Mods** and **Script Mods
   Allowed**, then restart the game.
5. Start your headset's OpenXR runtime first. For Quest Link, set the Meta/Oculus app as the active OpenXR
   runtime and connect Link.
6. Load a household, select a Sim and press **INSERT**. You are now in first-person VR.

Uninstalling, troubleshooting and more details: [`install/INSTALL.md`](install/INSTALL.md).

## Default controls

| Action | VR | Keyboard |
|---|---|---|
| First-person / VR walk mode | — | `INSERT` |
| Walk / turn | left stick / right stick | `WASD` + mouse |
| Run | click left stick (toggle) | hold `Left Alt` |
| Laser (point at objects and Sims) | hold right grip | — |
| Select / open interactions | right trigger | — |
| Back / close menu | `B` | — |
| Hand UI panel | look at your left hand + left grip | — |
| Recenter view | click right stick | `,` |
| Virtual screen (panel mode) | tap left `≡` | `.` |
| **Settings menu** | **hold left `≡`** | **`DEL`** |
| Hide / show the Sim's body | — | `END` |
| Grab a Sim or a small object | bring your hand close + grip | — |
| Wrist watch (needs, time, money) | turn your left wrist toward your face | — |
| Arm test pose (debug) | — | `Ç` (ABNT2 keyboard) |

You can remap all of these in the settings menu, in the **VR Controls** and **Keyboard** tabs.

## Repository layout

| Folder | Contents |
|---|---|
| `install/` | Ready-to-use files: the loader (`version.dll`), the mod (`TS4VR.dll`), the OpenXR loader, the default `fpc_rig.ini` and the Python script mod. |
| `source/dll/` | C++ source of the mod DLL (MinHook, OpenXR, D3D11, GDI+). Build it with `build.bat` (VS 2022 Build Tools + Windows SDK `fxc`). |
| `source/loader/` | The tiny `version.dll` proxy that loads the mod. It forwards to the real Windows `version.dll`. |
| `source/script_mod/` | The in-game Python script mod (`fpc_walk.py`), compiled for the game's Python 3.7. |
| `tools/` | Reverse-engineering helper scripts, the optional crosshair-package generator and development notes. |

**Want to contribute?** Start with [`ARCHITECTURE.md`](ARCHITECTURE.md): it explains how the mod works and how to update it
after a game patch. Developer notes in `tools/notes` and comments in the source are mostly in Portuguese.

## Building

1. Mod DLL: `source/dll/build.bat` builds `source/dll/out/fpc_rig.dll`. Rename it to `TS4VR.dll` when you
   install it.
2. Loader: `source/loader/build.bat` builds `out/version.dll`.
3. Script mod: `python source/script_mod/build.py C:\path\to\python37\python.exe` builds
   `fpc_walk.ts4script`. It must be compiled with **Python 3.7**, the same version the game uses.
4. Offline tests: `source/dll/tests/build_teste_menu.bat` and `build_teste_config.bat` render the menus to
   PNG images and check them. The rig test needs a skeleton header generated from your own game files,
   which is not included.

## Legal / disclaimer

- This is a free, non-commercial fan mod. It is not affiliated with or endorsed by Electronic Arts or Maxis.
  "The Sims" is a trademark of Electronic Arts Inc.
- No game files, decompiled game code or game assets are included in this repository. Everything here is
  original code, third-party open-source code (see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)) or
  tools that work on **your own** installed copy of the game.
- The mod changes the game's code in memory. It is meant for single-player use. Use it at your own risk and
  back up your saves.

## License

The original code is released under the MIT License (see [`LICENSE`](LICENSE)). Third-party components keep
their own licenses.
