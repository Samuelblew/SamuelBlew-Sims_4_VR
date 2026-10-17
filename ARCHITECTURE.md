# How the TS4 VR mod works

This page is a technical map for anyone who wants to continue the project. The source comments and the
notes in `tools/notes` go deeper, but they are mostly in Portuguese.

## Overview

```
TS4_x64.exe ──loads──> version.dll (source/loader) ──loads──> TS4VR.dll (source/dll)
                                                              │  hooks game functions found by AOB signatures (MinHook)
                                                              │  OpenXR session + D3D11 (same device as the game)
                                                              │
               Python script mod (Mods/fpc_walk.ts4script) <──┤  console commands  ("fpc.move", "fpc.ray", ...)
                    runs inside the game's simulation       ──┘  replies: "camera.fpc.idletime <code>" + Mods/fpc_*.txt
```

The game is split into a **client** (rendering, animation, UI: `TS4_x64.exe`) and a **simulation server**
(Python game logic, in the same process). The DLL owns everything client-side: camera, body pose, UI and
input. Gameplay actions (walking, interactions, picking up objects) go through the Python script mod, so
they stay valid for the simulation.

## Source map (`source/dll`)

| File | Responsibility |
|---|---|
| `fpc_rig.cpp` | Entry point and main loop. Hooks the first-person camera (`FPS_Update`, `FPS_SetParam`, `SetPose`), handles walk mode (`INSERT`) and old-style locomotion (`fpc.move` at 120 Hz), pose freezing, config loading. |
| `xr_session.inl` | OpenXR instance and session, head tracking, **synchronous stereo**: the game's frame (`R_FrameA/B`) runs twice per headset frame, once per eye, with an off-center projection. Swapchains, frame submission, `Present` hooks (shadow vtable on the game's swapchain). |
| `xr_input.inl` | OpenXR actions (Touch controllers), haptics, **remapping layer** (`g_ctrlRaw` → `g_ctrl`). |
| `xr_ui.inl` | Redirects Scaleform UI rendering (`HAL::BeginScene/EndScene`) into a separate texture. Shown on a left-hand panel and as a world-space layer that matches the game camera. Laser beam layer. |
| `xr_upscale.inl` | Backbuffer → eye image: FSR 1.0 (EASU+RCAS), CAS, bilinear (`fsr/*.hlsl`, compiled by `fxc`). |
| `xr_vignette.inl` | Comfort vignette, a head-locked quad layer. |
| `xr_cursor.inl` | Draws the Windows cursor into images that go to the headset. |
| `vr_rig.inl` | Body solver. Bone lookup (`Bone_HashName`/`Bone_FindIndex`), two-bone arm IK, head/neck, lean, legs with planted feet and steps, crouch. The pose is written in the `Pose_Commit` hook (joints: 0x30 bytes = pos, quat xyzw, scale). |
| `vr_avatar.inl` | Client-side avatar: the client places the Sim every frame (`Loc_SetTransform`), ignoring small server corrections, so the body moves smoothly. |
| `vr_grab.inl` | Grabbing other Sims (HIGGS/PLANCK-style soft IK on their limbs). |
| `vr_objgrab.inl` | Grabbing small objects (client-side transform while held, `fpc.dropobj` on release). |
| `vr_laser.inl` | Right-hand laser. The ray-cast runs in Python (`fpc.ray`), the native highlight is the client event `0x11/0x12`, and the trigger asks for the target's interactions (`fpc.choices`). |
| `vr_menu.inl` | Custom 3D radial menu, with texts resolved by the game's own localization service. |
| `vr_status.inl` | Wrist watch (needs, time, money). |
| `vr_config.inl` | In-game settings menu (GDI+ → OpenXR layer + monitor overlay), registry of every option, ini save/load. |

## DLL ↔ Python channel

- **DLL → Python:** `SendServerCommand("fpc.<command> args")`, the same path as the cheat console.
- **Python → DLL:** the script runs `client_cheat 'camera.fpc.idletime <code>'`, which the DLL intercepts in
  `FPS_SetParam`. Bulk data goes through small files in the Mods folder (`fpc_menu.txt`, `fpc_hit.txt`,
  `fpc_grab.txt`, `fpc_status.txt`).
- **Signal codes:**

  | Code | Meaning |
  |---|---|
  | -1000 / -1001 / -1002 | Sim free / busy / blocked |
  | -1005 / -1006 | Pie menu opened / option selected |
  | -1007 | VR menu ready |
  | -1009 / -1010 | Object grab OK / nothing to grab |
  | -1011 | Ray-cast result ready |

- **Interaction flow:**
  1. The laser trigger sends `fpc.choices`.
  2. The script calls `interaction_commands.generate_choices`.
  3. The `create_pie_menu_message` hook writes the options to `fpc_menu.txt` and stores
     `choice_menu.revision`.
  4. The VR menu sends `fpc.select <id>`.
  5. The script runs `client.select_interaction(id, revision)`.

## Updating after a game patch

Every hook is found by a byte signature: `tools/notes/assinaturas_AOB.txt`, plus the `SIG_*` constants in
the source. After an update:

1. Check `Game/Bin/fpc_rig.log`. Any hook that logs a null address (`0000000000000000`) has a broken
   signature.
2. Find the function again in the new `TS4_x64.exe`, using the old address, strings and xrefs as hints
   (`tools/reverse_engineering`: `tsdis.py`, `callers.py`, `xref.py`, `vtables.py`, `decomp.py` with Ghidra).
3. Rebuild a unique signature with `sigs.py` and update it in the source.
4. Struct offsets used in the code, such as actor+0x380 or comp+0x10/0x30, may move too. The log shows the
   values read at startup.

## Ideas for the future

- Fixed foveated rendering (needs NVIDIA VRS / NVAPI), NIS upscaler.
- Render the game UI with real depth, instead of the flat world layer.
- Room-scale interactions with more objects (doors, sinks...) through the simulation's interaction API.
- Support for more headsets and controller profiles (Index, Vive, WMR) in `xr_input.inl`.
