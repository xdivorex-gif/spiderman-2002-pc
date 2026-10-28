r"""Build the Shocker-mode hero package (SHOCKER_PLAN.md).

    py -3 tools/shocker_build.py <overlay dir>

Writes <overlay dir>\data\heroes\SHOCKSPI.xbs and aux_xbox.txt.

The shockspidey package with the Shocker's animations from the boss level,
SUBWAY_D, added under their own names, and Spider-Man's animation list
(spiderman.ani) and moveset (spiderman.mbo) pointed at them: every
"anim_file X" / "anim X" naming a Spider-Man file in ANIMS is renamed. The
two skeletons share bone ids, so the Shocker's animations drive the hero
rig. Traversal (swinging, crawling, zips, hanging) keeps Spider-Man's.

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
    "SmPPPUpper": "ShkWeakBlast", "SmPPKThrust": "ShkWeakBlast",
    "SmKKKMuleKck": "ShkWeakBlast", "SmKKPUpper": "ShkWeakBlast",
    "SmKPPElbo": "ShkWeakBlast", "SmPKKThrust": "ShkWeakBlast",
    "SmPKWBall": "ShkWeakBlast", "SmPPWFeet": "ShkWeakBlast",
    "SmKKWCocoon": "ShkWeakBlast",
    # ... the strongest the ring blast ...
    "SmPKP": "ShkShkwavRng", "SmKPK": "ShkShkwavRng", "SmKJp": "ShkShkwavRng",
    # ... jump finishers the spin blast ...
    "SmPKJ_Palm": "ShkSpinSwavBlst", "SmPPJFrog": "ShkSpinSwavBlst",
    "SmKPJ": "ShkSpinSwavBlst", "SmKKJFlipMule": "ShkSpinSwavBlst",
    "SmPJP": "ShkSpinSwavBlst", "SmPJK": "ShkSpinSwavBlst",
    "SmKJJtackle": "ShkSpinSwavBlst", "SmKJKHandSpring": "ShkSpinSwavBlst",
    # ... and the uppercuts the weak ring jump.
    "SmJmpUpper": "ShkShockJmp", "SmJmpKnee": "ShkShockJmp", "SmPJJ": "ShkShockJmp",
}


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
        data = boss.data(shk.lower() + ".anml")
        assert data[:4] == b"ANMX", shk
        hero.put(shk.lower() + ".anml", data, xbs.TYPE_ANML)
    for name, keyword in (("spiderman.ani", "anim_file"), ("spiderman.mbo", "anim")):
        text, n = rename(hero.data(name).decode("latin1"), keyword)
        hero.put(name, text.encode("latin1"))
        print("%s: %d animations renamed" % (name, n))
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
