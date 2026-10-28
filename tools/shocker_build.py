r"""Build the Shocker-mode hero package (SHOCKER_PLAN.md).

    py -3 tools/shocker_build.py <overlay dir>

Writes <overlay dir>\data\heroes\SHOCKSPI.xbs and aux_xbox.txt.

The shockspidey package with the Shocker's animations from the boss level,
SUBWAY_D, added under their own names, and Spider-Man's animation list
(spiderman.ani) and moveset (spiderman.mbo) pointed at them: every
"anim_file X" / "anim X" naming a Spider-Man file in ANIMS is renamed. The
two skeletons share bone ids, so the Shocker's animations drive the hero
rig. No spider powers: swinging, crawling, zips and hanging are switched
off (TEXT), and his blasts are given strike events (SOURCES) so they hit.

The additions overflow the 4,092,800 bytes aux_xbox.txt reserves for a hero
package, so the aux_xbox.txt written beside it raises the shockspidey line.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xbs

GAME = r"C:\Users\adub9\xboxrecomp\game_files\data"
BUDGET = 6000000        # aux_xbox.txt bytes for the package; game's own: 4092800

# Spider-Man animation file -> the Shocker's.
ANIMS = {
    # standing, moving, falling
    "SmIdl": "ShkIdl",
    "SmWalk": "ShkWalkTns",
    "SmRun18": "ShkRun",
    "SmFall": "ShkFall",
    # jumps are his shock jump
    "SmJmpTwoLnch": "ShkShockJmpFLnch",
    "SmJmpTwoFly": "ShkShockJmpFFly",
    "SmJmpTwoLnd": "ShkShockJmpFLnd",
    # getting hit, knocked down, dying
    "SmIdlHitHigh": "ShkHitF",
    "SmCrumpleDeath": "ShkDeath",
    "SmBlowFLnch": "ShkBlowBLaunch",
    "SmBlowBLnch": "ShkBlowBLaunch",
    "SmBlowFFly": "ShkBlowFFly",
    "SmBlowBFly": "ShkBlowBFly",
    "SmBlowFLnd": "ShkBlowFLnd",
    "SmBlowBLnd": "ShkBlowBLnd",
    "SmProneB": "ShkDOnB",
    "SmProneF": "ShkDOnF",
    "SmGetUB": "ShkGetUB",
    "SmBlowFGetU": "ShkGetUF",
    # web weapons: impact web = weak blast, dome = ring blast, gloves = charge
    "SmImpactWeb": "ShkWeakBlast",
    "SmImpactWebADV": "ShkWeakBlast",
    "SmIdl2ShieldDome": "ShkShkwavRng",
    "SmShieldDomeIdl": "ShkStandBlastLoop",
    "SmShieldDome2Idl": "ShkStandBlast2Idl",
    "SmCreateWebSpikeHn": "ShkStandBlastChrg",
    "SmCreateWebSpikeAdv": "ShkStandBlastChrg",
    # combos: single hits are his shock punch (he has no kicks) ...
    "SmPHookR": "ShkPun2", "SmCrchPunR": "ShkPun2", "SmCrchPunL": "ShkPun2",
    "SmPPCrossL": "ShkPun2", "SmPunLSideJab": "ShkPun2", "SmPunR": "ShkPun2",
    "SmPunBckElbo": "ShkPun2", "SmKSideKck": "ShkPun2", "SmKKGutKck": "ShkPun2",
    "SmKckLSide": "ShkPun2", "SmKckRSide": "ShkPun2",
    "SmKckDualBckKck": "ShkPun2", "SmPK_Punt": "ShkPun2",
    "SmKPRUpperLnch": "ShkPun2", "SmJmpAirPunHit": "ShkPun2",
    "SmJmpAirKckHit": "ShkPun2",
    # ... three-hit finishers a gauntlet blast ...
    "SmPPPUpper": "ShkWeakBlastHit", "SmPPKThrust": "ShkWeakBlastHit",
    "SmKKKMuleKck": "ShkWeakBlastHit", "SmKKPUpper": "ShkWeakBlastHit",
    "SmKPPElbo": "ShkWeakBlastHit", "SmPKKThrust": "ShkWeakBlastHit",
    "SmPKWBall": "ShkWeakBlastHit", "SmPPWFeet": "ShkWeakBlastHit",
    "SmKKWCocoon": "ShkWeakBlastHit",
    # ... the strongest the ring blast ...
    "SmPKP": "ShkRingHit", "SmKPK": "ShkRingHit", "SmKJp": "ShkRingHit",
    # ... jump finishers the spin blast ...
    "SmPKJ_Palm": "ShkSpinHit", "SmPPJFrog": "ShkSpinHit",
    "SmKPJ": "ShkSpinHit", "SmKKJFlipMule": "ShkSpinHit",
    "SmPJP": "ShkSpinHit", "SmPJK": "ShkSpinHit",
    "SmKJJtackle": "ShkSpinHit", "SmKJKHandSpring": "ShkSpinHit",
    # ... and the uppercuts the weak ring jump.
    "SmJmpUpper": "ShkShockJmpHit", "SmJmpKnee": "ShkShockJmpHit",
    "SmPJJ": "ShkShockJmpHit",
}

# An animation ends with its events: (seconds, flag) pairs, offset at +0x4C.
# 0x100 is the strike (a combo move hits what its limbs touch then), 0x20 the
# fire / launch point (web shots leave, jumps take off). His blasts carry no
# strikes, so as combo finishers they hit nothing; these copies add them.
# name: (Shocker animation in SUBWAY_D, events or None to keep his own)
STRIKE, FIRE = 0x100, 0x20
SOURCES = {
    "ShkWeakBlastHit": ("ShkWeakBlast", [(0.3, STRIKE)]),
    "ShkRingHit": ("ShkShkwavRng", [(0.5, STRIKE), (2.47, STRIKE)]),
    "ShkSpinHit": ("ShkSpinSwavBlst", [(0.2, STRIKE), (0.45, STRIKE)]),
    "ShkShockJmpHit": ("ShkShockJmp", [(0.3, STRIKE), (1.07, STRIKE)]),
    # his own fires at 0.0, which a web shot never reaches
    "ShkWeakBlast": ("ShkWeakBlast", [(0.25, FIRE)]),
}

# Text edits: (entry, old, new). No spider powers: the Goblin's package turns
# traversal off the same way (plrgoblin_enx.inc).
TEXT = (
    ("spiderman_new.ai", "  type spiderman\r\n",
     "  type spiderman\r\n  disable_zip\r\n  disable_swing\r\n"
     "  disable_hang\r\n  disable_crawl\r\n"),
    # The web weapons: fields of the hero's spidermanX goal, read by the same
    # parser as head_icon (sub_00244FC0: web_tie +0xCF ... web_yank +0xD7).
    ("shockspidey.enx", "      head_icon",
     "".join("      %s 0\r\n" % k for k in (
         "web_tie_enabled", "web_spikes_enabled", "adv_web_spikes_enabled",
         "web_dome_enabled", "adv_web_dome_enabled", "impact_web_enabled",
         "adv_impact_web_enabled", "web_laser_enabled", "web_yank_enabled",
         "swing_attack_enabled"))
     + "      head_icon"),
)


def set_events(anim, events):
    """The animation with its event list replaced."""
    import struct
    end = struct.unpack_from("<I", anim, 0x4C)[0] or len(anim)
    out = bytearray(anim[:end])
    out.extend(bytes(-len(out) % 4))
    at = len(out)
    out += struct.pack("<4I", 0, 1, len(events), 0)
    for t, flag in events:
        out += struct.pack("<fI", t, flag)
    struct.pack_into("<I", out, 0x4C, at)
    return bytes(out)


def rename(text, keyword):
    """Replace '<keyword> <Spider-Man file>' with the Shocker's file."""
    lower = {k.lower(): v for k, v in ANIMS.items()}
    count = [0]
    def sub(m):
        new = lower.get(m.group(2).lower())
        if not new:
            return m.group(0)
        count[0] += 1
        return m.group(1) + new
    return re.sub(r"(\b%s[ \t]+)(\w+)\b" % keyword, sub, text), count[0]


def main(out_dir):
    hero = xbs.Package(os.path.join(GAME, "heroes", "SHOCKSPI.xbs"))
    boss = xbs.Package(os.path.join(GAME, "levels", "SUBWAY_D.xbs"))
    for shk in sorted(set(ANIMS.values())):
        src, events = SOURCES.get(shk, (shk, None))
        data = boss.data(src.lower() + ".anml")
        assert data[:4] == b"ANMX", src
        if events is not None:
            data = set_events(data, events)
        hero.put(shk.lower() + ".anml", data, xbs.TYPE_ANML)
    for name, keyword in (("spiderman.ani", "anim_file"), ("spiderman.mbo", "anim")):
        text, n = rename(hero.data(name).decode("latin1"), keyword)
        hero.put(name, text.encode("latin1"))
        print("%s: %d animations renamed" % (name, n))
    for name, old, new in TEXT:
        text = hero.data(name).decode("latin1")
        assert text.count(old) == 1, (name, old)
        hero.put(name, text.replace(old, new).encode("latin1"))
    print("%d Shocker animations; stream 0 %d bytes, reserved %d"
          % (len(set(ANIMS.values())), len(hero.s0), BUDGET))
    assert len(hero.s0) <= BUDGET

    heroes = os.path.join(out_dir, "data", "heroes")
    os.makedirs(heroes, exist_ok=True)
    hero.write(os.path.join(heroes, "SHOCKSPI.xbs"))
    aux = open(os.path.join(GAME, "heroes", "aux_xbox.txt"), "rb").read().decode("latin1")
    aux = re.sub(r"(SHOCKSPIDEY,SHOCKSPI,\d+,)\d+", r"\g<1>%d" % BUDGET, aux)
    open(os.path.join(heroes, "aux_xbox.txt"), "wb").write(aux.encode("latin1"))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
