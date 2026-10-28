/*
 * shocker_powers.c -- what the Shocker's moves do beyond what their data can.
 *
 * His tornado (the web dome, L2 + Triangle, src/shocker_input.c) draws the
 * enemies round him in like a magnet and hurts them while it lasts. Every
 * TICK_MS of it this calls the engine's radius damage round him
 * (sub_0012BBE0, under the script native apply_radius_damage: world
 * [0x4C06B8] as this; position, radius, damage, hit-the-hero): each AI in
 * range takes a blast (damage record type 6, sub_002B50B4), which throws
 * it along a direction from the centre out. While these calls run that
 * direction is turned round (sub_002B50B4_enter), so they are thrown in.
 *
 * It runs on the game thread, from the script engine's entry
 * (shocker_powers_frame: many times a frame), at most every TICK_MS, and only
 * while the Shocker is worn and a level has a hero.
 *
 * RECOMP_GAME_LOG also logs the damage records made just after his
 * double-arm blast (L2 + Square), to see which kind its hit is.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;
extern __declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
extern __declspec(thread) int g_fp_top;

void sub_0012BBE0(void);            /* world: radius damage */
void sub_0015B8E0(void);            /* entity: its position (a vector3d *) */
int costume_worn_index(void);

#define GUEST32(va) (*(volatile uint32_t *)((uint8_t *)g_xbox_mem_offset + (va)))

#define SM_WORLD_PTR   0x004C06B8u  /* -> the world */
#define SM_WORLD_HERO  0x1E4u       /* world: the hero entity */
#define SHOCKER_INDEX  8

#define TORNADO_DELAY_MS 600        /* his launch, before the spin */
#define TORNADO_MS       4000       /* the dome's own time */
#define TICK_MS          400
#define TORNADO_RADIUS   8.0f
#define TORNADO_DAMAGE   6.0f

static volatile DWORD s_tornado_at, s_blast_at;
static int s_pulling, s_busy;

void shocker_powers_tornado(void) { s_tornado_at = GetTickCount(); }
void shocker_powers_blast(void)   { s_blast_at = GetTickCount(); }

static int game_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GAME_LOG");
        on = e && *e && *e != '0';
    }
    return on;
}

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* Call a recompiled function as the game would: `args` pushed (the first
 * nearest the return address), `ecx` as this. Registers and the stack are
 * as they were afterwards, whatever the callee pops; returns eax. */
static uint32_t guest_call(void (*fn)(void), uint32_t ecx, const uint32_t *args, int n)
{
    uint32_t eax = g_eax, edx = g_edx, ebx = g_ebx, esi = g_esi, edi = g_edi;
    uint32_t esp = g_esp, ecx0 = g_ecx, r;
    int fp = g_fp_top, i;

    g_esp -= 0x40;                  /* clear of anything the caller keeps below */
    for (i = n - 1; i >= 0; i--) {
        g_esp -= 4;
        GUEST32(g_esp) = args[i];
    }
    g_esp -= 4;
    GUEST32(g_esp) = 0;             /* return address: nothing reads it */
    g_ecx = ecx;
    fn();
    r = g_eax;
    g_eax = eax; g_ecx = ecx0; g_edx = edx; g_ebx = ebx; g_esi = esi; g_edi = edi;
    g_esp = esp;
    g_fp_top = fp;
    return r;
}

static uint32_t hero_entity(uint32_t *world)
{
    uint32_t w = GUEST32(SM_WORLD_PTR), h;
    if (!w || w >= 0x04000000)
        return 0;
    h = GUEST32(w + SM_WORLD_HERO);
    if (!h || h >= 0x04000000)
        return 0;
    *world = w;
    return h;
}

static void tornado_tick(uint32_t world, uint32_t hero)
{
    uint32_t pos = guest_call(sub_0015B8E0, hero, NULL, 0), args[6];
    if (!pos || pos >= 0x04000000)
        return;
    args[0] = GUEST32(pos);
    args[1] = GUEST32(pos + 4);
    args[2] = GUEST32(pos + 8);
    args[3] = fbits(TORNADO_RADIUS);
    args[4] = fbits(TORNADO_DAMAGE);
    args[5] = 0;                    /* not the hero */
    s_pulling = 1;
    guest_call(sub_0012BBE0, world, args, 6);
    s_pulling = 0;
}

/* The script engine's entry (sub_00051C00_enter, recomp_manual.c): the
 * game thread, many times a frame. */
void shocker_powers_frame(void)
{
    static DWORD last;
    DWORD t = GetTickCount(), since;
    uint32_t world, hero;

    if (s_busy || t - last < TICK_MS || !s_tornado_at)
        return;
    since = t - s_tornado_at;
    if (since < TORNADO_DELAY_MS || since > TORNADO_DELAY_MS + TORNADO_MS)
        return;
    if (costume_worn_index() != SHOCKER_INDEX || !(hero = hero_entity(&world)))
        return;
    last = t;
    s_busy = 1;                     /* the damage may run scripts: no re-entry */
    tornado_tick(world, hero);
    s_busy = 0;
}

/* A damage record being made (sub_002B50B4_enter, recomp_manual.c; cdecl:
 * type, centre, direction, ...). In the tornado's calls the direction is
 * turned round: thrown in, not out. */
void shocker_powers_damage_record(void)
{
    uint32_t type = GUEST32(g_esp + 4), dir = GUEST32(g_esp + 12);

    if (s_pulling && dir && dir < 0x04000000) {
        int k;
        for (k = 0; k < 3; k++)
            GUEST32(dir + 4 * k) ^= 0x80000000u;
    }
    if (game_log_on() && s_blast_at && GetTickCount() - s_blast_at < 2000 && !s_pulling)
        fprintf(stderr, "[SHOCKER] damage record type %u after the blast (%lu ms)\n",
                type, GetTickCount() - s_blast_at);
}
