/*
 * The Shocker's gauntlets heat up. Each special adds heat; it bleeds off over
 * time. At full heat they overheat: no specials until they have cooled all
 * the way down. gpu_backend.c draws the heat in the slot of the health bar
 * that showed web fluid (his gauntlet, tools/shocker_hud.ps1, is drawn round
 * it), so heat is his limit, not webbing.
 *
 * His specials are Spider-Man's web specials re-skinned (shocker_input.c),
 * and those spend web fluid; with heat as the limit the fluid must not run
 * out under it. While he is worn the game's own Unlimited Webbing cheat
 * (options + 0x1554, read by sub_000EA520) is set, and put back as it was
 * when he is not.
 *
 * Heat is a value 0..1 kept with its time: read on the render thread,
 * changed on the USB thread (shocker_input.c), so the cooling is worked out
 * from the clock on each read rather than ticked.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern ptrdiff_t g_xbox_mem_offset;
#define GUEST8(va)  (*(volatile uint8_t *)((uint8_t *)g_xbox_mem_offset + (va)))
#define GUEST32(va) (*(volatile uint32_t *)((uint8_t *)g_xbox_mem_offset + (va)))

int costume_worn_index(void);
#define SHOCKER_INDEX   8

#define COOL_PER_S      0.20        /* heat lost a second: full to cold in 5 s */
#define VENT_PER_S      0.25        /* overheated: cold again in 4 s */
#define COOL_AFTER_MS   1200        /* no cooling this soon after a special, so
                                       firing as fast as he can overheats him */
#define OPT_UNLIMITED_WEB 0x1554u

static CRITICAL_SECTION s_lock;
static volatile LONG s_init;
static double s_heat;               /* as of s_at */
static ULONGLONG s_at, s_fired;    /* s_heat's time; the last special's */
static int s_over;                  /* overheated, venting to 0 */

static void lock(void)
{
    if (InterlockedCompareExchange(&s_init, 1, 0) == 0) {
        InitializeCriticalSection(&s_lock);
        InterlockedExchange(&s_init, 2);
    }
    while (s_init != 2)
        Sleep(0);
    EnterCriticalSection(&s_lock);
}

/* Bring s_heat up to now. */
static void settle(ULONGLONG now)
{
    ULONGLONG from = s_at;
    double dt;
    if (!s_over && s_fired + COOL_AFTER_MS > from)
        from = s_fired + COOL_AFTER_MS;
    dt = s_at && now > from ? (double)(now - from) / 1000.0 : 0.0;
    s_heat -= dt * (s_over ? VENT_PER_S : COOL_PER_S);
    if (s_heat <= 0.0) {
        s_heat = 0.0;
        s_over = 0;
    }
    s_at = now;
}

/* Overheated: shocker_input.c keeps his specials' buttons from the game. */
int shocker_heat_blocked(void)
{
    int over;
    lock();
    settle(GetTickCount64());
    over = s_over;
    LeaveCriticalSection(&s_lock);
    return over;
}

static void add(double cost)
{
    lock();
    settle(GetTickCount64());
    if (!s_over) {
        s_fired = s_at;
        s_heat += cost;
        if (s_heat >= 1.0) {        /* the one that gets there still fired */
            s_heat = 1.0;
            s_over = 1;
        }
    }
    LeaveCriticalSection(&s_lock);
}

/* A sound the game is starting (recomp_manual.c, sub_002B3D00_enter): heat
 * comes from a special's own sound, so a press the game turns down (in the
 * air, mid-move) costs nothing. The hook sees a sound again on each of its
 * updates, every frame while it plays, and back-to-back blasts overlap, so
 * a new one is told by its instance slot: a slot not heard with that sound
 * for 150 ms is a new play. */
void shocker_heat_sound(const char *name, uint32_t slot)
{
    static struct { uint32_t slot; int i; ULONGLONG at; } live[16];
    static const struct { const char *name; double heat; } specials[] = {
        { "WEBLAUNCH", 0.30 },      /* impact web: his double-arm blast */
        { "WEBDOME",   0.45 },      /* web dome: his tornado */
        { "WEBGLOVES", 0.25 },      /* web gloves: his charged gauntlets */
        { "WEBYANK",   0.22 },      /* web yank: his electric yank (name unseen) */
    };
    ULONGLONG now;
    int i, k, old = 0;

    for (i = 0; i < 4; i++)
        if (!_stricmp(name, specials[i].name))
            break;
    if (i == 4 || costume_worn_index() != SHOCKER_INDEX)
        return;
    now = GetTickCount64();
    for (k = 0; k < 16; k++) {
        if (live[k].slot == slot && live[k].i == i && now - live[k].at < 150) {
            live[k].at = now;           /* the same play, updated */
            return;
        }
        if (live[k].at < live[old].at)
            old = k;
    }
    live[old].slot = slot;
    live[old].i = i;
    live[old].at = now;
    add(specials[i].heat);
    {
        static int log = -1;
        if (log < 0)
            log = getenv("RECOMP_SUBS_LOG") != NULL;
        if (log)
            fprintf(stderr, "  [HEAT] %s (slot %08X) +%.2f\n", name, slot, specials[i].heat);
    }
}

/* Heat now, 0..1; *over whether he is overheated. */
float shocker_heat(int *over)
{
    float h;
    lock();
    settle(GetTickCount64());
    h = (float)s_heat;
    if (over)
        *over = s_over;
    LeaveCriticalSection(&s_lock);
    return h;
}

/* The game thread (shocker_powers_frame): the webbing cheat with the suit. */
void shocker_heat_frame(void)
{
    static int set, was;
    uint32_t app = GUEST32(0x4BC614u), opt;
    int shocker;

    if (app < 0x10000u || app > 0x03FFFF00u)
        return;
    opt = GUEST32(app + 0x11C);
    if (opt < 0x10000u || opt > 0x03FFFF00u - OPT_UNLIMITED_WEB)
        return;
    shocker = costume_worn_index() == SHOCKER_INDEX;
    if (shocker && !set) {
        was = GUEST8(opt + OPT_UNLIMITED_WEB);
        set = 1;
    }
    if (shocker)
        GUEST8(opt + OPT_UNLIMITED_WEB) = 1;
    else if (set) {
        GUEST8(opt + OPT_UNLIMITED_WEB) = (uint8_t)was;
        set = 0;
    }
}
