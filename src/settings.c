/*
 * settings.ini: the player's settings, turned into the RECOMP_* switches the
 * toolkit reads.
 *
 * The toolkit is configured through environment variables, which suits
 * bring-up and play.bat, not a player. This runs first thing in WinMain and
 * does two things:
 *
 *   1. Started without play.bat (from the launcher, or by double-clicking the
 *      exe), the switches every run needs are set here -- the same list
 *      play.bat sets. A variable already in the environment is left alone, so
 *      play.bat and run.ps1 keep full control for debugging.
 *   2. settings.ini, next to the exe, is read and its friendly keys mapped to
 *      switches. These do override the environment: they are what the player
 *      chose in the launcher. A missing file or key changes nothing.
 *
 *   [Display]
 *   Resolution=1920x1080      output size; 640x480 is the original
 *   AspectRatio=stretch       widescreen: the 3D sees more to the sides (Hor+)
 *                             | stretch: the 4:3 picture filling 16:9
 *                             | 4:3: original shape, bars
 *   Fullscreen=0              1: borderless fullscreen at start (F11 toggles)
 *   VSync=1                   0: present without waiting for the display
 *   FrameRate=60              the game's own 60 | N: cap at N fps | 0: unlocked
 *   [Graphics]
 *   Renderer=gpu              gpu: D3D11 | cpu: the software rasteriser
 *                             (slower, the reference; always 640x480)
 *   [Controls]
 *   ButtonPrompts=xbox        xbox | ps5 | ps4: the prompt icons drawn in
 *                             menus and the HUD (GPU renderer only; ps5 has
 *                             the DualSense's white symbols, ps4 the coloured
 *                             ones; "playstation" is ps5), and the button
 *                             names in the text (overlay\pc_ps)
 *   [Game]
 *   Costume=default           default: the game's own choice (Secret Store,
 *                             cheats) | spiderman: the classic suit | peter,
 *                             wrestler, alexross, goblin, mjspidey,
 *                             shockspidey, scispidey, thugspidey, btgspidey,
 *                             sthspidey, helispidey, freakspidey, copspidey.
 *                             Worn from the next level start; levels with
 *                             their own hero keep it (recomp_manual.c)
 *   [Pad], [Keyboard]         controller and key mapping, read by the toolkit
 *                             (src/input/input_config.h): a=south, a=Z ...
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every run needs these (PORT_NOTES.md, "Run switches"). */
static const char *const s_required[][2] = {
    { "RECOMP_PB_EXEC",        "1" },       /* GPU executor */
    { "RECOMP_PB_WALK",        "dma" },
    { "RECOMP_NV2A_SWMETHOD",  "1" },       /* software-method interrupt */
    { "RECOMP_VBLANK",         "paced" },   /* 59.94 Hz vblank */
    { "RECOMP_TITLE_KEVENTS",  "1" },
    { "RECOMP_FB_WINDOW",      "1" },
    { "RECOMP_USB",            "1" },       /* controllers */
    { "RECOMP_KEYBOARD",       "1" },       /* keyboard stands in for a pad */
    { "RECOMP_WINDOW_SCALE",   "2" },
    { "RECOMP_D3D_PRESENT",    "1" },
    { "RECOMP_GPU_RENDER",     "1" },
    { "RECOMP_NV2A_FAST_KICK", "1" },
    { "RECOMP_FLIP_PACING",    "1" },
    { "RECOMP_AC97_READY",     "1" },       /* sound */
    { "RECOMP_HEAP_RECLAIM",   "1" },       /* free memory between levels */
    { "RECOMP_ISR_NO_STALL",   "1" },       /* no 10 ms sound gap per APU reset */
    { "RECOMP_DISPATCH_GATE",  "1" },       /* APU ISR never beside DISPATCH code */
};

static void set(const char *name, const char *value)
{
    _putenv_s(name, value ? value : "");    /* "" removes it */
}

static void exe_dir(char *out, size_t cap)
{
    char *slash;
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (!n || n >= cap) { out[0] = 0; return; }
    slash = strrchr(out, '\\');
    if (slash) slash[1] = 0;
}

void settings_apply(void)
{
    char ini[MAX_PATH], v[64];
    unsigned w = 640, h = 480;
    int i, gpu, four3;
    const char *ps;

    /* 1. Defaults, unless something set up the environment already. */
    if (!getenv("RECOMP_PB_EXEC"))
        for (i = 0; i < (int)(sizeof s_required / sizeof s_required[0]); i++)
            if (!getenv(s_required[i][0]))
                set(s_required[i][0], s_required[i][1]);
    /* The title's threads on one core, as on the console, launched by a
     * batch file or not. The movie player's two threads hand two mutants
     * back and forth, and on many cores at once a hand-off was lost after
     * a few movies: both waited forever, the movie black (kernel_sync.c
     * breaks such a wait too). One core holds 60 fps in movies and play.
     * RECOMP_GUEST_CPUS=all for every core. */
    if (!getenv("RECOMP_GUEST_CPUS"))
        set("RECOMP_GUEST_CPUS", "one");

    /* 2. The player's settings. */
    exe_dir(ini, sizeof ini);
    if (!ini[0] || strlen(ini) + 13 >= sizeof ini)
        return;
    strcat(ini, "settings.ini");
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES)
        return;

    GetPrivateProfileStringA("Graphics", "Renderer", "gpu", v, sizeof v, ini);
    gpu = _stricmp(v, "cpu") != 0;
    set("RECOMP_GPU_RENDER", gpu ? "1" : NULL);

    GetPrivateProfileStringA("Display", "Resolution", "640x480", v, sizeof v, ini);
    if (sscanf(v, "%ux%u", &w, &h) != 2 || w < 320 || h < 240 || w > 7680 || h > 4320) {
        w = 640; h = 480;
    }
    GetPrivateProfileStringA("Display", "AspectRatio", "stretch", v, sizeof v, ini);
    four3 = strcmp(v, "4:3") == 0;
    if (four3)
        w = (h * 4 + 1) / 3;                /* the original shape, centred */
    {
        /* widescreen: the 3D camera sees as much more to the sides as the
         * screen is wider than 4:3 (Hor+, recomp_manual.c), so the stretch
         * to the screen gives everything its true shape */
        char s[32];
        double f = (double)w * 3.0 / ((double)h * 4.0);
        if (strcmp(v, "widescreen") == 0 && f > 1.01) {
            sprintf(s, "%.4f", f);
            set("RECOMP_HOR_PLUS", s);
        } else {
            set("RECOMP_HOR_PLUS", NULL);
        }
    }
    if (gpu && (w != 640 || h != 480)) {
        char s[32];
        sprintf(s, "%ux%u", w, h);
        set("RECOMP_RENDER_SIZE", s);
        sprintf(s, "%ux%u", w, h);
        set("RECOMP_OUTPUT_SIZE", s);
    } else {
        set("RECOMP_RENDER_SIZE", NULL);
        set("RECOMP_OUTPUT_SIZE", NULL);
    }

    set("RECOMP_FULLSCREEN",
        GetPrivateProfileIntA("Display", "Fullscreen", 0, ini) ? "1" : NULL);
    set("RECOMP_PRESENT_VSYNC",
        GetPrivateProfileIntA("Display", "VSync", 1, ini) ? NULL : "0");
    {   /* FrameRate: 60 the game's own | N a cap | 0 unlocked -- in play only;
         * the menus keep the console's pacing (frame_rate.c). */
        int hz = GetPrivateProfileIntA("Display", "FrameRate", 60, ini);
        char s[32];
        if (hz == 60 || hz < 0) {
            set("RECOMP_FRAME_RATE", NULL);
        } else {
            sprintf(s, "%d", hz);
            set("RECOMP_FRAME_RATE", s);
        }
    }

    GetPrivateProfileStringA("Controls", "ButtonPrompts", "xbox", v, sizeof v, ini);
    ps = !_stricmp(v, "ps4") ? "ps4"
       : !_stricmp(v, "ps5") || !_stricmp(v, "playstation") ? "ps5" : NULL;
    set("RECOMP_BUTTON_PROMPTS", ps);
    {
        /* The game's text with PC wording (tools/make_overlay.py), copied
         * beside the exe by the build: overlay\pc, or overlay\pc_ps with
         * PlayStation button names to go with the PlayStation prompts. */
        char ov[MAX_PATH];
        exe_dir(ov, sizeof ov);
        if (strlen(ov) + 16 < sizeof ov) {
            strcat(ov, ps ? "overlay\\pc_ps" : "overlay\\pc");
            set("RECOMP_OVERLAY_DIR", ov);
        }
    }

    /* "default" leaves RECOMP_COSTUME as the environment has it (a .bat). */
    {   /* the player's own copy of the game (main.c, game_dir) */
        char dir[MAX_PATH];
        GetPrivateProfileStringA("Game", "DataDir", "", dir, sizeof dir, ini);
        if (dir[0] && !getenv("RECOMP_GAME_DIR"))
            set("RECOMP_GAME_DIR", dir);
    }
    GetPrivateProfileStringA("Game", "Costume", "default", v, sizeof v, ini);
    if (_stricmp(v, "default"))
        set("RECOMP_COSTUME", v);

    /* [Pad] and [Keyboard]: the controller and key mapping, read by the
     * toolkit itself (xboxrecomp src/input/input_config.h). */
    set("RECOMP_INPUT_INI", ini);
}
