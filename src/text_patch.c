/*
 * On-screen text that names the Xbox, reworded for the PC -- in guest memory.
 *
 * Most of the title's text is in data\locales\<language>.lng, which the
 * overlay folder replaces (tools\make_overlay.py). Some is not: "Saving game.
 * Please don't turn off your Xbox console." is in the menu container
 * (bonus\MENU.xbs), which is compressed, so there is no file to edit. Once
 * the title has unpacked it the string sits in guest RAM as plain text, so a
 * low-priority thread looks for it there every couple of seconds and rewrites
 * it in place. Each replacement is no longer than what it replaces, so it
 * always fits: the rest of the string moves up and its terminator with it.
 *
 * ASCII and UTF-16 are both searched. One pass over 64 MB is ~10 ms on one
 * core. The boot legal screen (bonus\LEGAL.xbs, compressed too) has its
 * Microsoft/Xbox trademark paragraph swapped the same way. RECOMP_TEXT_PATCH=0
 * turns it off.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void *xbox_GetMemoryBase(void);

#define GUEST_RAM (64u << 20)

#define MS_LF(sep) \
    "Microsoft, Xbox, and the Xbox logo are either registered " sep \
    "trademarks or trademarks of Microsoft Corporation in the " sep \
    "U.S. and/or other countries and are used under license " sep \
    "from Microsoft."
#define PC_LF(sep) \
    "This PC version is an unofficial fan recompilation and " sep \
    "is not affiliated with or endorsed by Activision, " sep \
    "Treyarch, Marvel, Columbia Pictures or Microsoft."

static const struct { const char *find, *with; } s_patches[] = {
    { "your Xbox console", "your PC" },
    { "Xbox console", "PC" },
    /* The legal screen at boot (bonus\LEGAL.xbs; legalbox.txt is the same
     * text, unpacked): its Microsoft trademark paragraph. */
    { MS_LF("\n"), PC_LF("\n") },
    { MS_LF("\r\n"), PC_LF("\r\n") },
};

/* Replace `find` (n bytes, `unit` bytes a character) at p inside a string
 * that runs to its terminator, by `with`, shifting the tail up. */
static void splice(uint8_t *p, const uint8_t *end, size_t n, const char *with, int unit)
{
    size_t wn = strlen(with), i, tail = 0;
    uint8_t *after = p + n;

    /* The tail: up to and including the terminator (bounded). */
    while (after + tail + unit <= end && tail < 4096) {
        int zero = after[tail] == 0 && (unit == 1 || after[tail + 1] == 0);
        tail += unit;
        if (zero)
            break;
    }
    for (i = 0; i < wn; i++) {
        p[i * unit] = (uint8_t)with[i];
        if (unit == 2)
            p[i * unit + 1] = 0;
    }
    memmove(p + wn * unit, after, tail);
    /* Clear what the shorter string left behind. */
    memset(p + wn * unit + tail, 0, n - wn * unit);
}

static int patch_pass(uint8_t *ram)
{
    const uint8_t *end = ram + GUEST_RAM;
    int done = 0;
    size_t k;

    for (k = 0; k < sizeof s_patches / sizeof s_patches[0]; k++) {
        const char *f = s_patches[k].find;
        size_t n = strlen(f);
        uint8_t wide[64];
        size_t i;
        uint8_t *p;

        /* ASCII */
        for (p = ram; p + n <= end; p++) {
            if (*p != (uint8_t)f[0] || memcmp(p, f, n) != 0)
                continue;
            splice(p, end, n, s_patches[k].with, 1);
            done++;
        }
        /* UTF-16LE */
        if (2 * n > sizeof wide)
            continue;
        for (i = 0; i < n; i++) { wide[2 * i] = (uint8_t)f[i]; wide[2 * i + 1] = 0; }
        for (p = ram; p + 2 * n <= end; p += 2) {
            if (*p != wide[0] || memcmp(p, wide, 2 * n) != 0)
                continue;
            splice(p, end, 2 * n, s_patches[k].with, 2);
            done++;
        }
    }
    return done;
}

static DWORD WINAPI text_patch_main(LPVOID unused)
{
    uint8_t *ram = (uint8_t *)xbox_GetMemoryBase();
    (void)unused;
    if (!ram)
        return 0;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    for (int pass = 0;; pass++) {
        int n;
        /* Often at first, to catch the legal screen before it is up, then
         * every couple of seconds. */
        Sleep(pass < 100 ? 300 : 2000);
        /* All 64 MB are mapped from the start (one section, xbox_memory_layout),
         * as the APU's walk over the same RAM relies on. */
        n = patch_pass(ram);
        if (n)
            fprintf(stderr, "[TEXT] reworded %d Xbox string(s) for the PC\n", n);
    }
}

void text_patch_start(void)
{
    const char *e = getenv("RECOMP_TEXT_PATCH");
    if (e && *e == '0')
        return;
    CloseHandle(CreateThread(NULL, 0, text_patch_main, NULL, 0, NULL));
}
