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
import struct
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
    # combos: the punch button is his shock punch ...
    "SmPHookR": "ShkPun2", "SmCrchPunR": "ShkPun2", "SmCrchPunL": "ShkPun2",
    "SmPPCrossL": "ShkPun2", "SmPunLSideJab": "ShkPun2", "SmPunR": "ShkPun2",
    "SmPunBckElbo": "ShkPun2", "SmPK_Punt": "ShkPun2",
    "SmKPRUpperLnch": "ShkPun2", "SmJmpAirPunHit": "ShkPun2",
    # ... the kick button (he has no kicks) a short gauntlet blast, three
    # kicks his standing blast, a kick in the air his shock blast down ...
    "SmKSideKck": "ShkWeakBlastHit", "SmKKGutKck": "ShkWeakBlastHit",
    "SmKckLSide": "ShkWeakBlastHit", "SmKckRSide": "ShkWeakBlastHit",
    "SmKckDualBckKck": "ShkWeakBlastHit",
    "SmKKKMuleKck": "ShkStandBlastHit",
    "SmJmpAirKckHit": "ShkShockJmpHit",
    # ... three-hit finishers a gauntlet blast ...
    "SmPPPUpper": "ShkWeakBlastHit", "SmPPKThrust": "ShkWeakBlastHit",
    "SmKKPUpper": "ShkWeakBlastHit",
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
    # dodges and rolls are his spin blast (it strikes on the way)
    "SmDodgeF": "ShkSpinHit", "SmDodgeB": "ShkSpinHit",
    "SmRollL": "ShkSpinHit", "SmRollR": "ShkSpinHit",
    # one-arm web fire (partial, arm only): his one-arm gauntlet blasts
    "SmRArmWebFire": "ShkRArmBlst", "SmLArmWebFire": "ShkLArmBlst",
    # web yank: the shot is a blast, the pull his blast release, the lasso
    # his spin; the line itself is lightning (TEXTURES)
    "SmWebYankFire": "ShkYankFire", "SmCrchWebYankFireL": "ShkYankFire",
    "SmWebYankB": "ShkYankPull", "SmWebYankU": "ShkYankPull",
    "SmWebYankL": "ShkYankPull", "SmWebYankR": "ShkYankPull",
    "SmCrchWebYankB": "ShkYankPull", "SmCrchWebYankR": "ShkYankPull",
    "SmWebCowboy": "ShkSpinSwavBlst",
    # the rest of standing and moving: fidgets look around, crouching is his
    # stance, run starts and stops his run and idle
    "SmIdl2BoredLook": "ShkFacingL", "SmIdl2BoredCrchLook": "ShkFacingL",
    "SmCrchIdl": "ShkIdl", "SmCrch2Idl": "ShkIdl", "SmIdl2Crch": "ShkIdl",
    "SmIdl2Run": "ShkRun", "SmLnd2Run": "ShkRun",
    "SmRun2IdlMid": "ShkIdl", "SmRun2IdlLLegF": "ShkIdl", "SmRun2IdlRLegF": "ShkIdl",
    "SmRun2Idl_NoMove": "ShkIdl",
    "SmRun2LeapFlip": "ShkShockJmpFLnch", "SmRun1mLedgeU": "ShkShockJmpFLnd",
    # jumps: the double jump is his shock jump flight. The 180-degree turns
    # (SmIdlTrn180, SmRunTurnTrn, SmJmpTurn180) stay Spider-Man's: the turn is
    # in the animation, and his clips do not turn, so he ended facing backwards
    "SmJmpFlipJmp": "ShkShockJmpFFly",
    # web shots from the air or a crouch: his blast
    "SmJmpImpactWeb": "ShkWeakBlast", "SmCrchImpactWeb": "ShkWeakBlast",
    # hits and deaths
    "SmCrchIdlHit": "ShkHitF", "SmFFall": "ShkFall", "SmFFallLnd": "ShkBlowFLnd",
    # the rest of the combos: evasive flips his backward shock jump, prone,
    # sneak and web kicks his blasts, extra punches his shock punch
    "SmPJFlip": "ShkShockJmpBLnch", "SmKJBckFlip": "ShkShockJmpBLnch",
    "SmPunProne": "ShkWeakBlastHit", "SmKckProne": "ShkWeakBlastHit",
    "SmKckProne2": "ShkWeakBlastHit", "SmKckWebChest": "ShkWeakBlastHit",
    "SmKckSneak": "ShkPun2", "SmKckSneakSlam": "ShkStandBlastHit",
    "SmPunLUpper": "ShkPun2", "SmPunRHook": "ShkPun2",
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
    "ShkStandBlastHit": ("ShkStandBlastLoop", [(0.1, STRIKE), (0.35, STRIKE), (0.6, STRIKE)]),
    # the yank's line leaves and pulls at Spider-Man's moments
    "ShkYankFire": ("ShkWeakBlast", [(0.13, FIRE)]),
    "ShkYankPull": ("ShkStandBlast2Idl", [(-0.03, FIRE)]),
}

# Textures replaced by the Shocker's art: hero texture -> (level package,
# texture). The yank's line (spideywebstring*, drawn by the engine) becomes
# his lightning; it is DXT1 without alpha, so its brightness becomes alpha.
TEXTURES = {
    "spideywebstring": ("SUBWAY_D", "lightninga04"),
    "spideywebstringb": ("SUBWAY_D", "lightninga01"),
    "spideywebstringc": ("SUBWAY_D", "lightninga02"),
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
     "  jump_force 46.0\r\n  double_jump_force 44.0\r\n"
     "  jump_xz_move_speed 16.0\r\n  double_jump_xz_move_speed 22.0\r\n"
     # His run animation strides shorter than Spider-Man's sprint; the
     # locomotion base (sub_001F83F0, +0x118) scales movement.
     "  speed_modifier 1.5\r\n"),
    # The web weapons: fields of the hero's spidermanX goal, read by the same
    # parser as head_icon (sub_00244FC0: web_tie +0xCF ... web_yank +0xD7).
    # The impact web stays on: it becomes his gauntlet bolt (BOLT below).
    # The dome and gloves stay on too: his ring blast and charged gauntlets
    # (ENTITIES below).
    ("shockspidey.enx", "      head_icon",
     "".join("      %s 0\r\n" % k for k in (
         "web_tie_enabled", "web_laser_enabled", "swing_attack_enabled"))
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


# Sounds. A level loads its own bank and the menu bank, which holds the
# player's sounds (web shots, dome, footsteps, grunts) and stays loaded; his
# are only in subway_d's. data\xboxsound\<lang>\<bank>.XSH: three dwords
# (157, 0, entry count), then 0x60-byte entries: name[32], 1, offset and
# size in the .XSD, flags, volume / pitch / distances, an Xbox ADPCM
# WAVEFORMATEX. They are appended to the menu bank, and the hero's sound
# groups renamed to them.
SOUND_BANK = os.path.join(os.path.dirname(GAME), "data", "xboxsound", "ENGLISH")
HIS_SOUNDS = ("sh_gauntlet_blast", "sh_gauntlet_hit", "sh_tornado", "sh_weakring",
              "sh_strongring", "elecarc1", "footsteps_heavy1", "footsteps_heavy2",
              "footsteps_heavy3", "footsteps_heavy4", "shaiv010", "shaiv011",
              "shaiv012", "shaiv013", "shaiv014", "shaiv015", "shaiv016")
SOUNDS = {
    "webshot1": "sh_gauntlet_blast", "webshot2": "sh_gauntlet_blast",
    "webshot3": "sh_gauntlet_blast", "weblaunch": "sh_gauntlet_blast",
    "weblaunch_advanced": "sh_gauntlet_blast",
    "webimpact": "sh_gauntlet_hit", "weblaunch_advanced_explo": "sh_gauntlet_hit",
    "punch_sm": "sh_gauntlet_hit", "punch_sm2": "sh_gauntlet_hit",
    "webdome": "sh_weakring", "webdome2": "sh_weakring", "webbust": "sh_strongring",
    "webgloves": "elecarc1", "webgloves2": "elecarc1",
    "webtie": "elecarc1", "webyoyo": "elecarc1",
    "sm_ftsteps1": "footsteps_heavy1", "sm_ftsteps2": "footsteps_heavy2",
    "sm_ftsteps3": "footsteps_heavy3", "sm_ftsteps4": "footsteps_heavy4",
    # his jump is a blast off the ground and a ring on landing
    "smaiv059": "sh_gauntlet_blast", "smaiv060": "sh_gauntlet_blast",
    "smaiv061": "sh_gauntlet_blast", "smaiv062": "sh_gauntlet_blast",
    "sm_landing": "sh_weakring",
}
TAUNTS = ("shaiv010", "shaiv011", "shaiv012", "shaiv013", "shaiv014", "shaiv015", "shaiv016")


def bank_entries(path):
    d = open(path, "rb").read()
    n = struct.unpack_from("<I", d, 8)[0]
    return d[:12], [bytearray(d[12 + i * 0x60:12 + (i + 1) * 0x60]) for i in range(n)]


def build_bank(out_dir):
    """menu.XSH / .XSD with his sounds appended."""
    hdr, menu = bank_entries(os.path.join(SOUND_BANK, "menu.XSH"))
    _, boss = bank_entries(os.path.join(SOUND_BANK, "subway_d.XSH"))
    data = bytearray(open(os.path.join(SOUND_BANK, "menu.XSD"), "rb").read())
    boss_data = open(os.path.join(SOUND_BANK, "subway_d.XSD"), "rb").read()
    by_name = {e[:32].split(b"\0")[0].decode(): e for e in boss}
    for name in HIS_SOUNDS:
        e = bytearray(by_name[name])
        off, size = struct.unpack_from("<II", e, 0x24)
        struct.pack_into("<I", e, 0x24, len(data))
        data += boss_data[off:off + size]
        menu.append(e)
    hdr = bytearray(hdr)
    struct.pack_into("<I", hdr, 8, len(menu))
    dst = os.path.join(out_dir, "data", "xboxsound", "ENGLISH")
    os.makedirs(dst, exist_ok=True)
    open(os.path.join(dst, "menu.XSH"), "wb").write(bytes(hdr) + b"".join(menu))
    open(os.path.join(dst, "menu.XSD"), "wb").write(data)
    print("sound: %d of his sounds in the menu bank (+%d KB)"
          % (len(HIS_SOUNDS), (len(data) - os.path.getsize(os.path.join(SOUND_BANK, "menu.XSD"))) // 1024))


def resound(text):
    """Point 'file X' at his sounds, and give the taunt groups his taunts."""
    lower = {k.lower(): v for k, v in SOUNDS.items()}
    text = re.sub(r"(\bfile[ \t]+)(\w+)",
                  lambda m: m.group(1) + lower.get(m.group(2).lower(), m.group(2)), text)
    # the yank's lasso is his spin: its whoosh (shared with every attack)
    # becomes his tornado in that group only
    text = re.sub(r"(name rodeo_yank\b.*?file[ \t]+)whoosh3", r"\1sh_tornado", text,
                  count=1, flags=re.S)
    taunts = "".join("\t\t\tsound:\r\n\t\t\t\tfile %s\r\n\t\t\t\tfreq 1.0\r\n\t\t\tchunkend\r\n" % t
                     for t in TAUNTS)
    return re.sub(r"(name taunt\r?\n\s*history -?\d+\r?\n\s*voice\r?\n)", r"\1" + taunts.replace("\\", "\\\\"), text)


def dxt5_from_brightness(dds):
    """A DXT1 texture as DXT5 with alpha = brightness (glowing art on
    black), so it blends where the original had alpha."""
    import hud_upscale as hu
    h, w = struct.unpack_from("<II", dds, 12)
    px = hu.decode_dxt(dds[128:], w, h, dds[84:88])
    out = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            blk = [px[(by + t // 4) * w + bx + t % 4] for t in range(16)]
            rgb = [((p >> 16) & 255, (p >> 8) & 255, p & 255) for p in blk]
            alpha = [max(c) for c in rgb]
            a0, a1 = max(alpha), min(alpha)
            levels = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)] \
                if a0 > a1 else [a0, a1, a0, a0, a0, a0, 0, 255]
            bits = 0
            for t, a in enumerate(alpha):
                bits |= min(range(8), key=lambda i: abs(levels[i] - a)) << (3 * t)
            out += bytes((a0, a1)) + bits.to_bytes(6, "little")
            def c565(c):
                return (c[0] * 31 // 255) << 11 | (c[1] * 63 // 255) << 5 | (c[2] * 31 // 255)
            hi = max(rgb, key=sum)
            lo = min(rgb, key=sum)
            e0, e1 = c565(hi), c565(lo)
            ends = [hi, lo, tuple((2 * a + b) // 3 for a, b in zip(hi, lo)),
                    tuple((a + 2 * b) // 3 for a, b in zip(hi, lo))]
            idx = 0
            for t, c in enumerate(rgb):
                i = min(range(4), key=lambda k: sum((x - y) ** 2 for x, y in zip(c, ends[k])))
                idx |= i << (2 * t)
            out += struct.pack("<HHI", e0, e1, idx)
    hdr = bytearray(dds[:128])
    hdr[84:88] = b"DXT5"
    struct.pack_into("<I", hdr, 20, len(out))      # linear size
    return bytes(hdr) + bytes(out)


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
    # His gauntlets' blue aura: the boss's TRAILR / TRAILL, particle trails
    # under his hand bones that are always on (his ELEC / RING / STAR nodes
    # are hidden until the boss scripts show them), put under the hero's.
    boss_ent = boss.data("shocker.ent").decode("latin1")
    ent = hero.data("shockspidey.ent").decode("latin1")
    for side in "RL":
        node = re.search(r'node\r?\n"TRAIL%s".*?chunkend\r?\nchunkend\r?\n' % side,
                         boss_ent, re.S).group(0).replace("\r\n", "\n").replace("\n", "\r\n")
        # a bone's data closes with two chunkends; its children follow
        hand = re.search(r'"BIP01 %s HAND"\r\nanimid\r\n"%s_HAND"\r\npo\r\n[^\r\n]*\r\nentity\r\nbone\r\n\d+\r\nchunkend\r\nchunkend\r\n'
                         % (side, side), ent)
        ent = ent[:hand.end()] + node + ent[hand.end():]
        roots.append("sprayshkhandtrail")
    hero.put("shockspidey.ent", ent.encode("latin1"))
    copy_effects(hero, boss, ["sprayshkhandtrail"])
    for name in ("spiderman_enx.inc", "impact_web.enx", "adv_impact_web.enx"):
        hero.put(name, resound(hero.data(name).decode("latin1")).encode("latin1"))
    build_bank(out_dir)
    levels = {}
    for name, (level, src) in TEXTURES.items():
        if level not in levels:
            levels[level] = xbs.Package(os.path.join(GAME, "levels", level + ".xbs"))
        dds = levels[level].data(src + ".dds")
        if dds[84:88] == b"DXT1":
            dds = dxt5_from_brightness(dds)
        hero.put(name + ".dds", dds, xbs.TYPE_DDS, align=0x1000)
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
