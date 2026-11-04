/*
 * costume_panel.c -- the costume: chosen in settings.ini or in a panel over
 * the pause menu, worn from the next level start.
 *
 * The game's costumes are entries of its secrets table (built at boot by
 * sub_000F5CA0 at 0x4BCAA0, 0x2C bytes each). Which one is worn is one byte
 * per entry in the player profile, [[0x4BC614]+0x11C] + 0x1544 + index, at
 * most one set: the Secret Store and the costume cheats write it. At level
 * start sub_000F0690 (this = the app object) walks the set entries and puts
 * that costume's hero name in the hero slot -- except on levels that force
 * their own hero (Peter Parker, the Goblin), which it keeps. So choosing a
 * costume is writing that byte before a level starts: costume_level_start,
 * from the entry hook in recomp_manual.c, does it on every level start, and
 * the game loads the suit, names and all.
 *
 * The choice comes from RECOMP_COSTUME (settings.ini [Game] Costume, via
 * settings.c): "spiderman" is the classic suit (no costume byte set), unset
 * or "default" leaves the game's own choice alone.
 *
 * The panel. While the game is paused (byte [0x4BC614]+0x18B, what the
 * script call is_paused() returns), a hint in the corner offers Y /
 * Triangle; that opens the costume list over the pause menu. Up / down
 * choose, A applies, B closes. Apply writes the choice (and settings.ini),
 * closes the panel and restarts the level at once (level_restart_request,
 * shocker_powers.c): the hero is built at level load, so a new costume
 * needs a new start. RECOMP_COSTUME_RESTART, if set, presses the pause
 * menu's own Restart Level instead (below).
 *
 * Input goes through the toolkit's report filter (usb_gamepad.h), which sees
 * every pad report just before the game does -- the pad, the keyboard and
 * pad scripts already folded in, in Xbox terms. While the panel is open the
 * game gets an empty report. Drawing happens on the finished frame just
 * before it goes to the window (gpu_backend.c present_scaled), with GDI on a
 * copy: only while the game is paused, so it costs nothing in play. That is
 * the GPU renderer at a raised resolution (RECOMP_RENDER_SIZE); at 640x480
 * the window shows guest memory directly and the panel is not drawn.
 *
 * The 3D view. The panel's left half shows the selected costume's model,
 * turned with L1 / R1 (Black / White) while held. tools/xbxm.py --export
 * reads the game's hero files (heroes/*.xbs: XBXM meshes, DDS textures) into
 * previews/<FILE>.hpv beside the exe -- the bind pose, a triangle list and
 * the DXT texture blocks; the format is in that script. Drawn here by a
 * small software rasteriser into the copy of the frame, with a light from
 * the front: it never touches the game's D3D state, and runs only while the
 * panel is open.
 */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "usb_gamepad.h"

extern ptrdiff_t g_xbox_mem_offset;

#define GUEST32(va) (*(volatile uint32_t *)((uint8_t *)g_xbox_mem_offset + (va)))
#define GUEST8(va)  (*(volatile uint8_t *)((uint8_t *)g_xbox_mem_offset + (va)))

#define SM_APP_PTR          0x004BC614u /* -> the app object */
#define SM_APP_PROFILE      0x11Cu      /* app object -> player profile */
#define SM_APP_PAUSED       0x18Bu      /* app object: game paused */
#define SM_PROFILE_COSTUME  0x1544u     /* + secrets index: 1 = worn */
#define SM_PROFILE_HERO     0x14E8u     /* the hero's name, 32 bytes */
#define SM_COSTUME_FIRST 3
#define SM_COSTUME_LAST  15
#define SM_COSTUME_GOBLIN 6

/* sub_000EA580 answers "goblin mode" (in_goblin_mode(); it picks the hero's
 * voice lines and controls): yes if the Goblin costume byte is set or the
 * hero is "plrgoblin", and otherwise -- most of the time -- the answer it
 * last gave, kept in this byte. On the console nothing switches the Goblin
 * off mid-session; here a Goblin run left it at 1 and Spider-Man spoke with
 * the Goblin's voice afterwards. Cleared whenever another costume is put on. */
#define SM_GOBLIN_MODE_CACHE 0x004C01E8u

/* The names are settings.ini's (and the launcher's); the game names only its
 * Secret Store costumes, so the cheat ones go by hero name and cheat code. */
static const struct { const char *name; const wchar_t *label; int index; const char *file; }
s_costumes[] = {
    { "spiderman",   L"Spider-Man",                 -1, "SPIDERMA" },
    { "peter",       L"Peter Parker",                3, "PETERSTU" },
    { "wrestler",    L"Wrestler Spider-Man",         4, "SPIDEYWR" },
    { "alexross",    L"Alex Ross Spider-Man",        5, "ROSSSPID" },
    { "goblin",      L"Green Goblin",                6, "PLRGOBLI" },
    { "mjspidey",    L"mjspidey (GIRLNEXTDOOR)",     7, "MJSPIDEY" },
    { "shockspidey", L"shockspidey (HERMANSCHULTZ)", 8, "SHOCKSPI" },
    { "scispidey",   L"scispidey (SERUM)",           9, "SCISPIDE" },
    { "thugspidey",  L"thugspidey (KNUCKLES)",      10, "THUGSPID" },
    { "btgspidey",   L"btgspidey (STICKYRICE)",     11, "BTGSPIDE" },
    { "sthspidey",   L"sthspidey (THUGSRUS)",       12, "STHSPIDE" },
    { "helispidey",  L"helispidey (CAPTAINSTACEY)", 13, "HELISPID" },
    { "freakspidey", L"freakspidey (FREAKOUT)",     14, "FREAKSPI" },
    { "copspidey",   L"copspidey (REALHERO)",       15, "COPSPIDE" },
};
#define N_COSTUMES ((int)(sizeof s_costumes / sizeof s_costumes[0]))

static volatile int s_want_row = -2;    /* row of s_costumes; -1 game's choice; -2 unread */

void xbox_set_extra_overlay(const char *dir);   /* kernel_path.c */

/* Files only one costume uses: overlay\<costume> beside the exe, served
 * before the shared overlay while that costume is chosen. The Shocker's is
 * a menu sound bank with his sounds in the slots of Spider-Man's own
 * (tools/shocker_build.py); the menu bank is read at the front end, so it
 * changes with the costume chosen there or in settings.ini. */
static void costume_overlay(int row)
{
    char dir[MAX_PATH], *slash;
    if (row < 0 || strcmp(s_costumes[row].name, "shockspidey")
        || !GetModuleFileNameA(NULL, dir, sizeof dir) || !(slash = strrchr(dir, '\\'))) {
        xbox_set_extra_overlay(NULL);
        return;
    }
    _snprintf(slash + 1, sizeof dir - (slash + 1 - dir), "overlay\\%s", s_costumes[row].name);
    dir[sizeof dir - 1] = 0;
    xbox_set_extra_overlay(dir);
}

static void read_choice(void)
{
    const char *e;
    int i;

    if (s_want_row != -2)
        return;
    e = getenv("RECOMP_COSTUME");
    s_want_row = -1;
    for (i = 0; e && i < N_COSTUMES; i++)
        if (!_stricmp(e, s_costumes[i].name))
            s_want_row = i;
    if (e && *e && _stricmp(e, "default") && s_want_row < 0)
        fprintf(stderr, "  [COSTUME] unknown RECOMP_COSTUME \"%s\", ignored\n", e);
    costume_overlay(s_want_row);
}

static uint32_t app_object(void)
{
    uint32_t app = g_xbox_mem_offset ? GUEST32(SM_APP_PTR) : 0;
    return app >= 0x10000 && app < 0x04000000 ? app : 0;
}

static uint32_t profile_of(uint32_t app)
{
    uint32_t p = app ? GUEST32(app + SM_APP_PROFILE) : 0;
    return p >= 0x10000 && p < 0x04000000 ? p : 0;
}

static void write_costume_bytes(uint32_t profile, int row)
{
    int i, want = s_costumes[row].index;
    for (i = SM_COSTUME_FIRST; i <= SM_COSTUME_LAST; i++)
        GUEST8(profile + SM_PROFILE_COSTUME + i) = (uint8_t)(i == want);
    if (want != SM_COSTUME_GOBLIN)
        GUEST8(SM_GOBLIN_MODE_CACHE) = 0;
}

void costume_level_start(uint32_t app)
{
    uint32_t profile = profile_of(app);
    int row;

    read_choice();
    row = s_want_row;
    if (row < 0 || !profile)
        return;
    write_costume_bytes(profile, row);
    fprintf(stderr, "  [COSTUME] level start: %s (hero was \"%.31s\")\n",
            s_costumes[row].name, (const char *)g_xbox_mem_offset + profile + SM_PROFILE_HERO);
    fflush(stderr);
}

/* The row worn now, as the profile says: the panel opens on it. */
static int worn_row(void)
{
    uint32_t profile = profile_of(app_object());
    int i, r;
    if (!profile)
        return 0;
    for (r = 1; r < N_COSTUMES; r++) {
        i = s_costumes[r].index;
        if (GUEST8(profile + SM_PROFILE_COSTUME + i))
            return r;
    }
    return 0;
}

/* The secrets index of the costume worn now, -1 for the classic suit
 * (shocker_input.c: his blasts on L2 are his alone). */
int costume_worn_index(void)
{
    int r = s_want_row >= 0 ? s_want_row : worn_row();
    return s_costumes[r].index;
}

static int game_paused(void)
{
    uint32_t app = app_object();
    return app && GUEST8(app + SM_APP_PAUSED);
}


/* ── Pressing the pause menu's Restart Level ───────────────────── */

/* RECOMP_COSTUME_RESTART: the presses, comma-separated: up down left right
 * a b x y start back, a count as up*6, and waitN (ms). Pause menu: Audio
 * Options, Restart Level, Options, Combat Controls, Exit to Menu; then
 * "Restart at the beginning of level?" Ok / Cancel. The menu wraps (five
 * ups are no move at all), so the cursor cannot be sent to the top blind:
 * B closes the pause menu and Start opens it again, on its first item, and
 * one down is Restart Level. (up*6,down from Audio Options came back to
 * Audio Options, and A opened it. Start in the menu is A, not close.) */
#define RESTART_DEFAULT "wait300,b,wait900,start,wait900,down,a,wait900,up,a"
#define MAX_STEPS 32

typedef struct { uint8_t dig, ana; unsigned wait_ms; } Step;
static Step s_steps[MAX_STEPS];
static int s_nsteps;
static volatile int s_inject_on;
static DWORD s_inject_t0;

static void restart_parse(void)
{
    static const struct { const char *n; uint8_t dig, ana; } b[] = {
        { "up", 0x01, 0 }, { "down", 0x02, 0 }, { "left", 0x04, 0 }, { "right", 0x08, 0 },
        { "start", 0x10, 0 }, { "back", 0x20, 0 },
        { "a", 0, 0x01 }, { "b", 0, 0x02 }, { "x", 0, 0x04 }, { "y", 0, 0x08 },
    };
    const char *e = getenv("RECOMP_COSTUME_RESTART");
    char spec[256], *tok, *ctx = NULL;
    int k;

    _snprintf(spec, sizeof spec, "%s", e && *e ? e : RESTART_DEFAULT);
    spec[sizeof spec - 1] = 0;
    s_nsteps = 0;
    for (tok = strtok_s(spec, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        char *star = strchr(tok, '*');
        int n = star ? atoi(star + 1) : 1;
        if (star)
            *star = 0;
        if (!strncmp(tok, "wait", 4)) {
            if (s_nsteps < MAX_STEPS) {
                Step st = { 0, 0, (unsigned)atoi(tok + 4) };
                s_steps[s_nsteps++] = st;
            }
            continue;
        }
        for (k = 0; k < (int)(sizeof b / sizeof b[0]); k++)
            if (!_stricmp(tok, b[k].n))
                break;
        if (k == (int)(sizeof b / sizeof b[0]))
            continue;
        while (n-- > 0 && s_nsteps < MAX_STEPS) {
            Step st = { b[k].dig, b[k].ana, 0 };
            s_steps[s_nsteps++] = st;
        }
    }
}

/* Each press is held 100 ms and followed by 150 ms of nothing: the menus
 * act on a press, and two presses with no release between are one. */
#define PRESS_MS 100
#define GAP_MS   150

static void inject(uint8_t *r)
{
    DWORD t = GetTickCount() - s_inject_t0, at = 0;
    int i;

    memset(r + 2, 0, 18);
    for (i = 0; i < s_nsteps; i++) {
        if (s_steps[i].wait_ms) {
            at += s_steps[i].wait_ms;
            continue;
        }
        if (t >= at && t < at + PRESS_MS) {
            r[2] = s_steps[i].dig;
            for (int j = 0; j < 8; j++)
                if (s_steps[i].ana & (1u << j))
                    r[4 + j] = 0xFF;
            return;
        }
        at += PRESS_MS + GAP_MS;
    }
    if (t >= at)
        s_inject_on = 0;
}

/* ── The panel's input ─────────────────────────────────────────── */

void shocker_input_filter(uint8_t *r);  /* shocker_input.c */
void movement_stick(uint8_t *r);        /* movement.c */

static volatile int s_open, s_sel, s_paused;
static volatile int s_confirm, s_confirm_sel;     /* the "are you sure?" prompt: 0 Ok, 1 Cancel */

/* The pause menu's sounds: its own samples, tools/panel_assets.py
 * (panel\<name>.wav beside the exe), kept in memory and played from here. */
enum { SND_SCROLL, SND_CONFIRM, SND_BACK, N_SND };
static void *s_snd[N_SND];

static void *panel_file(const char *name, size_t *size)
{
    char path[MAX_PATH];
    size_t n = GetModuleFileNameA(NULL, path, MAX_PATH);
    FILE *f;
    void *d = NULL;
    long len;

    while (n && path[n - 1] != '\\')
        n--;
    _snprintf(path + n, MAX_PATH - n, "panel\\%s", name);
    path[MAX_PATH - 1] = 0;
    if (!(f = fopen(path, "rb")))
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len > 0 && (d = malloc((size_t)len)) && fread(d, 1, (size_t)len, f) != (size_t)len) {
        free(d);
        d = NULL;
    }
    fclose(f);
    if (size)
        *size = (size_t)len;
    return d;
}

static void menu_sound(int which)
{
    if (s_snd[which])
        PlaySoundA((const char *)s_snd[which], NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}
static volatile int s_turn;             /* the 3D view: -1 / 0 / +1, held */
static int s_swallow;                   /* eat the pad until all is released */

#define BTN_UP    0x0001
#define BTN_DOWN  0x0002
#define BTN_A     0x0100
#define BTN_B     0x0200
#define BTN_X     0x0400
#define BTN_Y     0x0800
#define BTN_L1    0x1000                /* Black: the left shoulder */
#define BTN_R1    0x2000                /* White: the right shoulder */

static unsigned buttons(const uint8_t *r)
{
    int16_t ly;
    unsigned b = r[2];
    int j;
    for (j = 0; j < 8; j++)
        if (r[4 + j] > 0x40)
            b |= 0x100u << j;
    memcpy(&ly, r + 14, 2);
    if (ly > 16000)  b |= BTN_UP;
    if (ly < -16000) b |= BTN_DOWN;
    return b;
}

static void apply(int row)
{
    uint32_t profile = profile_of(app_object());
    const char *ini = getenv("RECOMP_INPUT_INI");     /* settings.ini, see settings.c */

    s_want_row = row;
    costume_overlay(row);
    if (profile)
        write_costume_bytes(profile, row);
    if (ini && *ini)
        WritePrivateProfileStringA("Game", "Costume", s_costumes[row].name, ini);
    fprintf(stderr, "  [COSTUME] panel: %s, restarting the level\n", s_costumes[row].name);
    fflush(stderr);
    if (getenv("RECOMP_COSTUME_RESTART")) {
        restart_parse();
        s_inject_t0 = GetTickCount();
        s_inject_on = 1;
    } else {
        extern void level_restart_request(void);
        level_restart_request();
    }
}

/* Which page of the game's pause menu shows, followed from the presses the
 * menu gets (its pages are script, with nothing of theirs to read): it
 * opens on Audio Options, up and down move round its five lines, the cross
 * opens a page and the circle goes back. The Options page (Control Style,
 * Movement, Camera Mode) gets a fourth row, Subtitles, toggled with the
 * square (costume_panel_draw). */
enum { PAGE_TOP, PAGE_OPTIONS, PAGE_OTHER };
/* The row, in the guest's 640x480: centre, top, size (1 = the font's 36 px),
 * and the square's mark beside it (fraction of the width). */
#define SUB_ROW_X     320.0f
#define SUB_ROW_Y     343.0f            /* the rows' labels: 182, 242, 293 */
#define SUB_ROW_GAP   17.0f             /* label to its value */
#define SUB_ROW_SIZE  0.51f             /* an unfocused row's */
#define SUB_MARK_X    0.365
#define SUB_COL_L     236.0f            /* the row's halves: Subtitles, Difficulty */
#define SUB_COL_R     408.0f
#define PAUSE_LINES   5                 /* Audio, Restart, Options, Combat, Exit */
#define PAUSE_OPTIONS 2
static volatile int s_page, s_pause_cur;

/* Difficulty: options + 0x14B0, a 16-bit 0..2 (the menu's Select Difficulty:
 * Easy, Medium, Hard), read by the level scripts through the DIFFICULTY
 * setting. Changed on the top pause page with the shoulders, it is stored
 * at once and takes hold when the level next starts. */
#define SM_PROFILE_DIFFICULTY 0x14B0u
#define DIFFICULTY_LEVELS     3
static const char *const s_difficulty_name[DIFFICULTY_LEVELS] = { "Easy", "Medium", "Hard" };

static int difficulty_get(void)
{
    uint32_t p = profile_of(app_object());
    int d;
    if (!p)
        return -1;
    d = *(volatile int16_t *)((uint8_t *)g_xbox_mem_offset + p + SM_PROFILE_DIFFICULTY);
    return d >= 0 && d < DIFFICULTY_LEVELS ? d : -1;
}

static void difficulty_set(int d)
{
    uint32_t p = profile_of(app_object());
    if (p)
        *(volatile int16_t *)((uint8_t *)g_xbox_mem_offset + p + SM_PROFILE_DIFFICULTY) = (int16_t)d;
    fprintf(stderr, "  [DIFFICULTY] %s, from the next level start\n", s_difficulty_name[d]);
}

static void pause_track(unsigned edge)
{
    if (s_page == PAGE_TOP) {
        if (edge & BTN_UP)
            s_pause_cur = (s_pause_cur + PAUSE_LINES - 1) % PAUSE_LINES;
        if (edge & BTN_DOWN)
            s_pause_cur = (s_pause_cur + 1) % PAUSE_LINES;
        if (edge & BTN_A)
            s_page = s_pause_cur == PAUSE_OPTIONS ? PAGE_OPTIONS : PAGE_OTHER;
    } else if (edge & BTN_B) {
        s_page = PAGE_TOP;
    } else if (s_page == PAGE_OTHER && (edge & BTN_A)) {
        s_page = PAGE_TOP;              /* a Restart / Exit dialog answered */
    }
}

static void filter(int pad, uint8_t *r)
{
    static unsigned prev;
    static DWORD held_since, last_repeat;
    unsigned now, edge;
    DWORD t;

    if (pad != 0)
        return;
    if (s_inject_on) {
        inject(r);
        return;
    }
    s_paused = game_paused();
    now = buttons(r);
    edge = now & ~prev;
    prev = now;
    t = GetTickCount();

    if (!s_paused) {
        s_open = 0;
        s_confirm = 0;
        s_swallow = 0;
        s_page = PAGE_TOP;
        s_pause_cur = 0;
        shocker_input_filter(r);
        movement_stick(r);
        return;
    }
    if (s_swallow) {
        if (now)
            memset(r + 2, 0, 18);
        else
            s_swallow = 0;
        return;
    }
    if (!s_open) {
        pause_track(edge);
        if ((edge & BTN_X) && s_page == PAGE_OPTIONS) {    /* Options' Subtitles row */
            extern void subtitles_toggle(void);
            subtitles_toggle();
            menu_sound(SND_SCROLL);
        }
        r[4 + 2] = 0;                   /* X is that row's while paused */
        if (s_page == PAGE_OPTIONS && (edge & (BTN_L1 | BTN_R1))) {   /* Options' Difficulty */
            int d = difficulty_get();
            if (d >= 0) {
                d = (d + (edge & BTN_R1 ? 1 : DIFFICULTY_LEVELS - 1)) % DIFFICULTY_LEVELS;
                difficulty_set(d);
                menu_sound(SND_SCROLL);
            }
        }
        r[4 + 4] = r[4 + 5] = 0;        /* the shoulders are that row's while paused */
        if (edge & BTN_Y) {
            read_choice();
            s_sel = s_want_row >= 0 ? s_want_row : worn_row();
            s_open = 1;
            memset(r + 2, 0, 18);
        } else {
            r[4 + 3] = 0;               /* Y is the panel's while paused */
        }
        return;
    }

    if (s_confirm) {
        if (edge & (BTN_UP | BTN_DOWN)) {
            s_confirm_sel = !s_confirm_sel;
            menu_sound(SND_SCROLL);
        }
        if (edge & BTN_B) {
            s_confirm = 0;
            menu_sound(SND_BACK);
        } else if (edge & BTN_A) {
            s_confirm = 0;
            if (s_confirm_sel) {
                menu_sound(SND_BACK);
            } else {
                menu_sound(SND_CONFIRM);
                s_open = 0;
                s_swallow = 1;          /* the A still down is not the menu's */
                apply(s_sel);
            }
        }
        memset(r + 2, 0, 18);
        return;
    }

    /* Open: L1 / R1 or the right stick turn the model, up / down with
     * repeat choose, A apply, B close. */
    {
        int16_t rx;
        memcpy(&rx, r + 16, 2);
        s_turn = ((now & BTN_R1) || rx > 16000) - ((now & BTN_L1) || rx < -16000);
    }
    if (edge & (BTN_UP | BTN_DOWN)) {
        held_since = last_repeat = t;
    } else if ((now & (BTN_UP | BTN_DOWN)) && t - held_since > 400 && t - last_repeat > 110) {
        edge |= now & (BTN_UP | BTN_DOWN);
        last_repeat = t;
    }
    if (edge & BTN_UP)
        s_sel = (s_sel + N_COSTUMES - 1) % N_COSTUMES;
    if (edge & BTN_DOWN)
        s_sel = (s_sel + 1) % N_COSTUMES;
    if (edge & BTN_A) {
        s_confirm = 1;
        s_confirm_sel = 0;
        menu_sound(SND_CONFIRM);
    } else if (edge & BTN_B) {
        s_open = 0;
        s_swallow = 1;
    }
    memset(r + 2, 0, 18);
}

void costume_panel_init(void)
{
    s_snd[SND_SCROLL] = panel_file("button_scroll.wav", NULL);
    s_snd[SND_CONFIRM] = panel_file("button_confirm.wav", NULL);
    s_snd[SND_BACK] = panel_file("button_back.wav", NULL);
    read_choice();
    usb_gamepad_set_report_filter(filter);
}

/* ── Drawing ───────────────────────────────────────────────────── */

static HDC     s_dc;
static HBITMAP s_bmp;
static uint8_t *s_bits;
static uint32_t s_bw, s_bh;
static HFONT   s_font_title, s_font_item, s_font_small;
static uint32_t s_font_h;

/* ── The 3D view ───────────────────────────────────────────────── */

typedef struct { float x, y, z, u, v; } HVert;
typedef struct { uint32_t first, count; int32_t tex; } HPart;
typedef struct { uint32_t w, h; uint32_t *px; } HTex;     /* 0x00RRGGBB */
typedef struct {
    int loaded;                         /* 1 ok, -1 missing or bad */
    uint32_t nv, nt, np, nx;
    HVert *v;
    uint16_t *t;
    HPart *p;
    HTex *x;
    float lo[3], hi[3];
} HModel;

static HModel s_models[N_COSTUMES];

static uint32_t rgb565(uint16_t c)
{
    uint32_t r = (c >> 11 & 31) * 255 / 31, g = (c >> 5 & 63) * 255 / 63, b = (c & 31) * 255 / 31;
    return r << 16 | g << 8 | b;
}

static uint32_t mix(uint32_t a, uint32_t b, int wa, int wb, int d)
{
    uint32_t r = (((a >> 16 & 255) * wa + (b >> 16 & 255) * wb) / d);
    uint32_t g = (((a >> 8 & 255) * wa + (b >> 8 & 255) * wb) / d);
    uint32_t bl = (((a & 255) * wa + (b & 255) * wb) / d);
    return r << 16 | g << 8 | bl;
}

/* The top mip of a DXT1 / DXT5 texture to 0x00RRGGBB (alpha dropped). */
static void decode_dxt(const uint8_t *src, int dxt5, uint32_t w, uint32_t h, uint32_t *out)
{
    uint32_t bx, by, j;
    for (by = 0; by < h; by += 4)
        for (bx = 0; bx < w; bx += 4) {
            uint16_t c0, c1;
            uint32_t bits, pal[4];
            if (dxt5)
                src += 8;               /* the alpha block */
            memcpy(&c0, src, 2);
            memcpy(&c1, src + 2, 2);
            memcpy(&bits, src + 4, 4);
            src += 8;
            pal[0] = rgb565(c0);
            pal[1] = rgb565(c1);
            if (c0 > c1 || dxt5) {
                pal[2] = mix(pal[0], pal[1], 2, 1, 3);
                pal[3] = mix(pal[0], pal[1], 1, 2, 3);
            } else {
                pal[2] = mix(pal[0], pal[1], 1, 1, 2);
                pal[3] = 0;
            }
            for (j = 0; j < 16; j++)
                out[(by + j / 4) * w + bx + j % 4] = pal[bits >> (2 * j) & 3];
        }
}

/* previews\<FILE>.hpv beside the exe (tools/xbxm.py --export). */
static HModel *model(int row)
{
    HModel *m = &s_models[row];
    char path[MAX_PATH], *slash;
    FILE *f;
    uint32_t hdr[5], i;

    if (m->loaded)
        return m->loaded > 0 ? m : NULL;
    m->loaded = -1;
    if (!GetModuleFileNameA(NULL, path, sizeof path) || !(slash = strrchr(path, '\\')))
        return NULL;
    _snprintf(slash + 1, sizeof path - (slash + 1 - path), "previews\\%s.hpv", s_costumes[row].file);
    path[sizeof path - 1] = 0;
    if (!(f = fopen(path, "rb")))
        return NULL;
    if (fread(hdr, 4, 5, f) != 5 || hdr[0] != 0x31565048 /* "HPV1" */
        || hdr[1] > 65536 || hdr[2] > 65536 || hdr[3] > 64 || hdr[4] > 16)
        goto bad;
    m->nv = hdr[1]; m->nt = hdr[2]; m->np = hdr[3]; m->nx = hdr[4];
    m->v = malloc(m->nv * sizeof *m->v);
    m->t = malloc(m->nt * 6);
    m->p = malloc(m->np * sizeof *m->p);
    m->x = calloc(m->nx ? m->nx : 1, sizeof *m->x);
    if (!m->v || !m->t || !m->p || !m->x
        || fread(m->v, sizeof *m->v, m->nv, f) != m->nv
        || fread(m->t, 6, m->nt, f) != m->nt
        || fread(m->p, sizeof *m->p, m->np, f) != m->np)
        goto bad;
    for (i = 0; i < m->nx; i++) {
        uint32_t th[4];
        uint8_t *blocks;
        if (fread(th, 4, 4, f) != 4 || th[0] > 2048 || th[1] > 2048 || th[3] > 8 << 20)
            goto bad;
        blocks = malloc(th[3]);
        m->x[i].w = th[0];
        m->x[i].h = th[1];
        m->x[i].px = malloc((size_t)th[0] * th[1] * 4);
        if (!blocks || !m->x[i].px || fread(blocks, 1, th[3], f) != th[3]) {
            free(blocks);
            goto bad;
        }
        decode_dxt(blocks, th[2] == 0x35545844 /* "DXT5" */, th[0], th[1], m->x[i].px);
        free(blocks);
    }
    fclose(f);
    for (i = 0; i < 3; i++) {
        m->lo[i] = 1e9f;
        m->hi[i] = -1e9f;
    }
    for (i = 0; i < m->nv; i++) {
        const float *p = &m->v[i].x;
        int k;
        for (k = 0; k < 3; k++) {
            if (p[k] < m->lo[k]) m->lo[k] = p[k];
            if (p[k] > m->hi[k]) m->hi[k] = p[k];
        }
    }
    m->loaded = 1;
    return m;
bad:
    fclose(f);
    fprintf(stderr, "  [COSTUME] %s: not a hero preview\n", path);
    return NULL;
}

static float *s_zbuf;
static size_t s_zcap;

/* The model turned by yaw (radians) about its vertical axis, fitted into the
 * canvas box x0..x1, y0..y1: z-buffered, nearest texel, a front light. */
static void draw_model(HModel *m, float yaw, int x0, int y0, int x1, int y1)
{
    int bw = x1 - x0, bh = y1 - y0, i;
    float cy = cosf(yaw), sy = sinf(yaw);
    float cx = (m->lo[0] + m->hi[0]) / 2, cz = (m->lo[2] + m->hi[2]) / 2;
    float ym = (m->lo[1] + m->hi[1]) / 2;
    float wide = m->hi[0] - m->lo[0], deep = m->hi[2] - m->lo[2];
    float reach = sqrtf(wide * wide + deep * deep) / 2;   /* any turn fits */
    float scale = 0.92f * (bw / (2 * reach) < bh / (m->hi[1] - m->lo[1])
                           ? bw / (2 * reach) : bh / (m->hi[1] - m->lo[1]));
    float *sx, *syy, *sz;
    uint32_t p;

    if (bw < 8 || bh < 8)
        return;
    if (s_zcap < (size_t)bw * bh) {
        free(s_zbuf);
        s_zcap = (size_t)bw * bh;
        s_zbuf = malloc(s_zcap * sizeof *s_zbuf);
        if (!s_zbuf) {
            s_zcap = 0;
            return;
        }
    }
    for (i = 0; i < bw * bh; i++)
        s_zbuf[i] = -1e30f;
    sx = malloc(m->nv * 3 * sizeof(float));
    if (!sx)
        return;
    syy = sx + m->nv;
    sz = syy + m->nv;
    for (i = 0; i < (int)m->nv; i++) {
        float x = m->v[i].x - cx, z = m->v[i].z - cz;
        sx[i] = bw / 2.0f + (x * cy + z * sy) * scale;
        syy[i] = bh / 2.0f - (m->v[i].y - ym) * scale;
        sz[i] = -x * sy + z * cy;
    }
    for (p = 0; p < m->np; p++) {
        const HTex *tex = m->p[p].tex >= 0 && (uint32_t)m->p[p].tex < m->nx ? &m->x[m->p[p].tex] : NULL;
        uint32_t t;
        for (t = m->p[p].first; t < m->p[p].first + m->p[p].count && t < m->nt; t++) {
            int a = m->t[3 * t], b = m->t[3 * t + 1], c = m->t[3 * t + 2];
            float ax = sx[a], ay = syy[a], bx = sx[b], by = syy[b], qx = sx[c], qy = syy[c];
            float den = (by - qy) * (ax - qx) + (qx - bx) * (ay - qy);
            int minx, maxx, miny, maxy, x, y, shade;
            float nx, ny, nz, nl;
            if (a >= (int)m->nv || b >= (int)m->nv || c >= (int)m->nv || fabsf(den) < 1e-6f)
                continue;
            /* Face light: the normal in view space, against a light from the
             * front, a little above and left; both faces lit. */
            {
                float ux = (bx - ax), uy = (by - ay), uz = (sz[b] - sz[a]) * scale;
                float vx = (qx - ax), vy = (qy - ay), vz = (sz[c] - sz[a]) * scale;
                nx = uy * vz - uz * vy;
                ny = uz * vx - ux * vz;
                nz = ux * vy - uy * vx;
                nl = sqrtf(nx * nx + ny * ny + nz * nz);
                if (nl < 1e-9f)
                    continue;
                nl = fabsf((nx * -0.3f + ny * -0.4f + nz * 0.87f) / nl);
                shade = (int)(256 * (0.45f + 0.6f * nl));
                if (shade > 256)
                    shade = 256;
            }
            minx = (int)floorf(fminf(ax, fminf(bx, qx)));
            maxx = (int)ceilf(fmaxf(ax, fmaxf(bx, qx)));
            miny = (int)floorf(fminf(ay, fminf(by, qy)));
            maxy = (int)ceilf(fmaxf(ay, fmaxf(by, qy)));
            if (minx < 0) minx = 0;
            if (miny < 0) miny = 0;
            if (maxx > bw - 1) maxx = bw - 1;
            if (maxy > bh - 1) maxy = bh - 1;
            for (y = miny; y <= maxy; y++) {
                uint32_t *row = (uint32_t *)(s_bits + ((size_t)(y0 + y) * s_bw + x0) * 4);
                float *zr = s_zbuf + (size_t)y * bw;
                float py = y + 0.5f;
                for (x = minx; x <= maxx; x++) {
                    float px = x + 0.5f;
                    float l0 = ((by - qy) * (px - qx) + (qx - bx) * (py - qy)) / den;
                    float l1 = ((qy - ay) * (px - qx) + (ax - qx) * (py - qy)) / den;
                    float l2 = 1 - l0 - l1, z;
                    uint32_t col;
                    if (l0 < 0 || l1 < 0 || l2 < 0)
                        continue;
                    z = l0 * sz[a] + l1 * sz[b] + l2 * sz[c];
                    if (z <= zr[x])
                        continue;
                    zr[x] = z;
                    if (tex) {
                        float u = l0 * m->v[a].u + l1 * m->v[b].u + l2 * m->v[c].u;
                        float v = l0 * m->v[a].v + l1 * m->v[b].v + l2 * m->v[c].v;
                        int tu = (int)floorf(u * tex->w) % (int)tex->w;
                        int tv = (int)floorf(v * tex->h) % (int)tex->h;
                        if (tu < 0) tu += tex->w;
                        if (tv < 0) tv += tex->h;
                        col = tex->px[(size_t)tv * tex->w + tu];
                    } else {
                        col = 0x9A9A9A;
                    }
                    row[x] = ((col >> 16 & 255) * shade >> 8) << 16
                           | ((col >> 8 & 255) * shade >> 8) << 8
                           | ((col & 255) * shade >> 8);
                }
            }
        }
    }
    free(sx);
}

static int ensure_canvas(uint32_t w, uint32_t h)
{
    if (s_bmp && s_bw == w && s_bh == h)
        return 1;
    if (!s_dc)
        s_dc = CreateCompatibleDC(NULL);
    if (s_bmp)
        DeleteObject(s_bmp);
    {
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = (LONG)w;
        bi.bmiHeader.biHeight = -(LONG)h;         /* top-down, as the frame is */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        s_bmp = CreateDIBSection(s_dc, &bi, DIB_RGB_COLORS, (void **)&s_bits, NULL, 0);
    }
    if (!s_bmp)
        return 0;
    SelectObject(s_dc, s_bmp);
    s_bw = w; s_bh = h;
    return 1;
}

static void ensure_fonts(uint32_t h)
{
    int u = (int)(h / 480.0 * 16 + 0.5);    /* 16 px at 480 lines */
    if (s_font_title && s_font_h == h)
        return;
    if (s_font_title) DeleteObject(s_font_title);
    if (s_font_item)  DeleteObject(s_font_item);
    if (s_font_small) DeleteObject(s_font_small);
    s_font_title = CreateFontW(-u * 2, 0, 0, 0, FW_HEAVY, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                               CLEARTYPE_QUALITY, 0, L"Segoe UI");
    s_font_item  = CreateFontW(-u, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                               CLEARTYPE_QUALITY, 0, L"Segoe UI");
    s_font_small = CreateFontW(-(u * 3 / 4), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                               0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Symbol");
    s_font_h = h;
}

/* Darken a rectangle to `keep`/256 of itself. */
static void shade(int x0, int y0, int x1, int y1, int keep)
{
    int x, y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)s_bw) x1 = (int)s_bw;
    if (y1 > (int)s_bh) y1 = (int)s_bh;
    for (y = y0; y < y1; y++) {
        uint8_t *p = s_bits + (size_t)y * s_bw * 4 + (size_t)x0 * 4;
        for (x = x0; x < x1; x++, p += 4) {
            p[0] = (uint8_t)(p[0] * keep >> 8);
            p[1] = (uint8_t)(p[1] * keep >> 8);
            p[2] = (uint8_t)(p[2] * keep >> 8);
        }
    }
}

static void text(HFONT f, COLORREF c, int x, int y, int w, int h, UINT fmt, const wchar_t *s)
{
    RECT rc = { x, y, x + w, y + h };
    SelectObject(s_dc, f);
    SetTextColor(s_dc, c);
    DrawTextW(s_dc, s, -1, &rc, fmt | DT_NOPREFIX | DT_SINGLELINE);
}

/* PlayStation button prompts (settings.ini [Controls] ButtonPrompts)? */
static int ps_prompts(void)
{
    static int ps = -1;
    if (ps < 0) {
        const char *e = getenv("RECOMP_BUTTON_PROMPTS");
        ps = e && (!_stricmp(e, "ps4") || !_stricmp(e, "ps5"));
    }
    return ps;
}

/* The face buttons as the prompts setting draws them. */
static const wchar_t *face(char b)
{
    int ps = ps_prompts();
    /* Code points, not literals: the source stays ASCII whatever the
     * compiler takes its encoding to be. */
    static const wchar_t cross[] = { 0x2715, 0 }, circle[] = { 0x25CB, 0 },
                         triangle[] = { 0x25B3, 0 };
    switch (b) {
    case 'a': return ps ? cross : L"A";
    case 'b': return ps ? circle : L"B";
    case 'y': return ps ? triangle : L"Y";
    }
    return L"?";
}

static void draw_hint(void)
{
    int u = (int)(s_bh / 480.0 * 16 + 0.5);
    int w = u * 9, h = u * 2, x = (int)s_bw - w - u, y = u;
    wchar_t s[64];
    shade(x, y, x + w, y + h, 70);
    _snwprintf(s, 64, L"%s  Costume", face('y'));
    s[63] = 0;
    text(s_font_item, RGB(235, 235, 235), x, y, w, h, DT_CENTER | DT_VCENTER, s);
}

static void draw_panel(void)
{
    int u = (int)(s_bh / 480.0 * 16 + 0.5);
    int px0 = (int)(s_bw * 0.50), px1 = (int)(s_bw * 0.94);
    int py0 = (int)(s_bh * 0.07), py1 = (int)(s_bh * 0.93);
    int row_h = (int)(u * 1.55), list_y = py0 + u * 3;
    int worn = s_want_row >= 0 ? s_want_row : worn_row();
    int i, sel = s_sel;
    wchar_t s[96];

    shade(0, 0, (int)s_bw, (int)s_bh, 120);
    shade(px0, py0, px1, py1, 60);
    /* Left: the selected costume's model, turned by L1 / R1. */
    {
        static float yaw;
        static DWORD last;
        DWORD now = GetTickCount();
        int lx0 = (int)(s_bw * 0.06), lx1 = (int)(s_bw * 0.46);
        HModel *mdl = model(sel);
        if (last && now - last < 500)
            yaw += s_turn * 2.2f * (now - last) / 1000.0f;   /* ~126 deg/s */
        last = now;
        shade(lx0, py0, lx1, py1, 60);
        text(s_font_title, RGB(200, 30, 30), lx0, py0 + u, lx1 - lx0, u * 3, DT_CENTER,
             L"COSTUME");
        if (mdl)
            draw_model(mdl, yaw, lx0 + u, py0 + u * 4, lx1 - u, py1 - u * 3);
        text(s_font_item, RGB(240, 240, 240), lx0, py1 - u * 3, lx1 - lx0, u * 2,
             DT_CENTER | DT_VCENTER, s_costumes[sel].label);
        text(s_font_small, RGB(170, 170, 170), lx0, py1 - u * 3 / 2, lx1 - lx0, u,
             DT_CENTER | DT_VCENTER, ps_prompts() ? L"L1 / R1  Turn" : L"Black / White  Turn");
        if (!mdl)
            text(s_font_small, RGB(170, 170, 170), lx0, (py0 + py1) / 2 - u, lx1 - lx0, u * 2,
                 DT_CENTER | DT_VCENTER, L"(no preview: run tools/xbxm.py --export)");
    }

    for (i = 0; i < N_COSTUMES; i++) {
        int y = list_y + i * row_h;
        COLORREF c = i == sel ? RGB(255, 80, 80) : RGB(175, 175, 175);
        if (i == sel)
            shade(px0 + u / 2, y, px1 - u / 2, y + row_h, 20);
        _snwprintf(s, 96, L"%s%s", s_costumes[i].label, i == worn ? L"   - worn" : L"");
        s[95] = 0;
        text(s_font_item, c, px0 + u, y, px1 - px0 - 2 * u, row_h, DT_LEFT | DT_VCENTER, s);
    }
    _snwprintf(s, 96, L"%s Wear and restart level      %s Back", face('a'), face('b'));
    s[95] = 0;
    text(s_font_small, RGB(220, 220, 220), px0, py1 - u * 2, px1 - px0, u * 2,
         DT_CENTER | DT_VCENTER, s);
}

/* ── The prompt, drawn as the pause menu is ─────────────────────── */

/* panel\font.bin (tools/panel_assets.py): the game's chiselwide 36px --
 * atlas size, 256 glyphs, then the atlas's ARGB texels. */
typedef struct { int16_t x, y, cw, ch, ox, oy, adv, gw; } Glyph;
static const uint32_t *s_atlas;
static const Glyph *s_glyphs;
static uint32_t s_aw, s_ah;

static int font_ready(void)
{
    static int tried;
    if (!tried) {
        size_t size = 0;
        const uint8_t *d = (const uint8_t *)panel_file("font.bin", &size);
        tried = 1;
        if (d && size > 8 + 256 * sizeof(Glyph)) {
            s_aw = ((const uint32_t *)d)[0];
            s_ah = ((const uint32_t *)d)[1];
            s_glyphs = (const Glyph *)(d + 8);
            s_atlas = (const uint32_t *)(d + 8 + 256 * sizeof(Glyph));
        }
    }
    return s_atlas != NULL;
}

/* One line centred on (cx, top), in the guest's 640x480 units stretched to
 * the frame as the game's own 2D is; size 1 is the font's 36 px. Bilinear;
 * c (RGB) multiplies the glyph's colour. The font has capitals only: as in
 * the pause menu, a lower-case letter is the capital drawn smaller, on the
 * same baseline (31 down the 41-texel cell). */
static int s_text_grey;                 /* the glyphs' light, not their red */
static float s_text_wide = 1.0f;        /* letters drawn this much wider */
static float s_text_track;              /* and this many 640-units apart */

static void game_text(float cx, float top, float size, COLORREF c, const char *t)
{
    float sx0 = s_bw / 640.0f * size * s_text_wide, sy0 = s_bh / 480.0f * size, pen = 0;
    float track = s_text_track * s_bw / 640.0f;
    const char *q;
    int cr = GetRValue(c), cg = GetGValue(c), cb = GetBValue(c);

    for (q = t; *q; q++)
        pen += s_glyphs[(uint8_t)*q].adv * (*q >= 'a' && *q <= 'z' ? 0.8f : 1.0f);
    pen = cx * s_bw / 640.0f - (pen * sx0 + track * (float)(strlen(t) - 1)) * 0.5f;
    for (q = t; *q; q++) {
        const Glyph *g = &s_glyphs[(uint8_t)*q];
        float small = *q >= 'a' && *q <= 'z' ? 0.8f : 1.0f;
        float sx = sx0 * small, sy = sy0 * small;
        int dw = (int)(g->cw * sx), dh = (int)(g->ch * sy), x, y;
        int x0 = (int)(pen - 2 * sx);
        int y0 = (int)(top * s_bh / 480.0f + 31.0f * (sy0 - sy));
        for (y = 0; y < dh; y++) {
            int dy = y0 + y;
            float v = g->y + (y + 0.5f) / sy - 0.5f;
            int iv = (int)v;
            float fv = v - iv;
            if (dy < 0 || dy >= (int)s_bh || iv < 0 || iv + 1 >= (int)s_ah)
                continue;
            for (x = 0; x < dw; x++) {
                int dx = x0 + x, iu, k, a;
                float u = g->x + (x + 0.5f) / sx - 0.5f, fu, w[4], ch[4] = { 0, 0, 0, 0 };
                const uint32_t *row;
                uint8_t *p;
                iu = (int)u;
                if (dx < 0 || dx >= (int)s_bw || iu < 0 || iu + 1 >= (int)s_aw)
                    continue;
                fu = u - iu;
                w[0] = (1 - fu) * (1 - fv); w[1] = fu * (1 - fv);
                w[2] = (1 - fu) * fv;       w[3] = fu * fv;
                row = s_atlas + (size_t)iv * s_aw + iu;
                for (k = 0; k < 4; k++) {
                    uint32_t t4 = k < 2 ? row[k] : row[s_aw + k - 2];
                    float ta = (t4 >> 24) * w[k];
                    ch[0] += ta;
                    ch[1] += ((t4 >> 16) & 255) * ta;
                    ch[2] += ((t4 >> 8) & 255) * ta;
                    ch[3] += (t4 & 255) * ta;
                }
                a = (int)ch[0];
                if (a <= 0)
                    continue;
                if (s_text_grey) {      /* the pause rows: grey, as bright as the glyph */
                    float l = (ch[1] > ch[2] ? ch[1] : ch[2]) > ch[3] ? (ch[1] > ch[2] ? ch[1] : ch[2]) : ch[3];
                    ch[1] = ch[2] = ch[3] = l;
                }
                p = s_bits + ((size_t)dy * s_bw + dx) * 4;
                p[2] = (uint8_t)((p[2] * (255 - a) + ch[1] / ch[0] * cr / 255 * a) / 255);
                p[1] = (uint8_t)((p[1] * (255 - a) + ch[2] / ch[0] * cg / 255 * a) / 255);
                p[0] = (uint8_t)((p[0] * (255 - a) + ch[3] / ch[0] * cb / 255 * a) / 255);
            }
        }
        pen += g->adv * sx + track;
    }
}

/* A face button as the pause footer draws its own: a light ring, the
 * button's mark inside (a square; X for Xbox prompts). */
static void button_icon(int cx, int cy, int r, int ps)
{
    HPEN pen = CreatePen(PS_SOLID, r / 5 > 1 ? r / 5 : 1, RGB(215, 215, 215)), old;
    HBRUSH hollow = (HBRUSH)GetStockObject(HOLLOW_BRUSH), oldb;
    int k = r * 45 / 100;

    old = (HPEN)SelectObject(s_dc, pen);
    oldb = (HBRUSH)SelectObject(s_dc, hollow);
    Ellipse(s_dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    if (ps) {
        Rectangle(s_dc, cx - k, cy - k, cx + k + 1, cy + k + 1);
    } else {
        MoveToEx(s_dc, cx - k, cy - k, NULL); LineTo(s_dc, cx + k + 1, cy + k + 1);
        MoveToEx(s_dc, cx + k, cy - k, NULL); LineTo(s_dc, cx - k - 1, cy + k + 1);
    }
    SelectObject(s_dc, oldb);
    SelectObject(s_dc, old);
    DeleteObject(pen);
}

/* A shoulder button: a light rounded box, its name inside. */
static void shoulder_icon(int cx, int cy, int r, const wchar_t *name)
{
    HPEN pen = CreatePen(PS_SOLID, r / 5 > 1 ? r / 5 : 1, RGB(215, 215, 215)), old;
    HBRUSH oldb = (HBRUSH)SelectObject(s_dc, GetStockObject(HOLLOW_BRUSH));
    int hw = r * 3 / 2;

    old = (HPEN)SelectObject(s_dc, pen);
    RoundRect(s_dc, cx - hw, cy - r, cx + hw + 1, cy + r + 1, r, r);
    SelectObject(s_dc, oldb);
    SelectObject(s_dc, old);
    DeleteObject(pen);
    text(s_font_small, RGB(215, 215, 215), cx - hw, cy - r, hw * 2, r * 2, DT_CENTER | DT_VCENTER | DT_SINGLELINE, name);
}

static void draw_confirm(void)
{
    /* Up to ~20% dimmer and back, about once a second, on the selected line. */
    int pulse = 215 + (int)(40.0f * sinf(GetTickCount() * 0.0063f));
    COLORREF on = RGB(pulse, pulse, pulse), off = RGB(170, 170, 170);
    int ok = s_confirm_sel == 0;
    int u = (int)(s_bh / 480.0 * 16 + 0.5);
    wchar_t foot[96];

    shade(0, 0, (int)s_bw, (int)s_bh, 40);
    if (font_ready()) {
        game_text(320, 140, 1.0f, RGB(150, 22, 22), "Are you sure?");
        game_text(320, 190, 0.5f, RGB(190, 190, 190), "Restart at the beginning of level?");
        game_text(320, 238, ok ? 0.9f : 0.62f, ok ? on : off, "Ok");
        game_text(320, 276, ok ? 0.62f : 0.9f, ok ? off : on, "Cancel");
    } else {
        text(s_font_title, RGB(200, 30, 30), 0, (int)s_bh / 3, (int)s_bw, u * 3, DT_CENTER, L"Are you sure?");
        text(s_font_item, ok ? RGB(255, 255, 255) : RGB(170, 170, 170), 0, (int)s_bh / 2, (int)s_bw, u * 2, DT_CENTER, L"Ok");
        text(s_font_item, ok ? RGB(170, 170, 170) : RGB(255, 255, 255), 0, (int)s_bh / 2 + u * 2, (int)s_bw, u * 2, DT_CENTER, L"Cancel");
    }
    _snwprintf(foot, 96, L"%s Select      %s Exit", face('a'), face('b'));
    foot[95] = 0;
    text(s_font_small, RGB(220, 220, 220), 0, (int)(s_bh * 0.8), (int)s_bw, u * 2, DT_CENTER | DT_VCENTER, foot);
}

/* Whether costume_panel_draw would draw on this frame: the back end then
 * sends the frame through the CPU instead of sharing it on the GPU. */
int costume_panel_wants_frame(void)
{
    return s_paused && !s_inject_on;
}

/* Called with the frame about to be shown (BGRA, top-down). While the game
 * is paused, draws on a copy and points *pixels / *pitch at it. */
void costume_panel_draw(const void **pixels, uint32_t w, uint32_t h, uint32_t *pitch)
{
    uint32_t y;
    if (!s_paused || s_inject_on || !*pixels || w < 320 || h < 240)
        return;
    if (!ensure_canvas(w, h))
        return;
    ensure_fonts(h);
    for (y = 0; y < h; y++)
        memcpy(s_bits + (size_t)y * w * 4, (const uint8_t *)*pixels + (size_t)y * *pitch, w * 4);
    SetBkMode(s_dc, TRANSPARENT);
    if (s_open) {
        draw_panel();
        if (s_confirm)
            draw_confirm();
    }
    if (!s_open) {
        extern int subtitles_enabled(void);
        int u = (int)(h / 480.0 * 16 + 0.5);
        const wchar_t *mark = ps_prompts() ? L"\x25A1" : L"X";
        draw_hint();
        if (s_page == PAGE_OPTIONS && font_ready()) {
            /* the fourth row, under Camera Mode, as the game draws its rows
             * (label, its value under it, grey), in two halves: Subtitles,
             * the square beside it; Difficulty, its level between the
             * shoulder buttons that change it */
            int d = difficulty_get(), ps = ps_prompts();
            s_text_grey = 1;
            s_text_wide = 1.3f;
            s_text_track = 1.6f;
            game_text(SUB_COL_L, SUB_ROW_Y, SUB_ROW_SIZE, RGB(168, 168, 168), "Subtitles");
            game_text(SUB_COL_L, SUB_ROW_Y + SUB_ROW_GAP, SUB_ROW_SIZE, RGB(168, 168, 168),
                      subtitles_enabled() ? "On" : "Off");
            if (d >= 0) {
                game_text(SUB_COL_R, SUB_ROW_Y, SUB_ROW_SIZE, RGB(168, 168, 168), "Difficulty");
                game_text(SUB_COL_R, SUB_ROW_Y + SUB_ROW_GAP, SUB_ROW_SIZE, RGB(168, 168, 168),
                          s_difficulty_name[d]);
            }
            s_text_grey = 0;
            s_text_wide = 1.0f;
            s_text_track = 0;
            button_icon((int)(w / 640.0 * (SUB_COL_L - 80)), (int)(h / 480.0 * (SUB_ROW_Y + SUB_ROW_GAP * 0.5f + 6)),
                        (int)(h / 480.0 * 9.5f), ps);
            if (d >= 0) {
                int cy = (int)(h / 480.0 * (SUB_ROW_Y + SUB_ROW_GAP + 7));
                shoulder_icon((int)(w / 640.0 * (SUB_COL_R - 44)), cy, (int)(h / 480.0 * 7), ps ? L"L1" : L"LB");
                shoulder_icon((int)(w / 640.0 * (SUB_COL_R + 44)), cy, (int)(h / 480.0 * 7), ps ? L"R1" : L"RB");
            }
            (void)mark;
        }
    }
    GdiFlush();
    *pixels = s_bits;
    *pitch = w * 4;

    /* RECOMP_PANEL_DUMP=<file.bmp>: the drawn frame, every 2 s while shown
     * (frame dumps take guest memory, which never has the panel). */
    {
        static const char *dump = (const char *)-1;
        static DWORD last;
        if (dump == (const char *)-1)
            dump = getenv("RECOMP_PANEL_DUMP");
        if (dump && *dump && GetTickCount() - last > 2000) {
            FILE *f = fopen(dump, "wb");
            last = GetTickCount();
            if (f) {
                BITMAPFILEHEADER fh;
                BITMAPINFOHEADER ih;
                memset(&fh, 0, sizeof fh);
                memset(&ih, 0, sizeof ih);
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof fh + sizeof ih;
                fh.bfSize = fh.bfOffBits + w * h * 4;
                ih.biSize = sizeof ih;
                ih.biWidth = (LONG)w;
                ih.biHeight = -(LONG)h;
                ih.biPlanes = 1;
                ih.biBitCount = 32;
                fwrite(&fh, sizeof fh, 1, f);
                fwrite(&ih, sizeof ih, 1, f);
                fwrite(s_bits, 4, (size_t)w * h, f);
                fclose(f);
            }
        }
    }
}
