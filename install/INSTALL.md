# Installing the TS4 VR mod

1. **Close the game.**
2. Copy everything in `Game/Bin/` into your game's `Game/Bin` folder, for example
   `C:\Program Files\EA Games\The Sims 4\Game\Bin\`:
   - `version.dll`: the loader. Windows loads it with the game, and it loads `TS4VR.dll`. The real system
     `version.dll` keeps working, because the loader forwards to it.
   - `TS4VR.dll`: the mod.
   - `openxr_loader.dll`: the Khronos OpenXR loader.
   - `fpc_rig.ini`: the settings. It is optional; you can change everything in game with the settings menu.
3. Copy everything in `Mods/` into `Documents\Electronic Arts\The Sims 4\Mods\`. Keep the `Resource.cfg` that
   the game creates there.
4. Start the game. Open **Game Options → Other** and enable **Enable Custom Content and Mods** and
   **Script Mods Allowed**, then restart the game.
5. Make sure your headset's OpenXR runtime is active. For Quest Link, set the Meta/Oculus app as the active
   OpenXR runtime and start Link before launching the game.
6. Load a household, select a Sim and press **INSERT** to enter first-person VR mode. Press **DEL**, or hold
   the left `≡` button, to open the settings menu.

## Uninstall

Delete `version.dll`, `TS4VR.dll`, `openxr_loader.dll`, `fpc_rig.ini`, `fpc_rig.log` from `Game/Bin`, and
`fpc_walk.ts4script` plus the `fpc_*.txt` / `fpc_walk.log` files from the `Mods` folder.

## Troubleshooting

- The mod writes a log to `Game/Bin/fpc_rig.log`, and the script mod writes one to `Mods/fpc_walk.log`.
  Include both when you report a problem.
- If the image is wrong or black in the headset, open the settings menu → **Image** → **Upscaler** and
  choose **Off (direct copy)**.
- **Game resolution presets** resize the game window, so they need the game to run in **Windowed** mode.
- After a game update the mod may stop working until its byte signatures are updated.
