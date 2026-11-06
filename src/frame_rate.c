/*
 * frame_rate.c -- the game's frame cap (settings.ini [Display] FrameRate).
 *
 * In play the game is variable-step: its frame (sub_000F6FF0) spins until a
 * minimum frame time of TSC time has passed -- the float at 0x3DDD04, 1/60 s
 * -- then hands the elapsed time (clamped to 0.1 s) to the simulation. Its
 * menus are not: the level select stepped per frame and ran too fast
 * unlocked. So the cap applies only in play (a real hero in the world, not
 * the front end's "fakehero", and not paused); the menus keep the console's
 * pacing.
 *
 * In play: flips are not held to the emulated 59.94 Hz vblank (the toolkit's
 * flip pacing, g_nv2a_flip_pacing_override, nv2a_pb_exec.c) and the minimum
 * frame time is 1/N (unlocked: 1 ms, so the game never waits). Elsewhere: the
 * variable's pacing and 1/60. Checked at the top of every frame
 * (sub_000F6FF0_enter).
 *
 * RECOMP_FRAME_RATE (settings.c): 0 = unlocked, N = cap at N, unset = 60.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;
extern volatile int g_nv2a_flip_pacing_override;   /* toolkit, nv2a_pb_exec.c */

#define GUEST8(va)    (*(volatile uint8_t *)((uint8_t *)g_xbox_mem_offset + (va)))
#define GUEST32(va)   (*(volatile uint32_t *)((uint8_t *)g_xbox_mem_offset + (va)))
#define GUESTF(va)    (*(volatile float *)((uint8_t *)g_xbox_mem_offset + (va)))
#define OK(p)         ((p) >= 0x10000u && (p) < 0x04000000u)

#define MIN_FRAME_VA   0x003DDD04u  /* the frame limiter's 1/60 s */
#define SM_WORLD_PTR   0x004C06B8u  /* -> the world */
#define SM_WORLD_HERO  0x1E4u       /* world: the hero entity */
#define SM_APP_PTR     0x004BC614u  /* -> the app object (costume_panel.c) */
#define SM_APP_PROFILE 0x11Cu
#define SM_APP_PAUSED  0x18Bu
#define SM_PROFILE_HERO 0x14E8u

static int in_play(void)
{
    uint32_t w = GUEST32(SM_WORLD_PTR), app = GUEST32(SM_APP_PTR), prof;

    if (!OK(w) || !OK(GUEST32(w + SM_WORLD_HERO)) || !OK(app) || GUEST8(app + SM_APP_PAUSED))
        return 0;
    prof = GUEST32(app + SM_APP_PROFILE);
    if (OK(prof) && !_strnicmp((const char *)g_xbox_mem_offset + prof + SM_PROFILE_HERO,
                               "fakehero", 8))
        return 0;
    return 1;
}

void frame_rate_frame(void)
{
    static int hz = -2, was = -1;
    int play;
    float limit;

    if (hz == -2) {
        const char *e = getenv("RECOMP_FRAME_RATE");
        hz = e && *e ? atoi(e) : 60;
    }
    if (hz == 60)
        return;                             /* the game's own */
    play = in_play();
    if (play != was) {
        was = play;
        g_nv2a_flip_pacing_override = play ? 0 : -1;
        fprintf(stderr, "  [FPS] %s\n", play ? (hz <= 0 ? "in play: unlocked" : "in play: capped")
                                             : "menus / paused: 60");
    }
    limit = !play ? 1.0f / 60.0f : hz <= 0 || hz >= 1000 ? 0.001f : 1.0f / (float)hz;
    if (GUESTF(MIN_FRAME_VA) != limit)
        GUESTF(MIN_FRAME_VA) = limit;
}
