# Installing Spider-Man (2002) for PC

This guide takes you from your Xbox disc to playing the PC port. It takes
about ten minutes, most of it copying the game.

**Download:** [SpiderManSetup.exe](https://github.com/xdivorex-gif/spiderman-2002-pc/releases/latest/download/SpiderManSetup.exe)
(about 42 MB) · [all releases](https://github.com/xdivorex-gif/spiderman-2002-pc/releases)

> The installer contains no game files. You need your own copy of the
> original Xbox game, *Spider-Man* (2002, NTSC release).

## What you need

- Windows 10 or 11, 64-bit
- A graphics card with Direct3D 11 (any card from the last ten years)
- About 2.5 GB of free space
- Your own copy of *Spider-Man* (2002) for the original Xbox, unpacked to a
  folder (see step 1)
- A controller is recommended (Xbox controllers work directly; PlayStation
  controllers work through Steam or DS4Windows). Keyboard works too.

## 1. Get your game's files onto your PC

The installer needs the contents of your game disc as a normal folder: one
that holds a file called `default.xbe` and a folder called `data`.

Xbox discs cannot be read by a PC's disc drive. The usual way is to dump the
game from your own Xbox (for example with a softmodded console and an FTP
client) or to extract an image you made of your own disc with a tool such
as *extract-xiso*. Put the result in a folder, for example
`C:\Games\Spider-Man Xbox`.

When you open that folder you should see:

```
default.xbe
data\
```

## 2. Run the installer

1. Download **SpiderManSetup.exe** with the link above and run it. Windows
   may warn that it is from an unknown publisher: choose *More info*, then
   *Run anyway*.
2. **Your game:** click *Browse...* and choose the folder from step 1.
3. **Install to:** leave the suggested folder, or choose another.
4. **Copy the game into the install folder:** leave it ticked to keep
   everything in one place (about 2 GB). Untick it to use your game folder
   where it is; keep that folder in place afterwards.
5. Click **Install**.

The installer checks that your `default.xbe` is the release the port is made
for. If it says the file does not match, you have a different release (for
example PAL) or a modified copy; the port only supports the original NTSC
release for now.

When it says **Installed**, you have a **Spider-Man** shortcut on your
desktop and in the Start menu.

## 3. Play

Open the **Spider-Man** shortcut. The launcher opens first:

- **Resolution, aspect ratio, fullscreen, VSync:** set them for your screen
- **Button prompts:** Xbox or PlayStation
- **Controls:** remap the controller and keyboard
- **Costume:** choose the suit you start with

Click **Play**. The first start takes a few seconds longer while the game
prepares its PC files from your copy.

### In the game

- **Pause** shows the pause menu as on the console, plus:
  - **Triangle / Y** opens the costume picker. Picking a suit restarts the
    level in it.
  - **Options** has two extra settings at the bottom: **Subtitles** (Square
    / X to switch) and **Difficulty** (L1 / R1 to change; the level restarts
    on the new difficulty).

## The Shocker costume (work in progress)

Choosing the Shocker costume lets you play as the Shocker, with his own
blasts, tornado, yank and charged gauntlets (L2 + a face button), an
overheat gauge in place of web fluid, and his own HUD. It is still a work in
progress: some moves are rough and more updates are coming.

## Troubleshooting

| Problem | What to do |
|---|---|
| "That folder has no default.xbe" | Choose the folder that directly holds `default.xbe`, not a folder above it. |
| "default.xbe does not match" | The port needs the original NTSC Xbox release. Other releases are not supported yet. |
| "Failed to load default.xbe" when starting | The game folder moved. Run the installer again, or set `DataDir=` under `[Game]` in `settings.ini` in the install folder. |
| The screen is black or the wrong size | Open the launcher and set a resolution your monitor supports, or turn off fullscreen. |
| The controller does nothing | Connect it before starting the game. PlayStation controllers need Steam Input or DS4Windows. |

## Uninstalling

Delete the install folder and the two **Spider-Man** shortcuts. If you
unticked the copy option, your own game folder is left as it was.

---

This is an unofficial fan project, not affiliated with or endorsed by
Activision, Treyarch, Marvel, Sony Pictures or Microsoft.
