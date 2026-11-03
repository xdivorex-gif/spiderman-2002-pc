/*
 * shocker_powers.c -- what the Shocker's moves do beyond what their data can.
 *
 * Both go through the engine's radius damage (sub_0012BBE0, under the script
 * native apply_radius_damage: world [0x4C06B8] as this; position, radius,
 * damage, hit-the-hero): each AI in range takes a blast (sub_002B50B4 makes
 * it, type 6), which throws it along a direction, normally from the centre
 * out. While these calls run that direction is replaced
 * (shocker_powers_damage_record):
 *
 *   His tornado (the web dome, L2 + Triangle, src/shocker_input.c) draws the
 *   enemies round him in like a magnet and hurts them while it lasts: every
 *   TICK_MS a blast round him, its direction turned round, so they are
 *   thrown in.
 *
 *   His double-arm blast (L2 + Square) knocks down what it hits, backwards:
 *   when the shot (guided at what is ahead) has reached it, blasts at points
 *   along his facing, thrown along his facing -- away from him. A shot has
 *   no knock-down of its own: thrown items cannot make an explosive hit.
 *
 * It runs on the game thread, from the script engine's entry
 * (shocker_powers_frame: many times a frame), and only while the Shocker is
 * worn and a level has a hero.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern ptrdiff_t g_xbox_mem_offset;
extern __declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
extern __declspec(thread) int g_fp_top;

void sub_0012BBE0(void);            /* world: radius damage */
void sub_000ED460(void);            /* app: load_level(level, hero, n) */
void sub_000ED320(void);            /* app: leave the pause (pause_game(0)) */
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
#define TORNADO_DAMAGE   15.0f      /* a tick: 6 went unnoticed */

#define BLAST_HIT_MS     250        /* the shot (60 m/s) has reached what is ahead */
#define BLAST_DAMAGE     10.0f      /* each point; the shot does its own 60 */
static const float s_blast_at_m[][2] = { { 3, 3 }, { 9, 4 }, { 16, 4 } };   /* ahead, radius */

enum { DIR_KEEP, DIR_IN, DIR_ALONG };
static volatile DWORD s_tornado_at, s_blast_at;
static int s_dir_mode, s_busy;
static uint32_t s_along[3];         /* DIR_ALONG: the direction, float bits */
static int s_caught;                /* blasts made in this file's calls */

/* RECOMP_GAME_LOG: the double-arm blast's geometry and what it caught. */
static int log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GAME_LOG");
        on = e && *e && *e != '0';
    }
    return on;
}

void shocker_powers_tornado(void) { s_tornado_at = GetTickCount(); }
void shocker_powers_blast(void)   { s_blast_at = GetTickCount(); }

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static float gfloat(uint32_t va)
{
    uint32_t u = GUEST32(va);
    float f;
    memcpy(&f, &u, 4);
    return f;
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

/* The hero's position (his world matrix's last row, sub_0015B8E0), 0 if
 * none. The row before it is the way he faces. */
static uint32_t hero_pos(uint32_t hero)
{
    uint32_t pos = guest_call(sub_0015B8E0, hero, NULL, 0);
    return pos && pos < 0x04000000 ? pos : 0;
}

static void radius_damage(uint32_t world, float x, float y, float z, float radius,
                          float damage, int mode)
{
    uint32_t args[6] = { fbits(x), fbits(y), fbits(z), fbits(radius), fbits(damage), 0 };
    s_dir_mode = mode;
    guest_call(sub_0012BBE0, world, args, 6);
    s_dir_mode = DIR_KEEP;
}

static void tornado_tick(uint32_t world, uint32_t hero)
{
    uint32_t pos = hero_pos(hero);
    if (pos)
        radius_damage(world, gfloat(pos), gfloat(pos + 4), gfloat(pos + 8),
                      TORNADO_RADIUS, TORNADO_DAMAGE, DIR_IN);
}

static void blast_hit(uint32_t world, uint32_t hero)
{
    uint32_t pos = hero_pos(hero), fwd;
    float x, y, z, fx, fz, len;
    int k;

    if (!pos)
        return;
    fwd = pos - 0x10;
    fx = gfloat(fwd);
    fz = gfloat(fwd + 8);
    len = (float)sqrt(fx * fx + fz * fz);
    if (len < 0.01f)
        return;
    fx /= len;
    fz /= len;
    x = gfloat(pos);
    y = gfloat(pos + 4);
    z = gfloat(pos + 8);
    s_along[0] = fbits(fx);
    s_along[1] = fbits(0.0f);
    s_along[2] = fbits(fz);
    if (log_on())
        fprintf(stderr, "[SHOCKER] blast: pos %.2f %.2f %.2f  row0 %.2f %.2f %.2f  row1 %.2f %.2f %.2f"
                "  row2 %.2f %.2f %.2f\n", x, y, z,
                gfloat(pos - 0x30), gfloat(pos - 0x2C), gfloat(pos - 0x28),
                gfloat(pos - 0x20), gfloat(pos - 0x1C), gfloat(pos - 0x18),
                gfloat(pos - 0x10), gfloat(pos - 0x0C), gfloat(pos - 0x08));
    for (k = 0; k < (int)(sizeof s_blast_at_m / sizeof s_blast_at_m[0]); k++) {
        s_caught = 0;
        radius_damage(world, x + fx * s_blast_at_m[k][0], y + 1.0f, z + fz * s_blast_at_m[k][0],
                      s_blast_at_m[k][1], BLAST_DAMAGE, DIR_ALONG);
        if (log_on())
            fprintf(stderr, "[SHOCKER]   %.0f m ahead: %d caught\n", s_blast_at_m[k][0], s_caught);
    }
}

/* Restart the level now, as the script call load_level(level, hero, n) would
 * with what the running level was loaded with: sub_000ED460 on the app
 * ([0x4BC614]) keeps the two names (0x4BC948, 0x4BC848; it reads the char *
 * at +4 of each string object) and n (app+0x114), and flags the main loop to
 * reload. Run from the script engine's entry, where a script's call would be. */
static volatile LONG s_restart_wanted;

void level_restart_request(void)
{
    InterlockedExchange(&s_restart_wanted, 1);
}

static void level_load_names(const char *level, const char *hero, int n);

static void level_restart_run(void)
{
    uint32_t app = GUEST32(0x4BC614), i;
    char level[256], hero[256];

    if (!app)
        return;
    for (i = 0; i < 256; i++) {          /* the callee copies onto the originals */
        level[i] = ((const char *)g_xbox_mem_offset + 0x4BC948)[i];
        hero[i] = ((const char *)g_xbox_mem_offset + 0x4BC848)[i];
    }
    level[255] = hero[255] = 0;
    level_load_names(level, hero, (int)GUEST32(app + 0x114));
}

static void level_load_names(const char *level, const char *hero, int n)
{
    uint32_t app = GUEST32(0x4BC614), esp = g_esp, base, args[3];

    if (!app)
        return;
    g_esp -= 0x400;
    base = g_esp;
    strcpy((char *)g_xbox_mem_offset + base + 0x20, level);
    strcpy((char *)g_xbox_mem_offset + base + 0x120, hero);
    GUEST32(base + 4) = base + 0x20;
    GUEST32(base + 0x14) = base + 0x120;
    args[0] = base;
    args[1] = base + 0x10;
    args[2] = (uint32_t)n;
    fprintf(stderr, "  [COSTUME] load_level(\"%s\", \"%s\", %d)\n",
            (const char *)g_xbox_mem_offset + base + 0x20,
            (const char *)g_xbox_mem_offset + base + 0x120,
            (int)args[2]);
    guest_call(sub_000ED320, app, NULL, 0);     /* a paused game never gets to the reload */
    guest_call(sub_000ED460, app, args, 3);
    g_esp = esp;
}

/* The script engine's entry (sub_00051C00_enter, recomp_manual.c): the
 * game thread, many times a frame. */
void shocker_powers_frame(void)
{
    static DWORD last_tick, blast_done;
    DWORD t = GetTickCount();
    uint32_t world, hero;
    int tornado, blast;
    extern void movement_frame(void);

    movement_frame();               /* movement.c: the run threshold */

    /* RECOMP_TEST_LEVEL=<seconds>,<level>,<hero>,<n>: load that level once,
     * that long after start, for measuring one scene the same way each run. */
    {
        static int test_done;
        static DWORD t0;
        const char *e;
        if (!t0)
            t0 = t;
        if (!test_done && t - t0 > 3000 && (e = getenv("RECOMP_TEST_LEVEL")) != NULL) {
            char level[128], hero[128];
            int secs, n;
            if (sscanf(e, "%d,%127[^,],%127[^,],%d", &secs, level, hero, &n) != 4) {
                test_done = 1;
            } else if (t - t0 > (DWORD)secs * 1000 && !s_busy) {
                test_done = 1;
                s_busy = 1;
                level_load_names(level, hero, n);
                s_busy = 0;
            }
        }
    }
    if (s_restart_wanted && !s_busy) {
        InterlockedExchange(&s_restart_wanted, 0);
        s_busy = 1;
        level_restart_run();
        s_busy = 0;
    }
    if (s_busy || (!s_tornado_at && !s_blast_at))
        return;
    tornado = s_tornado_at && t - last_tick >= TICK_MS
              && t - s_tornado_at >= TORNADO_DELAY_MS
              && t - s_tornado_at <= TORNADO_DELAY_MS + TORNADO_MS;
    blast = s_blast_at && blast_done != s_blast_at && t - s_blast_at >= BLAST_HIT_MS;
    if (!tornado && !blast)
        return;
    if (costume_worn_index() != SHOCKER_INDEX || !(hero = hero_entity(&world)))
        return;
    s_busy = 1;                     /* the damage may run scripts: no re-entry */
    if (tornado) {
        last_tick = t;
        tornado_tick(world, hero);
    }
    if (blast) {
        blast_done = s_blast_at;
        if (t - s_blast_at < BLAST_HIT_MS + 500)     /* not a stale one */
            blast_hit(world, hero);
    }
    s_busy = 0;
}

/* sub_002B50B4 (sub_002B50B4_enter, recomp_manual.c; cdecl) is called all
 * over the game, thousands of times a second; the radius damage makes its
 * blast with it as (6, centre, direction, 0, 0x3F8570, 0x4C0714). Only
 * those, and only in this file's own calls, have the direction replaced.
 * (Logging every call for a while after the blast cost half the frame
 * rate.) */
void shocker_powers_damage_record(void)
{
    uint32_t dir;
    int k;

    if (s_dir_mode == DIR_KEEP || GUEST32(g_esp + 4) != 6
        || GUEST32(g_esp + 24) != 0x004C0714u)
        return;
    dir = GUEST32(g_esp + 12);
    if (!dir || dir >= 0x04000000)
        return;
    s_caught++;
    for (k = 0; k < 3; k++) {
        if (s_dir_mode == DIR_IN)
            GUEST32(dir + 4 * k) ^= 0x80000000u;
        else
            GUEST32(dir + 4 * k) = s_along[k];
    }
}
