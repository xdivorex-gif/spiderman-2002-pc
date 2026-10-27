@echo off
rem Play Spider-Man (2002, Xbox) - recompiled.
rem
rem Controller: any XInput pad (an Xbox pad, or a DualSense through
rem DualSenseX / DS4Windows with Xbox 360 emulation on). With no pad
rem connected, the keyboard drives player 1 - see PORT_NOTES.md.
rem F11 or Alt+Enter: fullscreen. Close the window to quit.
rem
rem Bring-up switches, all needed today (see PORT_NOTES.md):
rem   GPU executor + DMA walk + software-method interrupt, 60 Hz vblank,
rem   title-built kernel events, USB controllers, keyboard fallback.

cd /d "%~dp0"
set RECOMP_PB_EXEC=1
set RECOMP_PB_WALK=dma
set RECOMP_NV2A_SWMETHOD=1
set RECOMP_VBLANK=paced
set RECOMP_TITLE_KEVENTS=1
set RECOMP_FB_WINDOW=1
set RECOMP_USB=1
set RECOMP_KEYBOARD=1
set RECOMP_WINDOW_SCALE=2
rem Present through D3D11 on the GPU (scaling, fullscreen, vsync) instead of
rem GDI. Falls back to GDI if it fails.
set RECOMP_D3D_PRESENT=1
rem Rasterise the 3D on the GPU (src\gpu_backend.c) instead of the CPU:
rem about twice the frame rate, and Peter's jacket draws without speckles.
rem Remove this line to go back to the CPU rasteriser.
set RECOMP_GPU_RENDER=1
rem Resolution, aspect ratio, supersampling, fullscreen, vsync and renderer
rem come from build\Release\settings.ini (SpiderManLauncher.exe edits it; the
rem keys are in src\settings.c), and override the lines here.
rem Answer D3D's kickoffs the moment they are made instead of at the GPU
rem thread's next pass: part of what holds the level at a steady 60 fps.
set RECOMP_NV2A_FAST_KICK=1
rem Hold each flip to the next vblank, as the console's presentation does:
rem menus and movies ran at 100+ fps without it.
set RECOMP_FLIP_PACING=1
rem Sound: the emulated MCPX APU and AC'97, out through XAudio2.
set RECOMP_AC97_READY=1
rem Give freed memory back. Textures live in contiguous memory, and without
rem this nothing the game frees is reused: quitting a level to the main menu
rem ran the 64 MB out, and the menu came back without its background art.
set RECOMP_HEAP_RECLAIM=1
rem Sound: Spider-Man answers the APU's sample-limit interrupt by resetting
rem the DSPs and stalling 10 ms, on the thread that makes the sound -- about
rem 45% of the audio lost. This lets that stall return at once.
set RECOMP_ISR_NO_STALL=1
rem Run the APU's interrupt routine only where the console's one CPU could
rem have taken it, not beside a thread at raised IRQL: DirectSound tore a
rem voice down under its routine and crashed (~23 min into a level).
set RECOMP_DISPATCH_GATE=1
rem No watchdog: it ends the process after N seconds.
set RECOMP_WATCHDOG_SECS=

rem Diagnostics go to play_log.txt; this window stays open while you play.
"build\Release\spiderman_recomp.exe" > play_out.txt 2> play_log.txt
