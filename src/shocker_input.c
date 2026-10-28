/*
 * shocker_input.c -- the Shocker's blasts on L2 + a face button.
 *
 * His blasts are Spider-Man's web specials re-skinned (tools/shocker_build.py):
 * the web button (Y; Triangle on a PlayStation pad) with a direction of the
 * d-pad, as the pause menu's Combat Controls lists them:
 *
 *   L2 + Square    Y + up      impact web    -> his double-arm blast,
 *                  then Y + down (the yank)     which knocks down what it hit
 *   L2 + Triangle  Y + right   web dome      -> his tornado (9 m, 120 damage)
 *   L2 + Circle    Y + down    web yank      -> his electric yank
 *   L2 + Cross     Y + left    web gloves    -> his charged gauntlets
 *
 * The yank is the game's own knock-down: what it pulls flies and lands on
 * the ground. Thrown shots have no explosive hit (combo moves' "explosive"
 * flag is what knocks down), and the explosion scripts crashed.
 *
 * Only while the Shocker is worn (costume_worn_index). L2 -- the left
 * trigger, analog byte 6 of the report -- is kept from the game while held;
 * a short tap with no face button is passed on afterwards, so what L2 does
 * in the game still works. A press is played as a hand does it (recorded
 * with RECOMP_PAD_RECORD): the d-pad direction, the web button 30 ms
 * later, both held about half a second.
 *
 * Called from costume_panel.c's report filter (there is one filter), on the
 * USB thread, for pad 0, while the game is not paused.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int costume_worn_index(void);

#define SHOCKER_INDEX 8             /* secrets index of shockspidey */

#define AN_A  0                     /* analog buttons: report bytes 4.. */
#define AN_B  1
#define AN_X  2
#define AN_Y  3
#define AN_LT 6
#define HELD(r, i) ((r)[4 + (i)] > 0x40)

/* The d-pad, not the stick: the stick only turned or walked him. */
#define PAD_MS   500                /* the d-pad direction, from the start */
#define WEB_FROM 30                 /* the web button, from .. to */
#define WEB_TO   500
#define TAP_MS   250                /* a shorter L2 hold with no blast is a tap */
#define PULSE_MS 100

#define DPAD_UP    0x01             /* report byte 2 */
#define DPAD_DOWN  0x02
#define DPAD_LEFT  0x04
#define DPAD_RIGHT 0x08

/* One or two specials in a row: the second starts `then` ms after the
 * first, once the first animation has let him go. */
static const struct { int face; uint8_t dpad, dpad2; unsigned then; } s_map[] = {
    { AN_X, DPAD_UP,   DPAD_DOWN,  900  },  /* Square:   blast, yank  */
    { AN_Y, DPAD_RIGHT, 0,         0    },  /* Triangle: dome (a yank
                                               first kept him busy, and the
                                               dome never came) */
    { AN_B, DPAD_DOWN, 0,          0    },  /* Circle:   yank         */
    { AN_A, DPAD_LEFT, 0,          0    },  /* Cross:    gloves       */
};

static int s_lt_down, s_used, s_prev_face[4];
static DWORD s_lt_t0, s_macro_t0, s_pulse_t0;
static int s_macro = -1, s_pulse;

/* RECOMP_PAD_RECORD=1: each change of the pad as the game gets it, to
 * stderr: buttons held and the left stick's direction, with the time --
 * to copy how a special is pressed by hand. */
static void record(const uint8_t *r)
{
    static int on = -1;
    static char last[64];
    static DWORD t0;
    char now[64];
    int16_t x, y;
    if (on < 0) {
        const char *e = getenv("RECOMP_PAD_RECORD");
        on = e && *e && *e != '0';
        t0 = GetTickCount();
    }
    if (!on)
        return;
    memcpy(&x, r + 12, 2);
    memcpy(&y, r + 14, 2);
    _snprintf(now, sizeof now, "%s%s%s%s%s%s dpad=%X stick=%s%s",
              HELD(r, AN_A) ? "A" : "-", HELD(r, AN_B) ? "B" : "-",
              HELD(r, AN_X) ? "X" : "-", HELD(r, AN_Y) ? "Y" : "-",
              HELD(r, AN_LT) ? "L" : "-", HELD(r, 7) ? "R" : "-", r[2] & 0xF,
              y > 12000 ? "U" : y < -12000 ? "D" : "", x > 12000 ? "R" : x < -12000 ? "L" : "");
    now[sizeof now - 1] = 0;
    if (strcmp(now, last)) {
        fprintf(stderr, "[PAD] %6lu %s\n", GetTickCount() - t0, now);
        strcpy(last, now);
    }
}

void shocker_input_filter(uint8_t *r)
{
    DWORD t = GetTickCount();
    int lt, k;

    record(r);
    lt = HELD(r, AN_LT);

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
        uint8_t dpad = s_map[s_macro].dpad;
        if (s_map[s_macro].dpad2 && dt >= s_map[s_macro].then) {
            dt -= s_map[s_macro].then;
            dpad = s_map[s_macro].dpad2;
        }
        if (dt < PAD_MS) {
            r[2] = (uint8_t)((r[2] & ~0x0F) | dpad);
            if (dt >= WEB_FROM && dt < WEB_TO)
                r[4 + AN_Y] = 0xFF;
        } else if (!s_map[s_macro].dpad2 || t - s_macro_t0 >= s_map[s_macro].then) {
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
