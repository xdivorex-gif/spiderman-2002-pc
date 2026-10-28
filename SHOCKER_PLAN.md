# Shocker mode: feasibility and plan (2026-10-27)

Goal: when the player switches to the Shocker, play **as the Shocker**: his
body, his animations and his moves (shock punch, gauntlet bolts, ring blasts,
spin blast, shock jump), not Spider-Man in the "shockspidey" suit.

Nothing is built yet. This is what the game files hold, how it could be put
together, and the order to try it in.

## What the game has

**The hero is a data package, almost all of it text.** A hero `.xbs`
(`data\heroes\`, mapped by `aux_xbox.txt`) is a 0x40-byte header (stream
offsets and sizes) and three zlib streams: stream 0 binary records (XBXM
meshes, ANMX animations, DDS textures), stream 1 text, stream 2 a directory
of 64-byte entries keyed by packed names. The packed names decode losslessly
(4 bits length + 10 six-bit characters per 64-bit chunk), so every entry can
now be listed by name (scratch `names.py`; to go into tools/xbxm.py).

Spider-Man's package (`SPIDERMA.xbs`) holds:

- `spiderman.ent`: the hero entity: skeleton nodes, mesh, members, sound
  groups, and the lines `ani_file characters\spiderman\spiderman.ani`,
  `ai_file "ai\spiderman_new"`, `combo_file characters\spiderman\spiderman.mbo`,
  `locomotion: type spiderman`.
- `spiderman.ani` (text): 296 animation slots (IDLE, RUN, JUMP_FLY, SWING_…,
  WOUNDED…), each naming an `.anml` file.
- `spiderman.mbo` (text): the moveset. `move PUNCH_KICK_JUMP  anim SmPKJ_Palm
  damage 50 …`: button sequence → animation, damage, flags.
- 249 ANMX animations (234 on the 53-track hero rig), the web weapons as
  thrown-item entities (`smshitasswebball`, `…dome`, `impact_web`, gloves),
  their effect meshes and spray entities.

**The Shocker is all in SUBWAY_D.xbs (the boss fight), in the same format.**

- `shocker.ent` (mesh `shocker000`, 86 KB; textures `shocker.dds`,
  `shockmetal.dds`, `shockenv.dds`), `shocker.ani`, three gauntlet weapon
  entities (`shocker_gauntletl/r/d`).
- 60 `shk*.anml` animations: idle, walk, run, fall, shock punch (`ShkPun2`),
  stand blast charge/loop/release, weak blast, arm blasts (`ShkRArmBlst`,
  `ShkLArmBlst`), ring blast (`ShkShkwavRng`), spin blast
  launch/spin/land/dizzy, shock jump launch/fly/land (front and back), web
  shake/breakout, hit reactions, getting up, death.
  The `ShkRctSm*` set (P, PP, PK, KJ…) are his *reactions* to Spider-Man's
  combos, not attacks: his own melee is the shock punch only.
- About 100 effect entities and meshes: single bolt (`shkgntsinbolt`, hit and
  muzzle effects), strong and weak rings (`shksringgrnd`, `shkwringgrnd`),
  spin body (`shkspnbody`), jump ground wave, hand electricity, rings, trails.
- His behaviour, when to blast or spin, is boss AI driven by the level's
  compiled script `subway_d.sx` (`shocker_ring_blast_start`,
  `shocker_spinner_launch`, `shocker_jump_blast`…). That script exists only in
  SUBWAY_D.

**The two skeletons match by name.** Both are 3ds Max Biped rigs with the
same node ids (PELVIS, SPINE…SPINE2, NECK, HEAD, L/R CLAVICLE, UPPERARM,
FOREARM, HAND, FINGER0x, FINGER1x, THIGH, CALF, FOOT, TOE0). The Shocker
lacks fingers 2–4 and WEBSPHERE; he adds gauntlet nodes (TRAIL, ELEC, RING,
STAR). If ANMX tracks bind to bones by these ids (to verify, step 1), his
animations play on a hero rig and Spider-Man's play on his.

## What it would take

### A. Shocker's animations on the hero: likely, cheap to prove

Copy `shk*` ANMX records from SUBWAY_D into a hero package and point slots at
them: `spiderman.ani` IDLE/WALK/RUN/FALLING/WOUNDED/DEATH/getting-up → the
Shk files; `spiderman.mbo` punch moves → `ShkPun2`. Pure data.

### B. The Shocker's body: likely, medium

Put `shocker000` and his textures in the package; in the hero entity, swap
the mesh and use his skeleton node block, keeping Spider-Man's WEBSPHERE and
the missing finger nodes as plain nodes so code that looks them up still
finds them. The engine already loads any of 15 hero bodies by the same route.

### C. His moves: the web weapons become gauntlet attacks: medium-large

The player's special attacks are thrown-item entities (text: damage, radius,
launch script, visual entity, rocket/bounce behaviour). Re-skin them with his
effects, keeping the player-side code paths that already work:

| Spider-Man | Shocker mode | Shocker data |
|---|---|---|
| Impact web / web ball (shot) | gauntlet bolt | `shkgntsinbolt`, hit/muzzle fx, `ShkRArmBlst` |
| Web dome (area burst) | ring blast | `shksringgrnd`, `ShkShkwavRng` |
| Web gloves (powered fists) | shock punch | hand electricity members, `ShkPun2` |
| Combos | shock punch / blasts | `.mbo` anim and damage edits |

Limits:
- His boss-only routines (spin blast, shock jump) are script functions in
  `subway_d.sx`. Driving them in other levels needs either the script VM
  worked out (to add the functions to each level's script) or host code that
  plays the animation and spawns the effect entity. Plan them last.
- The `.mbo` launch lines call scripts such as
  `spiderman_webball_adv_launch`. Re-skinned weapons reuse those scripts, so
  they behave like web shots with Shocker visuals and animations, not like
  his exact boss attacks.

### D. Keep Spider-Man's traversal

Shocker has no swing, wall-crawl, zip or web-yank animations, and
`locomotion type spiderman` needs them. Those slots keep Spider-Man's
animations, playing on the Shocker's body (same bone names), so he can still
get around every level. Swinging as the Shocker will look odd but works;
the alternative, a walking-only hero, would make most levels unplayable.

### E. Choosing it

Shocker mode is a costume-panel entry ("Shocker"), separate from
shockspidey. Choosing it sets costume 8 (shockspidey) as now, and when the
game opens `heroes\SHOCKSPI.xbs`, the port serves the built package
(`mods\SHOCKER.xbs`) instead. Game files stay untouched. A file-open hook in
the port's I/O path is the only engine-side code; the rest is data. The
panel preview gets a Shocker `.hpv` from the same package.

## Risks and unknowns

1. **Animation binding.** That ANMX tracks bind by bone id is inferred from
   the shared names, not confirmed. Step 1 settles it in one level load.
   If tracks bind by index instead, the Shocker's 65-track animations need
   remapping to the hero's 53 tracks: a tool change, not a blocker.
2. **Package size.** `aux_xbox.txt` gives every hero `4092800`, apparently
   the memory reserved for the package. SPIDERMA's stream 0 is 4,090,880
   bytes (1.9 KB under). Adding the Shocker's mesh, textures, ~60 animations
   and the effects is about 1.2 MB, so either raise that number (test whether
   the engine honours it) or drop what Shocker mode never uses: the
   shockspidey mesh and textures, Spider-Man animations replaced by Shk ones,
   and the unused web-weapon effects.
3. **Writing packages.** tools/xbxm.py reads but does not write. A writer
   needs the 0x40-byte header and the directory entries' other fields
   (flags at +0x28…+0x3C) reproduced. Check first by rebuilding SPIDERMA
   unchanged and comparing.
4. **Hero-specific code.** The engine checks the hero name in places (the
   Goblin voice cache, `sub_000EA580`). Shocker mode keeps the name
   `shockspidey`, so those paths see a normal Spider-Man costume. Spider-Man's
   quips stay; the Shocker's own voice lines in SUBWAY_D (`taunt`) could be
   added later.
5. **Effects in other levels.** Effect entities reference meshes, textures
   and `.tbb` spray files; all must be copied in with them. A missing one
   most likely draws nothing; a crash is possible.

## Order

1. **Proof (small):** writer round-trip (rebuild SHOCKSPI unchanged, game
   loads it), then the same package with Spider-Man's right hook (`SmPHookR`)
   replaced by `ShkPun2` and IDLE → `ShkIdl`. Load Warehouse Hunt, punch.
   This answers risks 1–3.
2. **Body (B)** with Spider-Man's own animations: Shocker walking, swinging,
   fighting with Spider-Man's moves.
3. **Animations (A):** locomotion, hits, death and punch moves → Shk.
4. **Moves (C):** bolt for impact web and web ball, ring blast for the dome,
   shock punch for the gloves.
5. **Panel entry and redirect (E)**, preview.
6. Optional: spin blast and shock jump through host code or the script VM.

Steps 1–3 are mostly a Python tool (`tools/shocker_build.py`) and a small
file-redirect hook. Step 4 is iterative tuning in game. Step 6 is open-ended.

## Status

- **Step 1, proof: works (2026-10-28).** `tools/xbs.py` reads and writes
  packages (header and directory fields documented in it; rebuilding
  SHOCKSPI gives identical streams and directory). `tools/shocker_build.py`
  puts the Shocker's `ShkIdl` and `ShkPun2` in the shockspidey package in
  place of `SmIdl` and `SmPHookR`, appended to stream 0 (4,004,512 of
  4,092,800 bytes). Served through `RECOMP_OVERLAY_DIR` from
  `build\Release\overlay\pc_ps\data\heroes\SHOCKSPI.xbs`, it loaded in
  Warehouse Hunt without errors, and the hero stood in the Shocker's
  wide-armed idle with the rig intact: his 64/65-track animations drive the
  53-bone hero rig, so tracks bind by bone id (risk 1 settled). Risks 2 and 3
  are settled for small changes: the package loads with a rewritten header
  and directory and a longer stream 0. Whether the punch reads as ShkPun2 in
  play is still to be seen by eye.
- The overlay folder is only used with PlayStation prompts (`pc_ps`); with
  Xbox prompts it is `overlay\pc`. Step 5 replaces this with a proper
  redirect.
