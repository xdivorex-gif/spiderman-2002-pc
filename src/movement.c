/*
 * Movement feel: two changes on the way the game reads the left stick.
 *
 * Running. The hero's controller (sub_002608C0) walks him until the stick's
 * throw reaches the profile's RUN_SENSITIVITY (options + 0x14B4, 0.7 by
 * default), then runs him until it falls under half that. So most of the
 * stick is a slow walk and the run comes in late, with a jump. RunSensitivity
 * in settings.ini [Game] (0.45 by default here) is written over it each frame.
 *
 * The dead zone. The launcher's dead zone (usb_gamepad's radial one, 15%)
 * already rescales what is left to the full range, and the game then drops
 * the stick while both axes are under 0.1 on top. A push just past the first
 * did nothing until it cleared the second. Here a stick that is moving starts
 * at 0.15 of full throw, so the game sees it at once and the rest of the
 * throw stays linear.
 */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

extern ptrdiff_t g_xbox_mem_offset;
#define GUEST32(va) (*(volatile uint32_t *)((uint8_t *)g_xbox_mem_offset + (va)))

#define APP_PTR       0x4BC614u     /* the app; +0x11C the options */
#define OPT_RUN_SENS  0x14B4u
#define STICK_FLOOR   0.15

static float s_run_sens = 0.45f;

void movement_init(void)
{
    const char *ini = getenv("RECOMP_INPUT_INI");
    char v[32];

    if (ini && *ini) {
        GetPrivateProfileStringA("Game", "RunSensitivity", "0.45", v, sizeof v, ini);
        s_run_sens = (float)atof(v);
    }
    if (s_run_sens < 0.1f || s_run_sens > 1.0f)
        s_run_sens = 0.45f;
}

/* The game thread, many times a frame (shocker_powers_frame). */
void movement_frame(void)
{
    uint32_t app = GUEST32(APP_PTR), opt;
    union { float f; uint32_t u; } v;

    if (app < 0x10000u || app > 0x03FFFF00u)
        return;
    opt = GUEST32(app + 0x11C);
    if (opt < 0x10000u || opt > 0x03FFFF00u - OPT_RUN_SENS)
        return;
    v.f = s_run_sens;
    GUEST32(opt + OPT_RUN_SENS) = v.u;
}

/* A pad report in play (costume_panel.c's filter): bytes 12..15 the left stick. */
void movement_stick(uint8_t *r)
{
    int16_t x = (int16_t)(r[12] | r[13] << 8), y = (int16_t)(r[14] | r[15] << 8);
    double mag = sqrt((double)x * x + (double)y * y) / 32767.0, k;
    long ox, oy;

    if (mag <= 0.0)
        return;
    if (mag > 1.0)
        mag = 1.0;
    k = (STICK_FLOOR + (1.0 - STICK_FLOOR) * mag) / mag;
    ox = lround(x * k);
    oy = lround(y * k);
    ox = ox > 32767 ? 32767 : ox < -32767 ? -32767 : ox;
    oy = oy > 32767 ? 32767 : oy < -32767 ? -32767 : oy;
    r[12] = (uint8_t)(ox & 0xFF);
    r[13] = (uint8_t)((ox >> 8) & 0xFF);
    r[14] = (uint8_t)(oy & 0xFF);
    r[15] = (uint8_t)((oy >> 8) & 0xFF);
}
