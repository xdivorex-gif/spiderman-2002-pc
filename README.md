# Spider-Man (2002, Xbox) — PC port

An unofficial native Windows port of the original Xbox game, made by static
recompilation with the [xboxrecomp](https://github.com/sp00nznet/xboxrecomp)
toolkit. The game's own code is translated to C and built into a Windows
executable; its hardware (GPU, sound, controllers, storage) is provided by
the toolkit and by the host code in this repository.

**This repository contains no game content.** No executable, data, art,
audio, video or text from the game is included here or in releases. To play,
you need your own legally obtained copy of the Xbox game.

## Features

- Runs natively on Windows 10 / 11 with Direct3D 11, at any resolution, with
  a widescreen mode that keeps the HUD in proportion
- A steady 60 fps, with the game's threads kept on one core as on the console
- Controller support through XInput, a keyboard fallback, and optional
  PlayStation button prompts
- A launcher for display, controls and costume settings
- A costume picker on the pause screen, including the hidden costumes, with
  an instant level restart when you change suit
- Pause-menu options for subtitles and difficulty (Easy, Normal, Hero,
  Super Hero)
- More responsive movement: an earlier switch from walk to run, and no dead
  spot just past the stick's dead zone
- Optional enhancements made on your own PC from your own copy: sharper HUD,
  loading screens and intro movies, and subtitles

## The Shocker costume (work in progress)

The Shocker costume turns the Shocker costume slot into a playable Shocker
with his own moves:

- His blasts, tornado, yank and charged gauntlets, on L2 + a face button or
  the game's own web-special combos
- An overheat system in place of web fluid: each special heats his
  gauntlets, and overheating locks them until they vent
- His own HUD: a brass health bar with a gauntlet heat gauge that glows as
  it heats up, a compass with his icon, and a recoloured height meter

**This is still a work in progress.** Some moves are rough, the yank has not
been fully checked against the heat system, and the visuals will keep being
refined. More updates to the Shocker costume will come in future releases.

## Installing

Download `SpiderManSetup.exe` from the [Releases](../../releases) page and
run it. It asks for your copy of the game (its Xbox disc image, `.iso`, or
the unpacked disc: a folder holding `default.xbe` and `data`), checks it,
and installs the port with a desktop shortcut. See [INSTALL.md](INSTALL.md) for the full steps.

## Building from source

Requirements: Windows 10 / 11, Visual Studio 2022 with the C++ workload,
CMake 3.20+, Python 3.10+, Git, and your own copy of the game.

```bash
git clone https://github.com/sp00nznet/xboxrecomp.git ../xboxrecomp
git -C ../xboxrecomp checkout a4a925b1ae85c3074f756a924d2f91e2f8fe5216
git -C ../xboxrecomp apply ../spiderman-2002-pc/patches/xboxrecomp.patch
cp -r /path/to/your/game ../xboxrecomp/game_files   # default.xbe + data/
./regen.sh                                           # recompile and build
```

`regen.sh` analyses `default.xbe`, lifts it to C in `src/recomp/gen/` (not
committed: it is made from the game) and builds `build/Release/`. The port's
own code is in `src/`; `PORT_NOTES.md` explains how it works.

### Toolkit changes

The port needs some changes to xboxrecomp that are not upstream yet (shared
GPU presentation, flip pacing, file-open hooks, an overlay folder, recovery
from stalled waits). They are kept as `patches/xboxrecomp.patch`, made
against the commit above.

## Layout

| Path | What |
|---|---|
| `src/` | host code: renderer back end, launcher, settings, pause-menu additions, subtitles, movement, the Shocker costume |
| `tools/` | scripts that make the optional enhancements and costume files from your own copy |
| `assets/` | the launcher background and PlayStation button prompts (original art) |
| `patches/` | changes to the xboxrecomp toolkit |
| `PORT_NOTES.md` | how the port works, and what was found along the way |

## Legal

This is an unofficial fan project, not affiliated with or endorsed by
Activision, Treyarch, Marvel, Sony Pictures or Microsoft. Spider-Man and all
related characters are trademarks of their owners. The project contains no
copyrighted game material and does not help to obtain it.

## License

The port's own code is released under the [MIT License](LICENSE). It does
not cover the game: Spider-Man (2002) and its content belong to their
owners, and nothing from the game is in this repository or its releases.
The xboxrecomp toolkit is MIT-licensed by its author (sp00nz).
