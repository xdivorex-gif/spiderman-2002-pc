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
 * closes the panel and restarts the level through the pause menu's own
 * Restart Level, by pressing its buttons (RECOMP_COSTUME_RESTART, below):
 * the hero is built at level load, so a new costume needs a new start.
 *
 * Input goes through the toolkit's report filter (usb_gamepad.h), which sees
 * every pad report just before the game does -- the pad, the keyboard and
 * pad scripts already folded in, in Xbox terms. While the panel is open the
 * game gets an empty report. Drawing happens on the finished frame just
 * before it goes to the window (gpu_backend.c present_scaled), with GDI on a
 * copy: only while the game is paused, so it costs nothing in play. That is
 * the GPU renderer at a raised resolution (RECOMP_RENDER_SIZE); at 640x480
 * the window shows guest memory directly and the panel is not drawn.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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
static const struct { const char *name; const wchar_t *label; int index; } s_costumes[] = {
    { "spiderman",   L"Spider-Man",           -1 },
    { "peter",       L"Peter Parker",          3 },
    { "wrestler",    L"Wrestler Spider-Man",   4 },
    { "alexross",    L"Alex Ross Spider-Man",  5 },
    { "goblin",      L"Green Goblin",          6 },
    { "mjspidey",    L"mjspidey (GIRLNEXTDOOR)", 7 },
    { "shockspidey", L"shockspidey (HERMANSCHULTZ)", 8 },
    { "scispidey",   L"scispidey (SERUM)",     9 },
    { "thugspidey",  L"thugspidey (KNUCKLES)", 10 },
    { "btgspidey",   L"btgspidey (STICKYRICE)", 11 },
    { "sthspidey",   L"sthspidey (THUGSRUS)",  12 },
    { "helispidey",  L"helispidey (CAPTAINSTACEY)", 13 },
    { "freakspidey", L"freakspidey (FREAKOUT)", 14 },
    { "copspidey",   L"copspidey (REALHERO)",  15 },
};
#define N_COSTUMES ((int)(sizeof s_costumes / sizeof s_costumes[0]))

static volatile int s_want_row = -2;    /* row of s_costumes; -1 game's choice; -2 unread */

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

static int game_paused(void)
{
    uint32_t app = app_object();
    return app && GUEST8(app + SM_APP_PAUSED);
}

/* ── Pressing the pause menu's Restart Level ───────────────────── */

/* RECOMP_COSTUME_RESTART: the presses, comma-separated: up down left right
 * a b x y start back, a count as up*6, and waitN (ms). Pause menu: Audio
 * Options, Restart Level, Options, Combat Controls, Exit to Menu; then
 * "Restart at the beginning of level?" Ok / Cancel. */
#define RESTART_DEFAULT "wait300,up*6,down,a,wait900,up,a"
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

static volatile int s_open, s_sel, s_paused;
static int s_swallow;                   /* eat the pad until all is released */

#define BTN_UP    0x0001
#define BTN_DOWN  0x0002
#define BTN_A     0x0100
#define BTN_B     0x0200
#define BTN_Y     0x0800

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
    if (profile)
        write_costume_bytes(profile, row);
    if (ini && *ini)
        WritePrivateProfileStringA("Game", "Costume", s_costumes[row].name, ini);
    fprintf(stderr, "  [COSTUME] panel: %s, restarting the level\n", s_costumes[row].name);
    fflush(stderr);
    restart_parse();
    s_inject_t0 = GetTickCount();
    s_inject_on = 1;
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
        s_swallow = 0;
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

    /* Open: up / down with repeat, A apply, B close. */
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
        s_open = 0;
        apply(s_sel);
    } else if (edge & BTN_B) {
        s_open = 0;
        s_swallow = 1;
    }
    memset(r + 2, 0, 18);
}

void costume_panel_init(void)
{
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

/* The face buttons as the prompts setting draws them. */
static const wchar_t *face(char b)
{
    static int ps = -1;
    if (ps < 0) {
        const char *e = getenv("RECOMP_BUTTON_PROMPTS");
        ps = e && (!_stricmp(e, "ps4") || !_stricmp(e, "ps5"));
    }
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
    /* Left: where the 3D preview goes; the name for now. */
    shade((int)(s_bw * 0.06), py0, (int)(s_bw * 0.46), py1, 60);
    text(s_font_title, RGB(200, 30, 30), (int)(s_bw * 0.06), py0 + u, (int)(s_bw * 0.40),
         u * 3, DT_CENTER, L"COSTUME");
    text(s_font_item, RGB(240, 240, 240), (int)(s_bw * 0.06), (py0 + py1) / 2 - u,
         (int)(s_bw * 0.40), u * 2, DT_CENTER | DT_VCENTER, s_costumes[sel].label);

    for (i = 0; i < N_COSTUMES; i++) {
        int y = list_y + i * row_h;
        COLORREF c = i == sel ? RGB(255, 255, 255) : RGB(170, 170, 170);
        if (i == sel)
            shade(px0 + u / 2, y, px1 - u / 2, y + row_h, 30);
        _snwprintf(s, 96, L"%s%s", s_costumes[i].label, i == worn ? L"   - worn" : L"");
        s[95] = 0;
        text(s_font_item, c, px0 + u, y, px1 - px0 - 2 * u, row_h, DT_LEFT | DT_VCENTER, s);
    }
    _snwprintf(s, 96, L"%s Wear and restart level      %s Back", face('a'), face('b'));
    s[95] = 0;
    text(s_font_small, RGB(220, 220, 220), px0, py1 - u * 2, px1 - px0, u * 2,
         DT_CENTER | DT_VCENTER, s);
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
    if (s_open)
        draw_panel();
    else
        draw_hint();
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
