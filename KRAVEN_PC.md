# Kraven levels for the original 2002 Windows release

Goal: add the Xbox-only Bronx Zoo mission (Kraven; Xbox `ZOO_B.xbs` /
`ZOO_C.xbs`) to the retail PC game (`C:\Program Files (x86)\Activision\
Spider-Man (tm) Movie`) as **new** levels, replacing none of its files.
This is separate from the recompiled Xbox port in the rest of this repo.

Status (2026-11-06): **complete; the user played it through.** Level load,
cut-scenes (skippable), Kraven, voice and music, physics, the sniper glass,
and the pause, results and game-over menus all work. ZOO_C ("The Mighty
Hunter") shares every fix but got less play.

Installed in the game folder:
- `data\levels\ZOO_B.pkg`, `ZOO_C.pkg` (new);
- `animatic\av_zoo.bik`, `movies\kraven.bik`, `data\tips\ZOO_B/ZOO_C.*` (new);
- 60 audio files from `tools\pc_zoo_audio.py` (Kraven's voice, zoo music, cut-scene dialogue; new);
- `data\COMMON.pkg` with `SOUNDS\GLOBAL.SND` declaring them (backup `COMMON.pkg.original.bak`);
- `SpiderMan.exe` with the zero-dt patch (`tools\pc_nan_guard.py`; backup `SpiderMan.exe.before-nan-guard.bak`).

## Rebuild / reinstall

```bash
cd /c/Users/adub9/spiderman/tools && /c/Users/adub9/AppData/Local/Python/bin/python.exe pc_zoo.py C:/Users/adub9/xboxrecomp/game_files/data "C:/Program Files (x86)/Activision/Spider-Man (tm) Movie/data" C:/Users/adub9/spiderman/build_pc_zoo
```

- The build takes about 10 s.
- `build_pc_zoo\` holds the packages, the movies, the tips and the installer. It is game-derived, so never commit it.
- Program Files is write-protected. Each installer in `build_pc_zoo\` runs under UAC and keeps a backup:
  - `update_zoo.ps1`: ZOO_B/ZOO_C.pkg (old ones -> `.pkg.prev.bak`). Use this after every rebuild; the game must be closed.
  - `install_audio.ps1` / `update_audio.ps1`, `install_common.ps1`, `install_exe.ps1`.
- **Installer for other copies of the game:** `tools\installer\build_installer.py` builds `build_pc_zoo\KravenModInstaller.exe`.
  - It is one 132 MB file with the 70 mod files embedded, compiled with Windows' own .NET 4 `csc`.
  - The exe is game-derived: never commit or publish it.
  - What it does (details in the header of `KravenModInstaller.cs`):
    - finds the game folder; asks for UAC;
    - copies the files;
    - switches on the 5 zoo dialogue lines in whichever `COMMON.pkg` is there, appending the new GLOBAL.SND text and leaving the rest of the package unmoved;
    - applies the physics fix by byte signature, so it works on any exe build with that code and skips a build without it;
    - opens the game.ini level-select gate.
  - Everything is backed up to `<game>\KravenMod\backup`, and `uninstall` restores it.
  - It leaves the community fix files (Fix Enhancers' d3d8.dll, dxwrapper, Xidi) alone. It does not include the Disk 2 check change.
  - Tested on a copy of the original files: install, reinstall, uninstall back to identical bytes, an exe already fixed, a COMMON.pkg already switched on.
- `tools\pc_debuglog.py` runs the game under a passive debugger. It writes `build_pc_zoo\kraven_diagnostic.log` with script errors (`DEBUG ...`), crashes (EIP, stack, frames) and traced library calls.

## What was found (all verified)

**The exe needs no change.**
- `SpiderMan.exe` still has both levels in its level table: VA `0x8D1ED8`, pairs of (level path, pkg).
  - Index 14 = `M7bronxzoo\bronxzoo_b` / `levels\zoo_b`; index 15 = zoo_c.
  - Both sit between oscorp_e (13) and bridge_a (16).
- The PC scripts' level-select lists already include "Kraven's Test" and "The Mighty Hunter".
- The PC `COMMON.pkg` `SOUNDS\GLOBAL.SND` already declares the Kraven voice lines (`audio\characters\kraven\krbzb*`, `kraiv*`) and the zoo music (`audio\music\bronx_zoo_*`).
- PC exe mapping: VA = file offset + 0x400000.
- The Ghidra decompile is `cap\pc2002\pc_decomp.c`.

**PC .pkg archive (`tools\pc_pkg.py`).**
- Header: `u32 dirs, u32 files, u32 data_size`.
- Then each dir: `name[0x40], u32 n, n x u32 file index`.
- Then each file: `name[0x28], u32 size, u32 offset`. **Size comes before offset.**
- Data = the last data_size bytes of the file. Offsets count from the start of the data.
- A file may be listed in more than one dir.

**Per file type, Xbox vs PC.** Checked over all 24 levels both versions share.
- Byte-identical:
  - ANML, COLL, ENT, ENX, AI, ANI, FE, IFL, PATH, SCN, SNML, TBB, TERX, TXTMESH, VOQ;
  - SIN: 22 of 24 identical.
- Converted:
  - `.dds` -> `.TGA` (`tools\dds2tga.py`). Type-2 uncompressed, bottom-up, 24-bit, or 32-bit with descriptor 8 when there is alpha, plus the TGA 2.0 footer. The zoo uses DXT1/3/5, A8R8G8B8 and R5G6B5, all linear. Error vs the PC originals is about 2-4/255.
  - `.xbmesh` -> `.PCMESH` (`tools\pcmesh.py`, full layout in its docstring). Checked by converting the Xbox copy of every PC level mesh: same vertices, UVs, materials, bones and skin weights.
    - Strips come out longer than the PC's (it re-stripped them), but they are valid.
    - Skinned meshes (Kraven, the zoo snakes) use 0x30-byte PC positions: pos, normal, 4 x u16 global bone, 4 x f32 weight.
    - Xbox palettes are u16. A vertex's bone bytes index the palette of its sub-mesh.
    - Xbox VB header (0x28 bytes): `+0 verts, +0x10 count, +0x14 strip pointer, +0x24 strip length`. Meshes can share one buffer.
  - `.sx` / `.sxl` (level scripts, `tools\sx.py` reads/writes `.sx`). These differ on PC only because PC added keyboard/mouse menu functions to shared script objects (pause_menu, bonus_reward_screen, level_ender, ...). The bytecode calls:
    - engine library functions **by name** (`0a 04 <class str> <func str>`, both local string indices);
    - script-object functions by index (`09 05 <obj str> <idx>`);
    - data by offset (`07 1c`, `08 1d` ...).

    Every library function the zoo scripts call exists in the PC exe. But the Xbox menus froze on PC (see below), so `tools\pc_menus.py` grafts the PC menu code in.

**Folders inside the package.**
- The Xbox package keeps names only (hashed), so `pc_zoo.py` follows the PC conventions:
  - level files -> `M7BRONXZOO\`;
  - entities, meshes and collision -> `M7BRONXZOO\ENTITIES\`;
  - textures -> `M7BRONXZOO\TGA\`;
  - Kraven (`kraven*`, `kr*`, his weapons) -> `CHARACTERS\KRAVEN\` (`\TGA\`);
  - `ib_*` -> `INTERFACE\TGA\`; `.ai` -> `AI\`; `.fon` -> `INTERFACE\FONT\`.
- The zoo scripts' own paths (`m7bronxzoo\entities\...`, `m7bronxzoo\tga\...`) agree with this.
- Files already in `COMMON.pkg` are skipped. Files found in another PC level reuse that PC copy and its folder.
- **Not verified in game.** If a file is reported missing, the folder rule is the first suspect. Listing the file under extra dirs costs nothing.

## Fixes made in play-testing (2026-11-05/06)

Each is in `pc_zoo.py` unless named otherwise; the docstrings there have the detail.

- **Button font crash.** The Xbox scripts have no PC case and fall through to a button font the PC lacks; that string now names the PC font.
- **Explosion, Spider-Man on fire, death at load.** Every trap made its trigger before Spider-Man left his spawn point, where his height was still NaN, and a NaN position is inside every trigger. The trap setup functions now wait `TRAP_DELAY` (1 s) first.
- **NaN physics (Kraven drawn as legs only, instant "mission complete").** `FUN_004C5060` at 0x4C54D9 rebuilds velocity as displacement x 1/dt, which is NaN on a zero time step. `tools\pc_nan_guard.py` patches the exe to skip that step when dt is 0. A broad NaN clamp was tried first; it broke gravity and jumping.
- **Unskippable in-game cut-scene.** The Xbox scenes wait for CONTROLLER_1 X/START signals, which PC scripts never receive. The second half of each check now listens on KEYBOARD_1 KEY_DOWN, which is how a pad reaches the scripts on PC.
- **Voice lines and music.** These are Bink 1 files made with RAD Video Tools using `/BinkAudio1Deprecated` (the newer audio codec crashes the 2002 binkw32), declared in COMMON.pkg's GLOBAL.SND. Xbox ADPCM is decoded by `tools\adpcm.py`; ffmpeg drops a sample per block.
- **Sniper-walkway glass drawn solid.** `glass_fix` gives those materials the layout of the PC's own glass (OSCORPBX `obx_glasspane`, flags 0x49891):
  - the cut-out texture goes in slot 1, with that slot's mode 2 (alpha) and strength 255;
  - flags become 0x10|0x800 for slot 1;
  - bit 0x01 is set. Every PC see-through material has it, and it is what puts them in the sorted pass after the solid world.

  Each texture slot is a 0x20-byte name, then (u32 mode, u32 strength).
- **Pause, results and game-over menus ignored the pad.** The PC menus clear KEYBOARD_1, call `clear_keyboard_input()` and hook KEY_DOWN to `key_down_*` handlers. The Xbox ones listen only to CONTROLLER_1 signals. `tools\pc_menus.py` brings in the PC menu code from OSCORP_B:
  - pause_menu, bonus_reward_screen, fast_times_display and level_ender are replaced whole;
  - 70 menu and dialog library functions in the level object get the PC body;
  - 10 PC-only functions are added, among them `key_down_quit_retry`.

  Lessons from its two crashes:
  - **Overloads.** Functions are identified by name *and* parameter types, because the library overloads some (`move_selection_icon`, `quit_retry_screen`, `window_construct`, ...).
  - **Parameter types.** These are string numbers too, and must be remapped into the target's string table.
  - **Global offsets.** Offsets into the level object's statics are mapped by pairing shared functions. Offsets used only by the PC get a copy of the PC block appended.
  - **Linking.** At load the engine links each function's bytecode in place (`FUN_007E6AE0`): mode 8 becomes statics base + offset, mode 9 a function pointer, and modes 10/11 resolve a library class.

## Open work
1. The level-select titles that turned green, which the user reported once, were never looked at.
2. Give ZOO_C ("The Mighty Hunter") a full play-through.
