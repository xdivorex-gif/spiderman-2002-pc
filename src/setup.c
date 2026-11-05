/*
 * SpiderManSetup.exe: installs the port from the player's own copy of the game.
 *
 * One window: where the game is (the unpacked disc, default.xbe and data\),
 * where to install, whether to copy the game in or use it where it is, and
 * Install. It checks default.xbe is the release the port was made from (its
 * SHA-1), writes the two exes it carries (resources 201 and 202, setup.rc),
 * copies the game into <install>\game or points settings.ini [Game] DataDir
 * at it, and makes Desktop and Start-menu shortcuts to the launcher. The game
 * makes the rest from the player's copy itself (legal_pc.c). Nothing from the
 * game is in this exe.
 *
 * Plain Win32, no dependencies; the copy runs on a worker thread that posts
 * its progress to the window.
 */
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <stdio.h>
#include <wchar.h>

#pragma comment(linker, "\"/manifestdependency:type='win32' "                  \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "               \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

/* default.xbe of the release the port is built from (NTSC, 4 194 304 bytes). */
static const char XBE_SHA1[] = "09fad7548d11503458882f5c8ab45d8ed86d2a42";

#define RES_GAME_EXE     201
#define RES_LAUNCHER_EXE 202

#define ID_SRC       101
#define ID_SRC_BROWSE 102
#define ID_DST       103
#define ID_DST_BROWSE 104
#define ID_COPY      105
#define ID_INSTALL   IDOK
#define ID_CLOSE     IDCANCEL
#define ID_STATUS    106
#define ID_PROGRESS  107

#define WM_SETUP_PROGRESS (WM_APP + 1)  /* wParam: done / 1 MB, lParam: total / 1 MB */
#define WM_SETUP_DONE     (WM_APP + 2)  /* wParam: 1 ok; lParam: wchar_t * message, freed */

static HWND s_wnd, s_src, s_dst, s_copy, s_install, s_status, s_progress;
static HFONT s_font, s_title_font;
static int s_dpi = 96;
static wchar_t s_src_dir[MAX_PATH], s_dst_dir[MAX_PATH];
static int s_copy_game;
static int s_no_shortcuts;             /* /noshortcuts: a test install */
/* /silent /game=<dir> /dest=<dir> [/nocopy]: no window; the result goes to
 * <dest>\setup_result.txt and the exit code (0 installed). */
static int s_silent, s_silent_ok;
static wchar_t s_silent_msg[512];

static int px(int v) { return MulDiv(v, s_dpi, 96); }

static void status(const wchar_t *t) { SetWindowTextW(s_status, t); }

/* ── checks ───────────────────────────────────────────────────── */

static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

static int is_dir(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* The file's SHA-1 as 40 hex digits, or "" when it cannot be read. */
static void sha1_file(const wchar_t *path, char out[41])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    static BYTE buf[1 << 16];
    BYTE digest[20];
    DWORD got;
    int i;

    out[0] = 0;
    if (f == INVALID_HANDLE_VALUE)
        return;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0) == 0
            && BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        while (ReadFile(f, buf, sizeof buf, &got, NULL) && got)
            BCryptHashData(h, buf, got, 0);
        if (BCryptFinishHash(h, digest, 20, 0) == 0) {
            for (i = 0; i < 20; i++)
                sprintf(out + i * 2, "%02x", digest[i]);
        }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
}

/* NULL when the folder is the game, or why not. */
static const wchar_t *check_game(const wchar_t *dir)
{
    wchar_t p[MAX_PATH];
    char sum[41];

    _snwprintf(p, MAX_PATH, L"%s\\default.xbe", dir);
    if (!exists(p))
        return L"That folder has no default.xbe. Choose the folder of your unpacked game disc.";
    _snwprintf(p, MAX_PATH, L"%s\\data\\heroes", dir);
    if (!is_dir(p))
        return L"That folder has no data folder with the game's files. Choose the whole unpacked disc.";
    _snwprintf(p, MAX_PATH, L"%s\\default.xbe", dir);
    sha1_file(p, sum);
    if (strcmp(sum, XBE_SHA1) != 0)
        return L"This is not the release of the game the port is made for (default.xbe does not "
               L"match). The port needs the original NTSC Xbox release.";
    return NULL;
}

/* ── installing ───────────────────────────────────────────────── */

static ULONGLONG s_total, s_done;

static ULONGLONG tree_size(const wchar_t *dir)
{
    wchar_t pat[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    ULONGLONG n = 0;

    _snwprintf(pat, MAX_PATH, L"%s\\*", dir);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        _snwprintf(sub, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            n += tree_size(sub);
        else
            n += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static DWORD CALLBACK copy_progress(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER s1,
                                    LARGE_INTEGER s2, DWORD n, DWORD reason, HANDLE a, HANDLE b, LPVOID base)
{
    ULONGLONG at = *(ULONGLONG *)base + (ULONGLONG)done.QuadPart;
    static ULONGLONG last;
    (void)total; (void)s1; (void)s2; (void)n; (void)reason; (void)a; (void)b;
    if (!s_silent && (at - last > (8u << 20) || at < last)) {
        last = at;
        PostMessageW(s_wnd, WM_SETUP_PROGRESS, (WPARAM)(at >> 20), (LPARAM)(s_total >> 20));
    }
    return PROGRESS_CONTINUE;
}

static int copy_tree(const wchar_t *from, const wchar_t *to)
{
    wchar_t pat[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int ok = 1;

    CreateDirectoryW(to, NULL);
    _snwprintf(pat, MAX_PATH, L"%s\\*", from);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        _snwprintf(a, MAX_PATH, L"%s\\%s", from, fd.cFileName);
        _snwprintf(b, MAX_PATH, L"%s\\%s", to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok = copy_tree(a, b);
        } else {
            ULONGLONG size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            ok = CopyFileExW(a, b, copy_progress, &s_done, NULL, 0) != 0;
            s_done += size;
            SetFileAttributesW(b, FILE_ATTRIBUTE_NORMAL);   /* a disc copy is often read-only */
        }
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

static int write_resource(int id, const wchar_t *path)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
    const void *d = g ? LockResource(g) : NULL;
    DWORD n = r ? SizeofResource(NULL, r) : 0, put = 0;
    HANDLE f;
    if (!d || !n)
        return 0;
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    WriteFile(f, d, n, &put, NULL);
    CloseHandle(f);
    return put == n;
}

static int shortcut(const wchar_t *link, const wchar_t *target, const wchar_t *dir)
{
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    int ok = 0;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) {
        sl->lpVtbl->SetPath(sl, target);
        sl->lpVtbl->SetWorkingDirectory(sl, dir);
        sl->lpVtbl->SetDescription(sl, L"Spider-Man (2002) PC port");
        if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
            ok = SUCCEEDED(pf->lpVtbl->Save(pf, link, TRUE));
            pf->lpVtbl->Release(pf);
        }
        sl->lpVtbl->Release(sl);
    }
    return ok;
}

static void done(int ok, const wchar_t *msg)
{
    if (s_silent) {
        s_silent_ok = ok;
        _snwprintf(s_silent_msg, 511, L"%s", msg);
        return;
    }
    size_t n = wcslen(msg) + 1;
    wchar_t *m = (wchar_t *)malloc(n * sizeof *m);
    if (m)
        memcpy(m, msg, n * sizeof *m);
    PostMessageW(s_wnd, WM_SETUP_DONE, (WPARAM)ok, (LPARAM)m);
}

static DWORD WINAPI install_thread(LPVOID unused)
{
    wchar_t p[MAX_PATH], game[MAX_PATH], launcher[MAX_PATH], link[MAX_PATH], ini[MAX_PATH];
    PWSTR known = NULL;
    FILE *f;

    (void)unused;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    SHCreateDirectoryExW(NULL, s_dst_dir, NULL);
    if (!is_dir(s_dst_dir)) {
        done(0, L"Could not create the install folder. Choose another one.");
        return 0;
    }
    _snwprintf(p, MAX_PATH, L"%s\\spiderman_recomp.exe", s_dst_dir);
    _snwprintf(launcher, MAX_PATH, L"%s\\SpiderManLauncher.exe", s_dst_dir);
    if (!write_resource(RES_GAME_EXE, p) || !write_resource(RES_LAUNCHER_EXE, launcher)) {
        done(0, L"Could not write the game's files into the install folder. Is the game running?");
        return 0;
    }
    if (s_copy_game) {
        _snwprintf(game, MAX_PATH, L"%s\\game", s_dst_dir);
        s_total = tree_size(s_src_dir);
        s_done = 0;
        if (!copy_tree(s_src_dir, game)) {
            done(0, L"Copying your game failed part way. Check there is enough free space and try again.");
            return 0;
        }
    }
    /* settings.ini: the launcher fills in the rest the first time it saves. */
    _snwprintf(ini, MAX_PATH, L"%s\\settings.ini", s_dst_dir);
    if (!exists(ini) && (f = _wfopen(ini, L"w")) != NULL) {
        fwprintf(f, L"[Display]\nResolution=1920x1080\nAspectRatio=widescreen\nFullscreen=1\nVSync=1\n\n[Game]\n");
        if (!s_copy_game)
            fwprintf(f, L"DataDir=%s\n", s_src_dir);
        fclose(f);
    } else if (!s_copy_game) {
        WritePrivateProfileStringW(L"Game", L"DataDir", s_src_dir, ini);
    }
    if (!s_no_shortcuts && SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Desktop, 0, NULL, &known))) {
        _snwprintf(link, MAX_PATH, L"%s\\Spider-Man.lnk", known);
        shortcut(link, launcher, s_dst_dir);
        CoTaskMemFree(known);
    }
    if (!s_no_shortcuts && SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Programs, 0, NULL, &known))) {
        _snwprintf(link, MAX_PATH, L"%s\\Spider-Man.lnk", known);
        shortcut(link, launcher, s_dst_dir);
        CoTaskMemFree(known);
    }
    CoUninitialize();
    done(1, L"Installed. Start it from the Spider-Man shortcut on your desktop.");
    return 0;
}

/* ── the window ───────────────────────────────────────────────── */

static void browse(HWND edit, const wchar_t *title)
{
    IFileOpenDialog *d = NULL;
    IShellItem *it = NULL;
    PWSTR path = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return;
    d->lpVtbl->SetOptions(d, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->lpVtbl->SetTitle(d, title);
    if (SUCCEEDED(d->lpVtbl->Show(d, s_wnd)) && SUCCEEDED(d->lpVtbl->GetResult(d, &it))) {
        if (SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &path))) {
            SetWindowTextW(edit, path);
            CoTaskMemFree(path);
        }
        it->lpVtbl->Release(it);
    }
    d->lpVtbl->Release(d);
}

static HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, HFONT font)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h),
                             s_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static void start_install(void)
{
    const wchar_t *why;
    GetWindowTextW(s_src, s_src_dir, MAX_PATH);
    GetWindowTextW(s_dst, s_dst_dir, MAX_PATH);
    s_copy_game = SendMessageW(s_copy, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!s_src_dir[0] || !s_dst_dir[0]) {
        status(L"Choose where your game is and where to install.");
        return;
    }
    status(L"Checking your copy of the game...");
    UpdateWindow(s_status);
    if ((why = check_game(s_src_dir)) != NULL) {
        status(why);
        return;
    }
    EnableWindow(s_install, FALSE);
    EnableWindow(s_src, FALSE);
    EnableWindow(s_dst, FALSE);
    status(s_copy_game ? L"Installing: copying your game (about 2 GB)..." : L"Installing...");
    SendMessageW(s_progress, PBM_SETPOS, 0, 0);
    CloseHandle(CreateThread(NULL, 0, install_thread, NULL, 0, NULL));
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE: {
        wchar_t def[MAX_PATH];
        PWSTR local = NULL;
        s_wnd = w;
        add(L"STATIC", L"Spider-Man (2002) for PC", SS_LEFT, 20, 16, 520, 30, 0, s_title_font);
        add(L"STATIC", L"This installs the PC port from your own copy of the original Xbox game. "
                       L"No game files come with it.", SS_LEFT, 20, 50, 520, 36, 0, s_font);
        add(L"STATIC", L"Your game (the unpacked disc: default.xbe and the data folder)", SS_LEFT, 20, 98, 520, 18, 0, s_font);
        s_src = add(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 20, 118, 430, 24, ID_SRC, s_font);
        add(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 458, 117, 82, 26, ID_SRC_BROWSE, s_font);
        add(L"STATIC", L"Install to", SS_LEFT, 20, 156, 520, 18, 0, s_font);
        s_dst = add(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 20, 176, 430, 24, ID_DST, s_font);
        add(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 458, 175, 82, 26, ID_DST_BROWSE, s_font);
        s_copy = add(L"BUTTON", L"Copy the game into the install folder (about 2 GB; leave it off to use it where it is)",
                     BS_AUTOCHECKBOX | BS_MULTILINE, 20, 212, 520, 36, ID_COPY, s_font);
        SendMessageW(s_copy, BM_SETCHECK, BST_CHECKED, 0);
        s_progress = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE, px(20), px(258), px(520), px(18),
                                     w, (HMENU)ID_PROGRESS, GetModuleHandleW(NULL), NULL);
        SendMessageW(s_progress, PBM_SETRANGE32, 0, 100);
        s_status = add(L"STATIC", L"", SS_LEFT, 20, 284, 520, 40, ID_STATUS, s_font);
        s_install = add(L"BUTTON", L"Install", BS_DEFPUSHBUTTON, 366, 330, 82, 28, ID_INSTALL, s_font);
        add(L"BUTTON", L"Close", BS_PUSHBUTTON, 458, 330, 82, 28, ID_CLOSE, s_font);
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_UserProgramFiles, 0, NULL, &local))) {
            _snwprintf(def, MAX_PATH, L"%s\\Spider-Man", local);
            SetWindowTextW(s_dst, def);
            CoTaskMemFree(local);
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SRC_BROWSE: browse(s_src, L"Choose your unpacked game disc"); break;
        case ID_DST_BROWSE: browse(s_dst, L"Choose where to install"); break;
        case ID_INSTALL:    start_install(); break;
        case ID_CLOSE:      DestroyWindow(w); break;
        }
        return 0;
    case WM_SETUP_PROGRESS:
        if (lp)
            SendMessageW(s_progress, PBM_SETPOS, (WPARAM)(wp * 100 / lp), 0);
        return 0;
    case WM_SETUP_DONE: {
        wchar_t *msg = (wchar_t *)lp;
        SendMessageW(s_progress, PBM_SETPOS, wp ? 100 : 0, 0);
        status(msg ? msg : L"");
        free(msg);
        if (wp) {
            SetWindowTextW(s_install, L"Done");
        } else {
            EnableWindow(s_install, TRUE);
            EnableWindow(s_src, TRUE);
            EnableWindow(s_dst, TRUE);
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSW wc;
    MSG msg;
    RECT r;
    HDC dc;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };

    (void)prev;
    s_no_shortcuts = cmd && wcsstr(cmd, L"/noshortcuts") != NULL;
    if (cmd && wcsstr(cmd, L"/silent")) {
        int argc, i;
        wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc), res[MAX_PATH];
        const wchar_t *why;
        FILE *f;
        s_silent = 1;
        s_copy_game = 1;
        for (i = 1; argv && i < argc; i++) {
            if (!wcsncmp(argv[i], L"/game=", 6)) wcsncpy(s_src_dir, argv[i] + 6, MAX_PATH - 1);
            else if (!wcsncmp(argv[i], L"/dest=", 6)) wcsncpy(s_dst_dir, argv[i] + 6, MAX_PATH - 1);
            else if (!wcscmp(argv[i], L"/nocopy")) s_copy_game = 0;
        }
        if (!s_src_dir[0] || !s_dst_dir[0])
            return 2;
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if ((why = check_game(s_src_dir)) != NULL)
            done(0, why);
        else
            install_thread(NULL);
        SHCreateDirectoryExW(NULL, s_dst_dir, NULL);
        _snwprintf(res, MAX_PATH, L"%s\\setup_result.txt", s_dst_dir);
        if ((f = _wfopen(res, L"w")) != NULL) {
            fwprintf(f, L"%s\n%s\n", s_silent_ok ? L"ok" : L"failed", s_silent_msg);
            fclose(f);
        }
        return s_silent_ok ? 0 : 1;
    }
    SetProcessDPIAware();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    InitCommonControlsEx(&icc);
    dc = GetDC(NULL);
    s_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    s_font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    s_title_font = CreateFontW(-px(22), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"SpiderManSetup";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassW(&wc);
    r.left = 0; r.top = 0; r.right = px(560); r.bottom = px(372);
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    CreateWindowW(L"SpiderManSetup", L"Spider-Man (2002) PC port - Setup",
                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                  r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    ShowWindow(s_wnd, show);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(s_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return 0;
}
