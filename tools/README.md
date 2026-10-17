# Tools

Helper scripts used while developing the mod. They all work on **your own** installed copy of the game.
None of the game's files or code is included here.

## `reverse_engineering/`

Python scripts that analyze `TS4_x64.exe`. They need `capstone` for disassembly. Several of them have the
game path hard-coded near the top (`D:/Games/The Sims 4/...`); change it to your install path.

- **Disassembly and functions:** `tsdis.py`, `ts4dis.py`, `pdata.py`, `where.py` (finds the function that
  contains an address, from `.pdata`).
- **Cross-references:** `callers.py`, `xref.py`, `fieldrefs.py`, `find_str_users.py`.
- **Classes and vtables (RTTI):** `classes.py`, `vtables.py`.
- **Signatures:** `sigs.py` builds unique AOB signatures with wildcards for a list of functions.
- **Hashes:** `hashes.py`, `brute.py`, `hfind.py` (the game's FNV hashes of names).
- **Game packages:** `dbpf.py` (minimal DBPF/.package reader), `swf.py` (GFX/SWF parser), `scan_vc.py`,
  `scan_vc2.py` (find UI movies by class name).
- **Ghidra:** `decomp.py` plus `ghidra_scripts/DecompileList.java` decompile single functions headlessly
  with a local Ghidra project.
- **Other:** `analisar_frame.py` reads a RenderDoc XML export; `dump_rig.py` exports armatures from a
  Blender file.

Decompiled output, frame captures and game assets produced with these tools must stay on your machine. Do
not commit them.

## `crosshair_package/`

`gerar_sem_mira.py` builds `FPC_SemMira.package` from **your own** `UI.package`. The package hides the
game's first-person virtual-cursor circle. Example: `python gerar_sem_mira.py "<game>\Data\Client\UI.package" <out dir>`.
It is optional, and it is distributed as a generator instead of a ready package because the package
contains a modified copy of a game file.

## `notes/`

Development notes, mostly in Portuguese:

- the animation pipeline and VR rendering;
- the UI and settings;
- the AOB signature list (`assinaturas_AOB.txt`).
