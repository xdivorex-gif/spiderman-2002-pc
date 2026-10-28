/*
 * shocker_input.c -- the Shocker's blasts on L2 + a face button.
 *
 * His blasts are Spider-Man's web specials re-skinned (tools/shocker_build.py):
 * the web button (Y; Triangle on a PlayStation pad) with a direction of the
 * left stick, as the pause menu's Combat Controls lists them:
 *
 *   L2 + Square    Y + up      impact web    -> his gauntlet bolt
 *   L2 + Triangle  Y + right   web dome      -> his ring blast (twice: strong)
 *   L2 + Circle    Y + down    web yank      -> his electric yank, and its
 *                                               cowboy spin his tornado
 *   L2 + Cross     Y + left    web gloves    -> his charged gauntlets
 *
 * Only while the Shocker is worn (costume_worn_index). L2 -- the left
 * trigger, analog byte 6 of the report -- is kept from the game while held;
 * a short tap with no face button is passed on afterwards, so what L2 does
 * in the game still works. A press is played as the web button held, the
 * stick pushed to the direction a moment later, then both let go.
 *
 * Called from costume_panel.c's report filter (there is one filter), on the
 * USB thread, for pad 0, while the game is not paused.
 */
#include <windows.h>
#include <stdint.h>
#include <string.h>

int costume_worn_index(void);

#define SHOCKER_INDEX 8             /* secrets index of shockspidey */

#define AN_A  0                     /* analog buttons: report bytes 4.. */
#define AN_B  1
#define AN_X  2
#define AN_Y  3
#define AN_LT 6
#define HELD(r, i) ((r)[4 + (i)] > 0x40)

/* The web button first and the direction while it is held ("Triangle +
 * right"). Direction first turned him to face it, so every combo came out
 * as a plain shot ahead. */
#define WEB_MS     260              /* the web button, from the start */
#define STICK_FROM 60               /* the direction, from .. to */
#define STICK_TO   240
#define TAP_MS   250                /* a shorter L2 hold with no blast is a tap */
#define PULSE_MS 100

static const struct { int face; int16_t x, y; } s_map[] = {
    { AN_X,  0,      32767 },       /* Square:   up    */
    { AN_Y,  32767,  0     },       /* Triangle: right */
    { AN_B,  0,     -32767 },       /* Circle:   down  */
    { AN_A, -32767,  0     },       /* Cross:    left  */
};

static int s_lt_down, s_used, s_prev_face[4];
static DWORD s_lt_t0, s_macro_t0, s_pulse_t0;
static int s_macro = -1, s_pulse;

static void put_stick(uint8_t *r, int16_t x, int16_t y)
{
    memcpy(r + 12, &x, 2);
    memcpy(r + 14, &y, 2);
}

void shocker_input_filter(uint8_t *r)
{
    DWORD t = GetTickCount();
    int lt = HELD(r, AN_LT), k;

    if (costume_worn_index() != SHOCKER_INDEX) {
        s_lt_down = 0;
        s_macro = -1;
        s_pulse = 0;
        return;
    }

    if (lt && !s_lt_down) {
        s_lt_down = 1;
        s_used = 0;
        s_lt_t0 = t;
    } else if (!lt && s_lt_down) {
        s_lt_down = 0;
        if (!s_used && t - s_lt_t0 < TAP_MS) {
            s_pulse = 1;
            s_pulse_t0 = t;
        }
    }

    if (s_lt_down) {
        r[4 + AN_LT] = 0;
        for (k = 0; k < 4; k++) {
            int f = s_map[k].face, now = HELD(r, f);
            if (now && !s_prev_face[k] && s_macro < 0) {
                s_macro = k;
                s_macro_t0 = t;
                s_used = 1;
            }
            s_prev_face[k] = now;
            r[4 + f] = 0;           /* the face button is the blast's, not its own */
        }
    } else {
        memset(s_prev_face, 0, sizeof s_prev_face);
    }

    if (s_macro >= 0) {
        DWORD dt = t - s_macro_t0;
        if (dt < WEB_MS) {
            r[4 + AN_Y] = 0xFF;
            put_stick(r, 0, 0);
            if (dt >= STICK_FROM && dt < STICK_TO)
                put_stick(r, s_map[s_macro].x, s_map[s_macro].y);
        } else {
            s_macro = -1;
        }
    }

    if (s_pulse) {
        if (t - s_pulse_t0 < PULSE_MS)
            r[4 + AN_LT] = 0xFF;
        else
            s_pulse = 0;
    }
}
