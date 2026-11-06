/*
 * venom_reach.c -- Venom's tentacle reach in the hero's combo hit test.
 *
 * The hero's combo strikes are decided in sub_001E4100 (the combo action's
 * update; it hands each victim to sub_001E3390, which deals the damage). The
 * action holds its move's record from +0x10, so +0x9c are the move's flags and
 * +0xAC its target_range_adjust (spiderman.mbo). A strike hits a target when
 *
 *     horizontal distance < attacker radius + target radius + range adjust
 *
 * and the target is inside the move's arc (+0xB0); candidates are gathered in
 * a sphere of range adjust + 3 round his pelvis. Spider-Man's moves say 1.0 or
 * so: a blow lands at a unit past contact, wherever the limbs are. (Found in
 * play: the damage call's return address for the hero's blows was 0x1E351F,
 * inside sub_001E3390; the enemies' melee test sub_001E6280 is another path.)
 *
 * Venom's tentacles strike from several units, as in Ultimate Spider-Man
 * (tools/venom_tentacles.py: 3.6 units out from the hand), so while he is worn
 * the action's copy of the range adjust is raised (see below). Only the
 * action's copy: the move record's own value also sets the arc of the lunge
 * toward a target (sub_0021DAA0), which stays as the move file says.
 */
#include <windows.h>
#include <stdint.h>

extern ptrdiff_t g_xbox_mem_offset;
extern __declspec(thread) uint32_t g_ecx;
int costume_worn_index(void);

#define GUESTF(va)  (*(volatile float *)((uint8_t *)g_xbox_mem_offset + (va)))
#define OK(p)       ((p) >= 0x10000u && (p) < 0x04000000u)

/* All in reach, not one: with the move flag multi_hit (0x400, MULTI_HIT in
 * the move file) the same function takes, on the strike, every entity in a
 * sphere of range adjust + 3.0 round his pelvis that is inside the move's arc,
 * each once per move (sub_001E14B0 skips the ones already hit). Without it,
 * only the best one within both radii + range adjust. His strikes get that
 * flag, and a range adjust that puts the sphere at 5.5 units: a unit past the
 * ~4.5 the single-target test gave with 3.5. */
#define VENOM_INDEX    10           /* thugspidey: the Venom slot */
#define ACTION_FLAGS   0x9Cu        /* the action's move flags */
#define ACTION_RANGE   0xACu        /* the action's target_range_adjust */
#define FLAG_MULTI_HIT 0x400u
#define SPHERE_BASE    3.0f         /* 0x383518, added to the range adjust */
#define VENOM_SPHERE   5.5f         /* units round his pelvis */

#define GUEST32(va) (*(volatile uint32_t *)((uint8_t *)g_xbox_mem_offset + (va)))

void venom_combo_reach(void)
{
    uint32_t action = g_ecx;

    if (!OK(action) || costume_worn_index() != VENOM_INDEX)
        return;
    GUEST32(action + ACTION_FLAGS) |= FLAG_MULTI_HIT;
    GUESTF(action + ACTION_RANGE) = VENOM_SPHERE - SPHERE_BASE;
}
