# Costume selector: feasibility and plan (2026-10-27)

Goal: pick Spider-Man's costume from the **pause menu**, with a **3D preview of
the selected costume** that rotates with **L1 / R1**.

Nothing is built yet. This is what the game already has, what each part would
take, and the order to build it in.

## What the game already has

**Costumes are a built-in feature, driven by the hero's name.**

- `data\heroes\` holds 15 hero models (`aux_xbox.txt` maps name → file):
  SPIDERMAN, SPIDEYWRESTLE, PETERSTUDENT, ROSSSPIDEY, PLRGOBLIN, PLRROSSGOBLIN,
  MJSPIDEY, SHOCKSPIDEY, SCISPIDEY, THUGSPIDEY, BTGSPIDEY, STHSPIDEY,
  HELISPIDEY, FREAKSPIDEY, COPSPIDEY.
- The secrets/cheats table is built at boot by `sub_000F5CA0` at guest
  `0x4BCAA0` (entries of 0x2C bytes: display name, cheat code, points, unlock
  state, description, internal hero name). Two kinds of costume:
  - **Secret Store** heroes, bought with points and toggled On/Off: Peter
    Parker (`peterstudent`), Wrestler (`spideywrestle`), Alex Ross
    (`rossspidey`), Green Goblin (`plrgoblin`).
  - **Cheat-code** costumes, entered under Specials → Cheats: `girlnextdoor` →
    mjspidey, `hermanschultz` → shockspidey, `serum` → scispidey, `knuckles` →
    thugspidey, `stickyrice` → btgspidey, `thugsrus` → sthspidey,
    `captainstacey` → helispidey, `freakout` → freakspidey, `realhero` →
    copspidey.
- **At level start, `sub_000F0690` picks the hero.** It keeps heroes a level
  forces (`peterstudent`, `fakehero`, `plrgoblin`); otherwise it applies the
  active costume and copies its name into the hero slot at
  `[game+0x11C]+0x14E8` (32 bytes). `sub_000EA580` and others compare that
  slot (e.g. against `plrgoblin`).
- **Swapping the file does not work.** Serving SCISPIDE.xbs as SPIDEYWR.xbs
  crashed at level load: each model carries its own internal names, so the
  switch has to go through the hero name, as the game's own cheats do.

**Menus are script-driven.**

- `MENU.xbs` (third zlib stream) holds the front end's screen list as text:
  `SPECIALS_SCREEN specials_screen COSTUMES_SCREEN WARP_SCREEN CREDITS_SCREEN
  CHEATS_SCREEN TUTORIAL_SCREEN STORE_SCREEN`. So there is a **COSTUMES_SCREEN**
  (`costumes_screen`), but Specials shows only Credits, Cheats, Training,
  Secret Store and Level Warp today. It probably appears once a cheat costume
  is unlocked (not checked).
- The pause menu (seen in Warehouse Hunt): **Audio Options, Restart Level,
  Options, Combat Controls, Exit to Menu**, on the red spider background. Each
  level's compiled scripts (third zlib stream of every level `.xbs`) contain
  `pause`, `restart_level`, `dialog_open` / `dialog_select` and the "Restart
  at the beginning of level?" dialog. The item labels themselves were not
  found as text in any game file or the XBE, so where the list is built is
  still unknown.
- **The UI can show 3D models.** The script widget set includes
  `create_entity_widget(str)`, `set_entity_rotation(num,num,num)`,
  `scale_to(num)`, used by the level-complete bonus screen (`ib_spiderman`).

## The four parts

### 1. Costume switch: feasible, low risk

Use the game's own mechanism: from host code, set the chosen costume as the
active one (the secrets table's unlock/active state), or write its name into
the hero slot when `sub_000F0690` runs. The engine then loads the model,
animations and names consistently, as it does for the cheats.

- First: confirm which table field means "active" (unlock one cheat costume
  in game and diff the table), and whether level-forced heroes (Peter, Goblin
  levels) must be left alone, as `sub_000F0690` already does.
- Settable from the launcher too (a Costume dropdown), as a fallback.

### 2. Changing costume mid-level: apply on restart, not live

The hero is built when the level loads; rebuilding him mid-level (model,
skeleton, animation sets, script references to the hero entity) is not
realistic. The pause menu already has **Restart Level**, and the scripts have
`restart_level`: choosing a costume can set it and restart the level (with the
game's own "Restart at the beginning of level?" confirmation). That matches
how the game's cheats behave.

### 3. A Costume entry in the pause menu: two routes

- **3a. Native item.** Find the code that builds the pause list (engine code
  or level script bytecode) and add "Costume" plus its handler. Most
  authentic; effort unknown until the builder is found. If it is level
  script, the change has to be made in all 25 level files' compiled scripts
  (or in the script VM at run time), which needs the bytecode format worked
  out.
- **3b. Recomp-drawn panel.** While the game is paused, a button opens our own
  panel drawn over the frame by `gpu_backend.c`, styled to match (the game's
  font and red-spider art are textures we can already decode). Fully under
  our control; medium effort; looks close to, but is not, the game's menu.

Recommendation: start with 3b, and switch to 3a if finding the pause list
builder turns out to be cheap.

### 4. 3D preview rotated with L1 / R1: the biggest part

Only the current hero's model is in memory during a level; the other
costumes' `.xbs` files (1.5 MB each) are not loaded.

- **4a. Engine entity widget.** The engine can draw an entity in the UI and
  rotate it (`set_entity_rotation`). It would need the selected costume's
  model loaded mid-level and a widget created from host code. Possible but
  deep: loading a second hero package while a level runs is untested
  territory.
- **4b. Our own model viewer.** Decode the hero `.xbs` format (meshes,
  skeleton, textures; textures already decode through the toolkit) and draw
  the model in a standing pose with D3D11 in the 3b panel. L1 / R1 turn it.
  Fully under our control. The format must be reverse-engineered first:
  the largest single task here.
- **4c. Turntable images.** Render each costume once at N angles, ship the
  images, and L1 / R1 step through them. Looks 3D and is cheap at run time,
  but still needs 4a or 4b once to make the images.

Recommendation: 4b, after 1-3 work.

## Order

1. **Costume switch** (part 1), applied at level load. Test with the
   launcher dropdown. Small.
2. **Pause panel** (3b) with the costume list and "apply and restart level"
   (part 2). Medium.
3. **3D preview** (4b): decode the hero model format, draw it in the panel,
   L1 / R1 rotate. Large.
4. Optional: replace the panel with a native pause-menu item (3a).

## Status

- **Part 1 done (2026-10-27, 13f25b1).** `sub_000F0690_enter` in
  src/recomp_manual.c sets the chosen costume's byte at level start
  (settings.ini `[Game] Costume`, launcher "Costume" list, or
  `RECOMP_COSTUME`). Tested: scispidey in Search for Justice.
  `sub_000F0690` runs when the menu loads and at each level start reached
  from the menus (Level Warp, Start); the Continue path was not seen in a
  run with the hook, so check it next. Note the earlier run where Continue
  into Warehouse Hunt kept the wrestler with Peter on.
- Next: part 2/3 (pause panel + apply and restart), then part 4b.

## Checks done (2026-10-27)

Watched with `RECOMP_PEEK` (src/main.c; follows pointers, see its comment).

- **The active costume is one byte per secret, one at a time.** Profile
  object = `[[0x4BC614]+0x11C]` (0x012BAAC0 in that run); byte
  `profile + 0x1544 + i` is 1 for the active entry `i` of the secrets table.
  Entering `SERUM` set i = 9 (scispidey). Setting Peter Parker On in the
  Secret Store set i = 3 and cleared 9 in the same frame. The secrets table
  at `0x4BCAA0` itself never changes after boot.
- **The level loader honours it.** With Peter on, Level Warp → Warehouse Hunt
  loaded `ORIGIN_B.xbs` with `PETERSTU.xbs` instead of the wrestler suit.
  So part 1 is: write that byte (and clear the others) before a level loads.
- **No Costumes screen appears.** With SERUM active, Specials still shows
  Credits, Cheats, Training, Secret Store, Level Warp. COSTUMES_SCREEN in
  MENU.xbs is unreachable (cut or PS2-only), so there is no built-in 3D
  preview to reuse; part 4 stays 4b.
- **Secret Store's box is text only** ("Play the game as Peter Parker."), no
  model.
- Entry indices (0x2C bytes each from 0x4BCAA0): 0 Bowling, 1 Vulture movie,
  2 Shocker movie, 3 Peter Parker, 4 Wrestler, 5 Alex Ross, 6 Green Goblin,
  7 mjspidey, 8 shockspidey, 9 scispidey, 10 thugspidey, 11 btgspidey,
  12 sthspidey, 13 helispidey, 14 freakspidey, 15 copspidey, then
  Unlimited Webbing and the other cheats (check the tail before use).

Side finding: with RECOMP_DISPATCH_GATE=1 the game twice stuck on
"Autoloading Spider-Man saved game data" at boot (same save, after it was
rewritten at 15:56); with the gate off it loaded. Four earlier gate runs got
past it. Needs looking at before the gate can stay on (see PORT_NOTES.md).
