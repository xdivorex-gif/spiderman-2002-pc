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
    "SmIdl2ShieldDome": "ShkStandBlastChrg",
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
    # the jump leaves the ground at the fire event: his crouch is 0.33 s
    "ShkShockJmpFLnch": ("ShkShockJmpFLnch", [(0.2, FIRE)]),
}

# Text edits: (entry, old, new). No spider powers: the Goblin's package turns
# traversal off the same way (plrgoblin_enx.inc).
TEXT = (
    ("spiderman_new.ai", "  type spiderman\r\n",
     "  type spiderman\r\n  disable_zip\r\n  disable_swing\r\n"
     "  disable_hang\r\n  disable_crawl\r\n"
     # His shock jump is how he gets around instead: jump height goes with
     # the force squared. Defaults (sub_0025A1B0 / its constructor): 20, 15,
     # 7.5, 12.5.
     "  jump_force 34.0\r\n  double_jump_force 30.0\r\n"
     "  jump_xz_move_speed 11.0\r\n  double_jump_xz_move_speed 16.0\r\n"),
    # The web weapons: fields of the hero's spidermanX goal, read by the same
    # parser as head_icon (sub_00244FC0: web_tie +0xCF ... web_yank +0xD7).
    # The impact web stays on: it becomes his gauntlet bolt (BOLT below).
    # The dome and gloves stay on too: his ring blast and charged gauntlets
    # (ENTITIES below).
    ("shockspidey.enx", "      head_icon",
     "".join("      %s 0\r\n" % k for k in (
         "web_tie_enabled", "web_laser_enabled", "web_yank_enabled",
         "swing_attack_enabled"))
     + "      head_icon"),
)

# Effects the engine and the level scripts create by name (default.xbe names
# smshitassdome / smshitassglove_*; the scripts' spiderman_web_dome_destroy
# makes spideywebdomeexp), so they are re-skinned by replacing the hero's
# entity of that name. name: a SUBWAY_D entity whose text to use, or text.
IDENTITY = "1.000000 0.000000 0.000000 0.000000 0.000000 1.000000 0.000000 0.000000 0.000000 0.000000 1.000000 0.000000 "


def conglom(*sprays):
    """An effect entity of particle systems at its origin."""
    out = '"NEWENT"\r\nnode\r\n"CONGLOM"\r\npo\r\n%s\r\nconglom\r\nchunkend\r\n' % IDENTITY
    for i, s in enumerate(sprays):
        out += 'node\r\n"P%d"\r\npo\r\n%s\r\npartsys\r\n"%s"\r\nchunkend\r\nchunkend\r\n' % (i, IDENTITY, s.upper())
    return out + "chunkend\r\n"


def limb(node, *sprays):
    """A glove: the node the engine attaches to a forearm, as particles."""
    out = '"NEWENT"\r\nnode\r\n"%s"\r\npo\r\n%s\r\n' % (node, IDENTITY)
    if sprays:
        out += 'partsys\r\n"%s"\r\n' % sprays[0].upper()
    else:
        out += "conglom\r\n"
    return out + "chunkend\r\nchunkend\r\n"


def reskin(text, mesh=None, sprays=()):
    """Keep an entity's nodes (names, animids, bones), swap what they show:
    every mesh part shows `mesh`, every particle system the next of
    `sprays`, and a skinned mesh on the root is dropped. The level scripts
    animate the dome and its burst (smshitassdome_a.anmx,
    spideywebdomeadvexp_a.anmx) and look its parts up by name (DOME01..):
    an entity without them makes the game stop with a fatal error."""
    text = re.sub(r'(conglom\r?\n)meshfile\r?\n"[^"]*"\r?\n', r"\1", text)
    if mesh:
        text = re.sub(r'(entity\r?\nmeshfile\r?\n)"[^"]*"', r'\1"%s"' % mesh, text)
    if sprays:
        n = [0]
        def next_spray(m):
            n[0] += 1
            return m.group(1) + '"%s"' % sprays[(n[0] - 1) % len(sprays)].upper()
        text = re.sub(r'(partsys\r?\n)"[^"]*"', next_spray, text)
    return text


TRAILS = {
    "r_hand": "255 255 255", "l_hand": "255 255 255",
    "r_arm": "120 150 255", "l_arm": "120 150 255",
    "r_foot": "192 192 255", "l_foot": "192 192 255",
    "r_leg": "84 107 224", "l_leg": "84 107 224",
}

RING_MESH, WAVE_MESH ="fxshkwringring", "fxshksringgrndwave"
RING_SPRAYS = ("sprayshksringflash", "sprayshksringgrndwave", "sprayshksringchunk",
               "sprayshksringglow", "sprayshksringblast", "sprayshksringwaveslow",
               "sprayshksringupchunk", "sprayshksringupring", "sprayshksringflysmoke",
               "sprayshksringburst")
WEAK_SPRAYS = ("sprayshkwringflash", "sprayshkwringring", "sprayshkwringwaveslow")

# name: SUBWAY_D entity whose text to use, text, or (reskin mesh, sprays)
# applied to the hero's own entity of that name.
ENTITIES = {
    # web dome: held = rings of his energy round him, released = ring blast
    "smshitassdome.ent": (RING_MESH, ()),
    "smshitassdomeadv.ent": (RING_MESH, ()),
    "spideywebdomefx.ent": (None, WEAK_SPRAYS),
    "spideywebdomeexp.ent": (None, RING_SPRAYS),
    "spideywebdomeadvexp.ent": (RING_MESH, RING_SPRAYS),
    "spideywebdomeleftover.ent": (WAVE_MESH, ()),
    "spideywebdomeadvleftover.ent": (WAVE_MESH, ()),
    # every punch that lands (the engine makes fx\smhitreacta / b): his
    # gauntlet-hit sparks
    "smhitreacta.ent": (None, ("sprayshkgntsinrhitstar", "sprayshkgntsinrhitring")),
    "smhitreactb.ent": (None, ("sprayshkgntsinrhitstar", "sprayshkgntsinrhitring",
                               "sprayshkgntsinrhitrim")),
    # web gloves: electricity on the lower arms (ll/rl), nothing above
    "smshitassglove_ll.ent": limb("LL", "sprayshkhandelec"),
    "smshitassglove_rl.ent": limb("RL", "sprayshkhandelec"),
    "smshitassglove_lu.ent": limb("LU"),
    "smshitassglove_ru.ent": limb("RU"),
    "smshitassgloveadv_ll.ent": limb("LL", "sprayshkhandelec"),
    "smshitassgloveadv_rl.ent": limb("RL", "sprayshkhandelec"),
    "smshitassgloveadv_lu.ent": limb("LU", "sprayshkhandglowring"),
    "smshitassgloveadv_ru.ent": limb("RU", "sprayshkhandglowring"),
}

# The impact web shots (thrown items) re-skinned as his gauntlet bolt: his
# bolt and hit effects instead of the web ball, and no web scripts (they
# stick webbing to what is hit). Regex edits per item, applied in order.
BOLT_FX, HIT_FX = "shkgntsinbolt", "shkgntsinrhitfx"
BOLT = [
    (r"visual_entity \w+", "visual_entity " + BOLT_FX),
    (r"\n\s*script spiderman_\w+(launch)\b", r"\n    //script spiderman_\1"),
    (r"\n(\s*)script spiderman_\w+", r"\n\1effect items\\entities\\%s\r\n\1lifetime 0.5" % HIT_FX),
    (r"\n\s*sticky\w*[^\n]*", ""),
    (r"\n\s*preload [^\n]*", ""),
    # His bolt is a beam that crosses a room in a blink; the web ball is a
    # slow homing shot that lives 3-5 s, trailing his effect all the way.
    (r"launch_force [\d.]+", "launch_force 45.0"),
    (r"\n(\s*)timer [\d.]+", r"\n\1timer 1.0"),
]
BOLT_ITEMS = ("impact_web.enx", "adv_impact_web.enx")

# Particle settings of the copied effects, tuned for that projectile: the
# boss's trail emitters (45 rings a second, living up to 2.5 s) pile up
# along a flight that long and cost frames. spray entity: {field: value}.
SPRAYS = {
    "sprayshkgntsinboltring.ent": {"rate": 15, "life": 0.3, "fadeout": 0.25},
    "sprayshkgntsinboltwavea.ent": {"rate": 10, "life": 0.4, "fadeout": 0.3},
    "sprayshkgntsinboltwind.ent": {"rate": 5, "life": 0.4, "fadeout": 0.3},
    "sprayshkgntsinrhitchunk.ent": {"rate": 20, "life": 0.5},
    "sprayshkgntsinrhitfly.ent": {"rate": 10, "life": 0.6, "fadeout": 0.3},
    "sprayshkgntsinrhitrim.ent": {"rate": 8, "life": 0.5, "fadeout": 0.4},
    "sprayshkgntsinrhitbeam.ent": {"rate": 10},
}


def tune_spray(text, fields):
    """Set the first '<field>' value (on the next line) of a spray entity."""
    for field, value in fields.items():
        text, n = re.subn(r"(\n[ \t]*%s[ \t]*\r?\n[ \t]*)[-\d.]+" % field,
                          r"\g<1>%s" % value, text, count=1)
        assert n == 1, field
    return text

# Entry extensions an effect can reference by bare name.
REF_EXTS = (".ent", ".enx", ".dds", ".xbmesh", ".tbb", ".ifl")


def copy_effects(hero, src, roots):
    """Copy effect entities and everything they name (sprays, meshes,
    textures) from a level package into the hero package. Names the hero
    already has are kept. Textures go first, then meshes, then text, so
    each is in place before what uses it."""
    import struct
    found, todo = {}, [r + x for r in roots for x in REF_EXTS]
    while todo:
        name = todo.pop()
        if name in found or hero.find(name) or not src.find(name):
            continue
        e = src.find(name)
        found[name] = e
        # text names its sprays and meshes; a mesh names its textures in
        # its materials, as plain strings among the binary
        if e.stream == xbs.IN_TEXT or name.endswith(".xbmesh"):
            raw = src.data(name).decode("latin1")
            for tok in set(re.findall(r"[A-Za-z0-9_]{3,}", raw)):
                tok = tok.lower()
                todo += [tok + x for x in REF_EXTS]
    rank = {xbs.TYPE_DDS: 0, xbs.TYPE_MESH: 1}
    for name in sorted(found, key=lambda n: (rank.get(0x100 | (found[n].type & 0xFF), 2), n)):
        t = 0x100 | (found[name].type & 0xFF)
        hero.put(name, src.data(name), t,
                 align=0x1000 if t in (xbs.TYPE_DDS, xbs.TYPE_MESH) else 0x20)
    return sorted(found)


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
    roots = [BOLT_FX, HIT_FX]
    for name, src in ENTITIES.items():
        if isinstance(src, tuple):
            mesh, sprays = src
            text = reskin(hero.data(name).decode("latin1"), mesh, sprays)
            roots += [mesh] if mesh else []
        elif "\n" in src:
            text = src
        else:
            text = boss.data(src + ".ent").decode("latin1")
            roots.append(src)
        roots += [s.lower() for s in re.findall(r'"(SPRAY\w+)"', text)]
        hero.put(name, text.encode("latin1"))
    fx = copy_effects(hero, boss, roots)
    print("effects: %d entries copied" % len(fx))
    # motion trails: his gauntlets' blue-white glow instead of gold and brown
    text = hero.data("shockspidey.enx").decode("latin1")
    for part, rgb in TRAILS.items():
        text, n = re.subn(r"(%s_trail_color)[ \t]+[\d \t]+" % part, r"\1 %s" % rgb, text)
        assert n == 1, part
    hero.put("shockspidey.enx", text.encode("latin1"))
    for name, fields in SPRAYS.items():
        hero.put(name, tune_spray(hero.data(name).decode("latin1"), fields).encode("latin1"))
    for name in BOLT_ITEMS:
        text = hero.data(name).decode("latin1")
        for pat, rep in BOLT:
            text = re.sub(pat, rep, text)
        hero.put(name, text.encode("latin1"))
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
