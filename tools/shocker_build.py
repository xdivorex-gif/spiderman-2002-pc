r"""Build the Shocker-mode hero package (SHOCKER_PLAN.md).

    py -3 tools/shocker_build.py <out SHOCKSPI.xbs>

Step 1 (proof): the shockspidey package with two of Spider-Man's animations
replaced by the Shocker's from the boss level, SUBWAY_D: the idle (SmIdl ->
ShkIdl) and the right hook, the first punch of every combo (SmPHookR ->
ShkPun2). The slots keep their names, so no text changes. Served in place
of heroes\SHOCKSPI.xbs through RECOMP_OVERLAY_DIR.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xbs

GAME = r"C:\Users\adub9\xboxrecomp\game_files\data"
BUDGET = 4092800        # aux_xbox.txt: memory reserved for a hero package

SWAPS = (
    ("smidl.anml", "shkidl.anml"),
    ("smphookr.anml", "shkpun2.anml"),
)


def main(out):
    hero = xbs.Package(os.path.join(GAME, "heroes", "SHOCKSPI.xbs"))
    boss = xbs.Package(os.path.join(GAME, "levels", "SUBWAY_D.xbs"))
    for slot, src in SWAPS:
        data = boss.data(src)
        assert data[:4] == b"ANMX", src
        hero.put(slot, data)
        print("%-16s <- %s (%d bytes)" % (slot, src, len(data)))
    print("stream 0: %d of %d bytes" % (len(hero.s0), BUDGET))
    assert len(hero.s0) <= BUDGET
    hero.write(out)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
