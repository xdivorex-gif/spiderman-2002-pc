"""Build overlay/pc and overlay/pc_ps: the game's text with PC wording.

    python tools/make_overlay.py [<game_files dir>]

The title's on-screen text is game_files/data/locales/<language>.lng, one
`key = "text"` per line (Latin-1, CRLF). The Xbox build reads its own keys
(the XBOX ones and the shared ones). This writes edited copies, at the same
relative path, under two overlay folders the game reads first
(RECOMP_OVERLAY_DIR, kernel_path.c):

    overlay/pc      Xbox wording made PC wording; buttons named A, B, START
    overlay/pc_ps   the same with PlayStation names: Cross, Circle, OPTIONS
                    (settings.c picks it when Button prompts is PlayStation)

Only English for now. The unpacked disc is never written.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
GAME = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\adub9\xboxrecomp\game_files"

# key -> (PC text, PlayStation text); None for the second means the same.
EDITS = {
    "XBOX Please Reconnect 1": ("Please reconnect the controller", None),
    "XBOX Please Reconnect 2": ("and press START to continue", "and press OPTIONS to continue"),
    "Please Reconnect 2": ("and press START button to continue",
                           "and press OPTIONS button to continue"),
    "XBOX Space Warning 1": ("Your PC doesn't have enough free disk space", None),
    "XBOX Space Warning 2": ("to save games.  Press A to continue",
                             "to save games.  Press Cross to continue"),
    "XBOX Space Warning 3": ("without saving.", None),
    "XBOX Blocks Required 1": ("Spider-Man requires", None),
    "XBOX Blocks Required 2": (" blocks of free disk space to save.", None),
    "Press START": ("Press START", "Press OPTIONS"),
}

LINE = re.compile(r'^(?P<key>[^=]+?)\s*=\s*"(?P<text>.*)"\s*$')


def build(lang, variant):
    src = os.path.join(GAME, "data", "locales", lang + ".lng")
    with open(src, "rb") as f:
        lines = f.read().decode("latin-1").split("\r\n")
    out, changed = [], 0
    for line in lines:
        m = LINE.match(line)
        if m and m.group("key") in EDITS:
            pc, ps = EDITS[m.group("key")]
            text = (ps or pc) if variant == "pc_ps" else pc
            if text != m.group("text"):
                changed += 1
            line = '%s = "%s"' % (m.group("key"), text)
        out.append(line)
    dst = os.path.join(PROJECT, "overlay", variant, "data", "locales", lang + ".lng")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "wb") as f:
        f.write("\r\n".join(out).encode("latin-1"))
    print("%s: %d lines changed -> %s" % (variant, changed, dst))


for variant in ("pc", "pc_ps"):
    build("english", variant)
