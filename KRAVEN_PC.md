# Kraven levels for the original 2002 Windows release

Goal: add the Xbox-only Bronx Zoo mission (Kraven; Xbox `ZOO_B.xbs` /
`ZOO_C.xbs`) to the retail PC game (`C:\Program Files (x86)\Activision\
Spider-Man (tm) Movie`) as **new** levels, replacing none of its files.
This is separate from the recompiled Xbox port in the rest of this repo.

Status (2026-11-05): **built and installed, not yet played.**
- Installed, new files only:
  - `data\levels\ZOO_B.pkg` and `data\levels\ZOO_C.pkg`;
  - `animatic\av_zoo.bik` and `movies\kraven.bik`, copied from the Xbox disc (plain Bink, the same on PC);
  - `data\tips\ZOO_B/ZOO_C.TXT/.FRE/.GER`.
- The game could not be launched for a test because it asks for Disk 2.
  `FUN_00825e20` is the copy-protection check: it compares the CD audio TOC in a drive against `texture.dat`.
  - Do not patch that check.
  - The user needs the real disc, or a CD image of their own disc with audio tracks in a virtual drive.

## Rebuild / reinstall

```bash
cd /c/Users/adub9/spiderman/tools && /c/Users/adub9/AppData/Local/Python/bin/python.exe pc_zoo.py C:/Users/adub9/xboxrecomp/game_files/data "C:/Program Files (x86)/Activision/Spider-Man (tm) Movie/data" C:/Users/adub9/spiderman/build_pc_zoo
```

- The build takes about 10 s.
- `build_pc_zoo\` holds the packages, the movies, the tips and the installer. It is game-derived, so never commit it.
- Program Files is write-protected. `build_pc_zoo\INSTALL_ZOO.cmd` asks for admin rights (UAC), then runs `install_zoo.ps1`.
  - The script copies only files that are not there yet.
  - To install a rebuilt package, the user must first delete the old `ZOO_*.pkg` from `data\levels`.

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

    So the Xbox zoo scripts stay self-consistent and run unchanged. Every library function they call exists in the PC exe (`libcheck` passed). Cost: the PC-only keyboard/mouse handling in the zoo's pause and end-of-level menus is missing. A controller should still work.

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

## Open work, in order
1. **Play-test** once the disc check is satisfied: level select, or finish "Escape From Oscorp". Watch for missing files, wrong folders, script errors and broken menus.
2. **Audio.** The PC wants Bink audio for:
   - Kraven's voice: `audio\characters\kraven\*.bik`;
   - the zoo music: `audio\music\bronx_zoo_main/edit1-3.bik`;
   - cut-scene dialogue: `audio\LEVELS\M7BRONXZOO\zoo_*_in/out.bik`, compare `M8BRIDGE\bri_a_in.bik`.

   The Xbox sources are in `game_files\data\xboxsound` (the WAVEBK banks; `tools\adpcm.py` decodes them). No open Bink *encoder* exists, so this needs RAD Video Tools, a download that needs the user's OK. Never edit `COMMON.pkg`.
3. Optional: graft the PC keyboard/mouse menu functions into the zoo scripts. Hard, because object function indices and data offsets would all shift.
