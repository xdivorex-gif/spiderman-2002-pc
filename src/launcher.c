/*
 * SpiderManLauncher.exe: the settings window in front of the game.
 *
 * Edits settings.ini next to the game exe (the keys are documented in
 * settings.c, which the game reads at start-up) and starts the game. Plain
 * Win32, no dependencies: one window of standard controls, laid out in
 * 96-dpi units and scaled to the monitor.
 *
 * The background is the cover art (assets/launcher_bg.bmp, built in as
 * resource 1 by launcher.rc), stretched over the window with a darkened
 * panel behind the settings. Labels draw white straight onto it: their
 * background brush is that same picture, offset to where each one sits.
 *
 * The game's stdout and stderr go to play_out.txt / play_log.txt in the
 * project folder (two up from build\Release), where play.bat puts them, so a
 * run started here leaves the same logs.
 */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(linker, "\"/manifestdependency:type='win32' "                  \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "               \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define GAME_EXE   "spiderman_recomp.exe"
#define ID_RES     101
#define ID_ASPECT  102
#define ID_AA      103
#define ID_RENDER  104
#define ID_FULL    105
#define ID_VSYNC   106
#define ID_PLAY    IDOK
#define ID_SAVE    107
#define ID_CLOSE   IDCANCEL

#define ID_FULL_LABEL  108
#define ID_VSYNC_LABEL 109
#define ID_PROMPTS     110
#define ID_CONTROLS    111
#define ID_RESET       112
#define ID_COSTUME     113
#define ID_MAP_BASE    200              /* controls window: 200 + row */

/* Window and panel, 96-dpi units. */
#define WIN_W   636                     /* square: the art's own shape */
#define WIN_H   636
#define PANEL_X 16
#define PANEL_Y 200
#define PANEL_W 604
#define PANEL_H 420

static char    s_dir[MAX_PATH];      /* the exes' folder, with a trailing \ */
static char    s_ini[MAX_PATH];
static HFONT   s_font, s_title_font;
static int     s_dpi = 96;
static HWND    s_res, s_aspect, s_aa, s_render, s_full, s_vsync, s_prompts, s_costume;

/* [Game] Costume: settings.ini value and what the list shows. The values are
 * recomp_manual.c's costume names; the game names only its Secret Store
 * costumes, so the cheat ones go by their hero name and cheat code. */
static const struct { const char *value, *label; } s_costumes[] = {
    { "default",     "Game's choice (Secret Store / cheats)" },
    { "spiderman",   "Spider-Man (classic suit)" },
    { "peter",       "Peter Parker" },
    { "wrestler",    "Wrestler Spider-Man" },
    { "alexross",    "Alex Ross Spider-Man" },
    { "goblin",      "Green Goblin" },
    { "mjspidey",    "mjspidey (GIRLNEXTDOOR)" },
    { "shockspidey", "shockspidey (HERMANSCHULTZ)" },
    { "scispidey",   "scispidey (SERUM)" },
    { "thugspidey",  "thugspidey (KNUCKLES)" },
    { "btgspidey",   "btgspidey (STICKYRICE)" },
    { "sthspidey",   "sthspidey (THUGSRUS)" },
    { "helispidey",  "helispidey (CAPTAINSTACEY)" },
    { "freakspidey", "freakspidey (FREAKOUT)" },
    { "copspidey",   "copspidey (REALHERO)" },
};
#define N_COSTUMES ((int)(sizeof s_costumes / sizeof s_costumes[0]))
static HWND    s_main;
static HBITMAP s_bg;                 /* art + panel, the size of the client area */
static HBRUSH  s_bg_brush;           /* s_bg as a brush, for transparent labels */

static const char *const s_resolutions[] = {
    "640x480", "1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160",
};

static int px(int v) { return MulDiv(v, s_dpi, 96); }

static HWND add(HWND parent, const char *cls, const char *text, DWORD style,
                int x, int y, int w, int h, int id, HFONT font)
{
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             px(x), px(y), px(w), px(h), parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static void combo_add(HWND c, const char *text)
{
    SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)text);
}

static int combo_sel(HWND c) { return (int)SendMessageA(c, CB_GETCURSEL, 0, 0); }

/* The resolution list's text for a size, e.g. "1920 x 1080". */
static void res_label(const char *wxh, char *out, size_t cap)
{
    unsigned w, h;
    if (sscanf(wxh, "%ux%u", &w, &h) == 2)
        _snprintf(out, cap, w == 640 && h == 480 ? "%u x %u  (original)" : "%u x %u", w, h);
    else
        _snprintf(out, cap, "%s", wxh);
    out[cap - 1] = 0;
}

/* Back from the label to "WxH". */
static void res_value(HWND c, char *out, size_t cap)
{
    char t[64];
    unsigned w = 640, h = 480;
    int i = combo_sel(c);
    t[0] = 0;
    if (i >= 0)
        SendMessageA(c, CB_GETLBTEXT, i, (LPARAM)t);
    sscanf(t, "%u x %u", &w, &h);
    _snprintf(out, cap, "%ux%u", w, h);
    out[cap - 1] = 0;
}

static void update_enabled(void)
{
    BOOL gpu = combo_sel(s_render) == 0;
    EnableWindow(s_res, gpu);
    EnableWindow(s_aspect, gpu);
    EnableWindow(s_aa, gpu);
}

static void load(void)
{
    char v[64], label[64], desk[32];
    int i, found = -1;

    /* Resolutions: the list, and the desktop's own size if it is not in it. */
    for (i = 0; i < (int)(sizeof s_resolutions / sizeof s_resolutions[0]); i++) {
        res_label(s_resolutions[i], label, sizeof label);
        combo_add(s_res, label);
    }
    _snprintf(desk, sizeof desk, "%dx%d", GetSystemMetrics(SM_CXSCREEN),
              GetSystemMetrics(SM_CYSCREEN));
    desk[sizeof desk - 1] = 0;
    for (i = 0; i < (int)(sizeof s_resolutions / sizeof s_resolutions[0]); i++)
        if (!strcmp(s_resolutions[i], desk))
            break;
    if (i == (int)(sizeof s_resolutions / sizeof s_resolutions[0])) {
        res_label(desk, label, sizeof label);
        combo_add(s_res, label);
    }

    GetPrivateProfileStringA("Display", "Resolution", "1920x1080", v, sizeof v, s_ini);
    res_label(v, label, sizeof label);
    found = (int)SendMessageA(s_res, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)label);
    if (found < 0)                          /* a custom size from the file */
        found = (int)SendMessageA(s_res, CB_ADDSTRING, 0, (LPARAM)label);
    SendMessageA(s_res, CB_SETCURSEL, found, 0);

    GetPrivateProfileStringA("Display", "AspectRatio", "stretch", v, sizeof v, s_ini);
    SendMessageA(s_aspect, CB_SETCURSEL,
                 strcmp(v, "4:3") == 0 ? 2 : strcmp(v, "widescreen") == 0 ? 0 : 1, 0);
    SendMessageA(s_aa, CB_SETCURSEL,
                 GetPrivateProfileIntA("Display", "Supersampling", 0, s_ini) ? 1 : 0, 0);
    GetPrivateProfileStringA("Graphics", "Renderer", "gpu", v, sizeof v, s_ini);
    SendMessageA(s_render, CB_SETCURSEL, _stricmp(v, "cpu") == 0 ? 1 : 0, 0);
    GetPrivateProfileStringA("Controls", "ButtonPrompts", "xbox", v, sizeof v, s_ini);
    SendMessageA(s_prompts, CB_SETCURSEL,
                 !_stricmp(v, "ps4") ? 2 : !_stricmp(v, "ps5") || !_stricmp(v, "playstation") ? 1 : 0,
                 0);
    GetPrivateProfileStringA("Game", "Costume", "default", v, sizeof v, s_ini);
    for (i = N_COSTUMES - 1; i > 0 && _stricmp(v, s_costumes[i].value); i--)
        ;
    SendMessageA(s_costume, CB_SETCURSEL, i, 0);
    SendMessageA(s_full, BM_SETCHECK,
                 GetPrivateProfileIntA("Display", "Fullscreen", 1, s_ini) ? BST_CHECKED
                                                                         : BST_UNCHECKED, 0);
    SendMessageA(s_vsync, BM_SETCHECK,
                 GetPrivateProfileIntA("Display", "VSync", 1, s_ini) ? BST_CHECKED
                                                                    : BST_UNCHECKED, 0);
    update_enabled();
}

static int save(void)
{
    char v[64];
    BOOL ok = TRUE;
    res_value(s_res, v, sizeof v);
    ok &= WritePrivateProfileStringA("Display", "Resolution", v, s_ini);
    ok &= WritePrivateProfileStringA("Display", "AspectRatio",
                                     combo_sel(s_aspect) == 2 ? "4:3"
                                     : combo_sel(s_aspect) == 1 ? "stretch" : "widescreen", s_ini);
    ok &= WritePrivateProfileStringA("Display", "Supersampling",
                                     combo_sel(s_aa) == 1 ? "1" : "0", s_ini);
    ok &= WritePrivateProfileStringA("Display", "Fullscreen",
                                     SendMessageA(s_full, BM_GETCHECK, 0, 0) == BST_CHECKED
                                         ? "1" : "0", s_ini);
    ok &= WritePrivateProfileStringA("Display", "VSync",
                                     SendMessageA(s_vsync, BM_GETCHECK, 0, 0) == BST_CHECKED
                                         ? "1" : "0", s_ini);
    ok &= WritePrivateProfileStringA("Graphics", "Renderer",
                                     combo_sel(s_render) == 1 ? "cpu" : "gpu", s_ini);
    ok &= WritePrivateProfileStringA("Controls", "ButtonPrompts",
                                     combo_sel(s_prompts) == 2 ? "ps4"
                                     : combo_sel(s_prompts) == 1 ? "ps5" : "xbox", s_ini);
    {
        int c = combo_sel(s_costume);
        ok &= WritePrivateProfileStringA("Game", "Costume",
                                         s_costumes[c > 0 && c < N_COSTUMES ? c : 0].value,
                                         s_ini);
    }
    if (!ok)
        MessageBoxA(NULL, "Could not write settings.ini.", "Spider-Man", MB_ICONERROR);
    return ok;
}

static HANDLE log_file(const char *dir, const char *name)
{
    char path[MAX_PATH];
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    _snprintf(path, sizeof path, "%s%s", dir, name);
    path[sizeof path - 1] = 0;
    return CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

static int play(void)
{
    char exe[MAX_PATH], cwd[MAX_PATH], probe[MAX_PATH];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE out, err;
    BOOL ok;

    _snprintf(exe, sizeof exe, "%s%s", s_dir, GAME_EXE);
    exe[sizeof exe - 1] = 0;
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(NULL, "spiderman_recomp.exe was not found next to the launcher.",
                    "Spider-Man", MB_ICONERROR);
        return 0;
    }
    /* Logs where play.bat leaves them: the project folder, if this is a
     * build folder inside one; else next to the exe. */
    _snprintf(cwd, sizeof cwd, "%s..\\..\\", s_dir);
    cwd[sizeof cwd - 1] = 0;
    _snprintf(probe, sizeof probe, "%splay.bat", cwd);
    probe[sizeof probe - 1] = 0;
    if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES)
        strcpy(cwd, s_dir);

    out = log_file(cwd, "play_out.txt");
    err = log_file(cwd, "play_log.txt");
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out;
    si.hStdError = err;
    ok = CreateProcessA(exe, NULL, NULL, NULL, TRUE, 0, NULL, cwd, &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
    if (!ok) {
        MessageBoxA(NULL, "Could not start the game.", "Spider-Man", MB_ICONERROR);
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

/* ---- the Controls window ---------------------------------------------- */

/* One row per game button: its settings.ini key (input_map's control name),
 * what the player sees, the controller default (input_map's source names,
 * as xbox_InputMap_PadMapDefaults has them) and the keyboard default (as
 * keyboard.c's table has it). A NULL pad default is keyboard only: the
 * sticks as keys. */
static const struct {
    const char *key, *label, *pad, *keys;
} s_rows[] = {
    { "a",             "A  /  Cross",              "south",             "Z" },
    { "b",             "B  /  Circle",             "east",              "X" },
    { "x",             "X  /  Square",             "west",              "C" },
    { "y",             "Y  /  Triangle",           "north",             "V" },
    { "black",         "Black  /  L1",             "left_shoulder",     "E" },
    { "white",         "White  /  R1",             "right_shoulder",    "Q" },
    { "left_trigger",  "Left trigger  /  L2",      "left_trigger",      "1" },
    { "right_trigger", "Right trigger  /  R2",     "right_trigger",     "3" },
    { "start",         "Start  /  Options",        "start",             "Enter" },
    { "back",          "Back  /  Share",           "back",              "Backspace" },
    { "left_thumb",    "Left stick click  /  L3",  "left_stick_click",  "Shift" },
    { "right_thumb",   "Right stick click  /  R3", "right_stick_click", "Ctrl" },
    { "dpad_up",       "D-pad up",                 "dpad_up",           "Up" },
    { "dpad_down",     "D-pad down",               "dpad_down",         "Down" },
    { "dpad_left",     "D-pad left",               "dpad_left",         "Left" },
    { "dpad_right",    "D-pad right",              "dpad_right",        "Right" },
    { "left_stick_up",     "Left stick up",        NULL, "W, Numpad8" },
    { "left_stick_down",   "Left stick down",      NULL, "S, Numpad2" },
    { "left_stick_left",   "Left stick left",      NULL, "A, Numpad4" },
    { "left_stick_right",  "Left stick right",     NULL, "D, Numpad6" },
    { "right_stick_up",    "Right stick up",       NULL, "I" },
    { "right_stick_down",  "Right stick down",     NULL, "K" },
    { "right_stick_left",  "Right stick left",     NULL, "J" },
    { "right_stick_right", "Right stick right",    NULL, "L" },
};
#define N_ROWS (int)(sizeof s_rows / sizeof s_rows[0])

/* The physical controls a game button can come from. */
static const struct { const char *name, *label; } s_sources[] = {
    { "south", "Cross / A" },             { "east", "Circle / B" },
    { "west", "Square / X" },             { "north", "Triangle / Y" },
    { "left_shoulder", "L1 / LB" },       { "right_shoulder", "R1 / RB" },
    { "left_trigger", "L2 / LT" },        { "right_trigger", "R2 / RT" },
    { "start", "Options / Start" },       { "back", "Share / Back" },
    { "left_stick_click", "L3" },         { "right_stick_click", "R3" },
    { "dpad_up", "D-pad up" },            { "dpad_down", "D-pad down" },
    { "dpad_left", "D-pad left" },        { "dpad_right", "D-pad right" },
    { "none", "(nothing)" },
};
#define N_SOURCES (int)(sizeof s_sources / sizeof s_sources[0])

static HWND   s_ctl;                      /* the Controls window, while open */
static HWND   s_ctl_pad[N_ROWS], s_ctl_keys[N_ROWS], s_ctl_dz;
static HBRUSH s_ctl_brush;

static void ctl_fill(int defaults)
{
    char v[96];
    int r, i;
    for (r = 0; r < N_ROWS; r++) {
        if (s_rows[r].pad) {
            int sel = 0;
            v[0] = 0;
            if (!defaults)
                GetPrivateProfileStringA("Pad", s_rows[r].key, "", v, sizeof v, s_ini);
            if (!v[0])
                strcpy(v, s_rows[r].pad);
            for (i = 0; i < N_SOURCES; i++)
                if (!_stricmp(v, s_sources[i].name))
                    sel = i;
            SendMessageA(s_ctl_pad[r], CB_SETCURSEL, sel, 0);
        }
        if (defaults)
            strcpy(v, s_rows[r].keys);
        else
            GetPrivateProfileStringA("Keyboard", s_rows[r].key, s_rows[r].keys, v, sizeof v, s_ini);
        SetWindowTextA(s_ctl_keys[r], v);
    }
    if (defaults)
        strcpy(v, "15");
    else
        GetPrivateProfileStringA("Pad", "Deadzone", "15", v, sizeof v, s_ini);
    SetWindowTextA(s_ctl_dz, v);
}

static void ctl_save(void)
{
    char v[96];
    int r, dz;
    for (r = 0; r < N_ROWS; r++) {
        if (s_rows[r].pad) {
            int i = combo_sel(s_ctl_pad[r]);
            WritePrivateProfileStringA("Pad", s_rows[r].key,
                                       i >= 0 ? s_sources[i].name : s_rows[r].pad, s_ini);
        }
        GetWindowTextA(s_ctl_keys[r], v, sizeof v);
        WritePrivateProfileStringA("Keyboard", s_rows[r].key, v[0] ? v : "none", s_ini);
    }
    GetWindowTextA(s_ctl_dz, v, sizeof v);
    dz = atoi(v);
    if (dz < 0) dz = 0;
    if (dz > 95) dz = 95;
    _snprintf(v, sizeof v, "%d", dz);
    v[sizeof v - 1] = 0;
    WritePrivateProfileStringA("Pad", "Deadzone", v, s_ini);
}

#define CTL_ROW_H 25
#define CTL_TOP   44

static void ctl_build(HWND w)
{
    int r, i, y = CTL_TOP;
    add(w, "STATIC", "Game button", SS_LEFT, 20, 16, 180, 20, 0, s_title_font);
    add(w, "STATIC", "Controller", SS_LEFT, 214, 16, 180, 20, 0, s_title_font);
    add(w, "STATIC", "Keyboard", SS_LEFT, 408, 16, 180, 20, 0, s_title_font);
    for (r = 0; r < N_ROWS; r++, y += CTL_ROW_H) {
        add(w, "STATIC", s_rows[r].label, SS_LEFT, 20, y + 3, 190, 20, 0, s_font);
        if (s_rows[r].pad) {
            s_ctl_pad[r] = add(w, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                               214, y, 180, 300, ID_MAP_BASE + r, s_font);
            for (i = 0; i < N_SOURCES; i++)
                combo_add(s_ctl_pad[r], s_sources[i].label);
        }
        s_ctl_keys[r] = add(w, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                            408, y + 1, 172, 22, 0, s_font);
    }
    y += 6;
    add(w, "STATIC", "Stick deadzone (%)", SS_LEFT, 20, y + 3, 190, 20, 0, s_font);
    s_ctl_dz = add(w, "EDIT", "", WS_BORDER | ES_NUMBER | WS_TABSTOP, 214, y + 1, 60, 22, 0, s_font);
    y += 32;
    add(w, "STATIC",
        "Keys: letters, digits, Space, Enter, Shift, Ctrl, Alt, Tab, Up, Down, Left, "
        "Right, Numpad0-9, F1-F12, Mouse1-3. Several keys: separate with commas.",
        SS_LEFT, 20, y, 560, 40, 0, s_font);
    y += 48;
    add(w, "BUTTON", "OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 288, y, 92, 30, IDOK, s_font);
    add(w, "BUTTON", "Defaults", BS_PUSHBUTTON | WS_TABSTOP, 388, y, 92, 30, ID_RESET, s_font);
    add(w, "BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, 488, y, 92, 30, IDCANCEL, s_font);
}

static int ctl_height(void) { return CTL_TOP + N_ROWS * CTL_ROW_H + 6 + 32 + 48 + 46; }

static LRESULT CALLBACK ctl_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        ctl_build(w);
        ctl_fill(0);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            ctl_save();
            DestroyWindow(w);
            return 0;
        case ID_RESET:
            ctl_fill(1);
            return 0;
        case IDCANCEL:
            DestroyWindow(w);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp, RGB(255, 255, 255));
        SetBkColor((HDC)wp, RGB(18, 18, 22));
        return (LRESULT)s_ctl_brush;
    case WM_DESTROY:
        EnableWindow(s_main, TRUE);
        SetForegroundWindow(s_main);
        s_ctl = NULL;
        return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

static void open_controls(void)
{
    static int registered;
    RECT r, mr;
    if (s_ctl) {
        SetForegroundWindow(s_ctl);
        return;
    }
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = ctl_proc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
        s_ctl_brush = CreateSolidBrush(RGB(18, 18, 22));
        wc.hbrBackground = s_ctl_brush;
        wc.lpszClassName = "SpiderManControls";
        RegisterClassA(&wc);
        registered = 1;
    }
    r.left = 0; r.top = 0; r.right = px(600); r.bottom = px(ctl_height());
    AdjustWindowRectEx(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    /* Centred on the work area: it is tall, and below the launcher it ran
     * off the bottom of a 150%-scaled 1440p screen. */
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &mr, 0))
        GetWindowRect(s_main, &mr);
    {
        int ww = r.right - r.left, wh = r.bottom - r.top;
        int x = mr.left + ((mr.right - mr.left) - ww) / 2;
        int y = mr.top + ((mr.bottom - mr.top) - wh) / 2;
        if (y < mr.top) y = mr.top;
        s_ctl = CreateWindowExA(WS_EX_DLGMODALFRAME, "SpiderManControls", "Spider-Man - Controls",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, ww, wh,
                                s_main, NULL, GetModuleHandleA(NULL), NULL);
    }
    if (!s_ctl)
        return;
    EnableWindow(s_main, FALSE);
    ShowWindow(s_ctl, SW_SHOW);
}

/* The cover art stretched over the client area, with the settings panel
 * darkened so white text reads on it. */
static void make_background(HWND w)
{
    HINSTANCE inst = GetModuleHandleA(NULL);
    HBITMAP art = (HBITMAP)LoadImageA(inst, MAKEINTRESOURCEA(1), IMAGE_BITMAP, 0, 0,
                                      LR_CREATEDIBSECTION);
    HDC screen = GetDC(w), dst = CreateCompatibleDC(screen), src = CreateCompatibleDC(screen);
    int cw = px(WIN_W), ch = px(WIN_H);
    BITMAP bm;

    s_bg = CreateCompatibleBitmap(screen, cw, ch);
    SelectObject(dst, s_bg);
    if (art && GetObjectA(art, sizeof bm, &bm)) {
        /* The art at its own shape across the full width (the logo runs
         * nearly edge to edge); the window's extra height below is black,
         * behind the settings panel. */
        int ah = MulDiv(bm.bmHeight, cw, bm.bmWidth);
        RECT below = { 0, ah, cw, ch };
        SelectObject(src, art);
        SetStretchBltMode(dst, HALFTONE);
        SetBrushOrgEx(dst, 0, 0, NULL);
        StretchBlt(dst, 0, 0, cw, ah, src, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
        if (ah < ch)
            FillRect(dst, &below, (HBRUSH)GetStockObject(BLACK_BRUSH));
    } else {
        RECT r = { 0, 0, cw, ch };
        FillRect(dst, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    {
        /* Black at ~70% over the panel; a 1x1 black source stretched. */
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 180, 0 };
        HBITMAP one = CreateCompatibleBitmap(screen, 1, 1);
        RECT r = { 0, 0, 1, 1 };
        SelectObject(src, one);
        FillRect(src, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        AlphaBlend(dst, px(PANEL_X), px(PANEL_Y), px(PANEL_W), px(PANEL_H), src, 0, 0, 1, 1, bf);
        DeleteObject(one);
    }
    DeleteDC(src);
    DeleteDC(dst);
    ReleaseDC(w, screen);
    if (art)
        DeleteObject(art);
    s_bg_brush = CreatePatternBrush(s_bg);
}

static void build(HWND w)
{
    const int lx = 36, cx = 180, cw = 420, rh = 32;
    int y = PANEL_Y + 12, i;
    HWND c;

    add(w, "STATIC", "Xbox recompilation", SS_LEFT, 26, 62, 300, 20, 0, s_font);

    add(w, "STATIC", "DISPLAY", SS_LEFT, lx, y, 200, 20, 0, s_title_font);
    y += 26;
    add(w, "STATIC", "Resolution", SS_LEFT, lx, y + 5, 130, 20, 0, s_font);
    s_res = add(w, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                cx, y, cw, 220, ID_RES, s_font);
    y += rh;
    add(w, "STATIC", "Aspect ratio", SS_LEFT, lx, y + 5, 130, 20, 0, s_font);
    s_aspect = add(w, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP,
                   cx, y, cw, 120, ID_ASPECT, s_font);
    combo_add(s_aspect, "Widescreen (see more to the sides)");
    combo_add(s_aspect, "Stretch to fill the screen");
    combo_add(s_aspect, "Original 4:3 (bars at the sides)");
    y += rh;
    add(w, "STATIC", "Anti-aliasing", SS_LEFT, lx, y + 5, 130, 20, 0, s_font);
    s_aa = add(w, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP,
               cx, y, cw, 120, ID_AA, s_font);
    combo_add(s_aa, "Off");
    combo_add(s_aa, "Supersampling 2x2 (sharper edges)");
    y += rh + 4;
    /* Checkboxes without text: a themed checkbox draws its text black
     * whatever the colour asked for. The white labels beside them toggle
     * them too. */
    s_full = add(w, "BUTTON", "", BS_AUTOCHECKBOX | WS_TABSTOP, cx, y, 18, 20, ID_FULL, s_font);
    add(w, "STATIC", "Fullscreen", SS_LEFT | SS_NOTIFY, cx + 22, y + 1, 100, 20,
        ID_FULL_LABEL, s_font);
    s_vsync = add(w, "BUTTON", "", BS_AUTOCHECKBOX | WS_TABSTOP, cx + 150, y, 18, 20,
                  ID_VSYNC, s_font);
    add(w, "STATIC", "VSync", SS_LEFT | SS_NOTIFY, cx + 172, y + 1, 100, 20,
        ID_VSYNC_LABEL, s_font);
    y += 30;

    add(w, "STATIC", "GRAPHICS", SS_LEFT, lx, y, 200, 20, 0, s_title_font);
    y += 26;
    add(w, "STATIC", "Renderer", SS_LEFT, lx, y + 5, 130, 20, 0, s_font);
    s_render = add(w, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP,
                   cx, y, cw, 120, ID_RENDER, s_font);
    combo_add(s_render, "GPU (fast, any resolution)");
    combo_add(s_render, "CPU (reference, 640x480, slow)");
    y += rh;
    /* Worn from the next level start (recomp_manual.c, sub_000F0690_enter). */
    add(w, "STATIC", "Costume", SS_LEFT, lx, y + 5, 130, 20, 0, s_font);
    s_costume = add(w, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                    cx, y, cw, 300, ID_COSTUME, s_font);
    for (i = 0; i < N_COSTUMES; i++)
        combo_add(s_costume, s_costumes[i].label);
    y += 36;

    add(w, "STATIC", "CONTROLS", SS_LEFT, lx, y, 200, 20, 0, s_title_font);
    y += 26;
    add(w, "STATIC", "Button prompts", SS_LEFT, lx, y + 5, 130, 20, 0, s_font);
    s_prompts = add(w, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP,
                    cx, y, 250, 120, ID_PROMPTS, s_font);
    combo_add(s_prompts, "Xbox");
    combo_add(s_prompts, "PlayStation 5 (DualSense)");
    combo_add(s_prompts, "PlayStation 4");
    add(w, "BUTTON", "Mapping...", BS_PUSHBUTTON | WS_TABSTOP, cx + 260, y - 1, 160, 28,
        ID_CONTROLS, s_font);
    y += 40;

    c = add(w, "STATIC",
            "F11 or Alt+Enter switches fullscreen in game. For frame generation, "
            "use NVIDIA Smooth Motion or Lossless Scaling.",
            SS_LEFT, lx, y, 564, 40, 0, s_font);
    (void)c;

    y = PANEL_Y + PANEL_H - 44;
    add(w, "BUTTON", "Play", BS_DEFPUSHBUTTON | WS_TABSTOP, 312, y, 92, 30, ID_PLAY, s_font);
    add(w, "BUTTON", "Save", BS_PUSHBUTTON | WS_TABSTOP, 412, y, 92, 30, ID_SAVE, s_font);
    add(w, "BUTTON", "Close", BS_PUSHBUTTON | WS_TABSTOP, 512, y, 92, 30, ID_CLOSE, s_font);
}

static void toggle(HWND box)
{
    SendMessageA(box, BM_SETCHECK,
                 SendMessageA(box, BM_GETCHECK, 0, 0) == BST_CHECKED ? BST_UNCHECKED
                                                                     : BST_CHECKED, 0);
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        s_main = w;
        make_background(w);
        build(w);
        load();
        SetFocus(GetDlgItem(w, ID_PLAY));
        return 0;
    case WM_ERASEBKGND: {
        HDC src = CreateCompatibleDC((HDC)wp);
        RECT r;
        GetClientRect(w, &r);
        SelectObject(src, s_bg);
        BitBlt((HDC)wp, 0, 0, r.right, r.bottom, src, 0, 0, SRCCOPY);
        DeleteDC(src);
        return 1;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_CONTROLS:
            open_controls();
            return 0;
        case ID_FULL_LABEL:
            toggle(s_full);
            return 0;
        case ID_VSYNC_LABEL:
            toggle(s_vsync);
            return 0;
        case ID_RENDER:
            if (HIWORD(wp) == CBN_SELCHANGE)
                update_enabled();
            return 0;
        case ID_PLAY:
            if (save() && play())
                DestroyWindow(w);
            return 0;
        case ID_SAVE:
            save();
            return 0;
        case ID_CLOSE:
            DestroyWindow(w);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        /* White on the background picture, lined up with where the
         * control sits. */
        POINT p = { 0, 0 };
        MapWindowPoints((HWND)lp, w, &p, 1);
        SetTextColor((HDC)wp, RGB(255, 255, 255));
        SetBkMode((HDC)wp, TRANSPARENT);
        SetBrushOrgEx((HDC)wp, -p.x, -p.y, NULL);
        return (LRESULT)s_bg_brush;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    LOGFONTA lf;
    RECT r;
    HWND w;
    HDC dc;
    MSG msg;
    char *slash;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES };

    (void)prev; (void)cmd;
    SetProcessDPIAware();
    InitCommonControlsEx(&icc);
    GetModuleFileNameA(NULL, s_dir, sizeof s_dir);
    slash = strrchr(s_dir, '\\');
    if (slash) slash[1] = 0;
    _snprintf(s_ini, sizeof s_ini, "%ssettings.ini", s_dir);
    s_ini[sizeof s_ini - 1] = 0;

    dc = GetDC(NULL);
    s_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    /* Segoe UI at the system's 9 pt, sized for this monitor. */
    memset(&lf, 0, sizeof lf);
    lf.lfHeight = -px(15);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    strcpy(lf.lfFaceName, "Segoe UI");
    s_font = CreateFontIndirectA(&lf);
    lf.lfHeight = -px(14);                  /* section headings */
    lf.lfWeight = FW_BOLD;
    s_title_font = CreateFontIndirectA(&lf);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    wc.lpszClassName = "SpiderManLauncher";
    RegisterClassA(&wc);

    r.left = 0; r.top = 0; r.right = px(WIN_W); r.bottom = px(WIN_H);
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    w = CreateWindowExA(0, wc.lpszClassName, "Spider-Man - Launcher",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                        NULL, NULL, inst, NULL);
    ShowWindow(w, show);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        /* Tab and Enter for whichever window is in front. */
        if (!IsDialogMessageA(s_ctl ? s_ctl : w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    return 0;
}
