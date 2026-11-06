"""Build the Xbox-only Kraven levels (Bronx Zoo, ZOO_B / ZOO_C) as new
packages for the 2002 Windows release, without touching any of its files.

    py -3 tools/pc_zoo.py <xbox data dir> <pc data dir> [out dir]

SpiderMan.exe already lists both levels (M7bronxzoo\\bronxzoo_b,
levels\\zoo_b ...); only data\\levels\\ZOO_B.pkg and ZOO_C.pkg are missing.
For every file in the Xbox package:
  * in COMMON.pkg: skipped (the PC loads it for every level);
  * in another PC level package: the PC's own copy, under the same folder;
  * otherwise converted: .dds -> .TGA (dds2tga), .xbmesh -> .PCMESH (pcmesh),
    everything else as is (the scripts, entities, animations, collision and
    the rest are byte-identical on both platforms), in the folder PC levels
    use for that kind of file.
"""
import glob
import os
import sys
import struct
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dds2tga  # noqa: E402
import pc_menus  # noqa: E402
import pc_pkg  # noqa: E402
import pcmesh  # noqa: E402
import sx  # noqa: E402
import xbs  # noqa: E402

BS = '\\'
LEVEL_DIR = 'M7BRONXZOO' + BS
LEVELS = ('ZOO_B', 'ZOO_C')
CORE = {'sx', 'sxl', 'scn', 'sin', 'terx', 'snml', 'txtmesh', 'path', 'voq'}


def write_alias_package(path, files):
    """Multiple directories reference one file record; do not duplicate payloads."""
    dirs, unique, indices = {}, [], {}
    for d, n, data in files:
        key = (n, data)
        if key not in indices:
            indices[key] = len(unique)
            unique.append((n, data))
        dirs.setdefault(d, []).append(indices[key])
    body = bytearray()
    records = bytearray()
    for n, data in unique:
        body.extend(bytes(-len(body) % 4))
        records += n.encode('latin1').ljust(40, b'\0')
        records += struct.pack('<II', len(data), len(body))
        body += data
    header = bytearray(struct.pack('<III', len(dirs), len(unique), len(body)))
    for d, ids in dirs.items():
        assert len(d.encode('latin1')) < 64
        header += d.encode('latin1').ljust(64, b'\0') + struct.pack('<I', len(ids))
        header += struct.pack('<%dI' % len(ids), *ids)
    with open(path, 'wb') as f:
        f.write(header + records + body)
    check = pc_pkg.Pkg(path)
    actual = {(d, check.files[i][0]): check.data(i) for d, ids in check.dirs for i in ids}
    assert actual == {(d, n): data for d, n, data in files}


def pc_name(n):
    n = n.upper()
    if n.endswith('.DDS'):
        return n[:-4] + '.TGA'
    if n.endswith('.XBMESH'):
        return n[:-7] + '.PCMESH'
    return n


def is_kraven(n):
    """Kraven's own files: his model, animations (kr*.anml), AI and weapons."""
    low = n.lower()
    return low.startswith(('kraven', 'kr')) or low.split('.')[0] in ('huntrifle', 'machette', 'machete')


def folder(n, kraven_tex):
    base, ext = n.rsplit('.', 1)
    ext = ext.lower()
    low = n.lower()
    if ext in CORE:
        return LEVEL_DIR
    if ext == 'fe':
        return 'INTERFACE' + BS
    if ext == 'ai':
        return 'AI' + BS
    if ext == 'fon':
        return 'INTERFACE' + BS + 'FONT' + BS
    if ext == 'pcmesh' and low.startswith('bronxzoo_'):
        return LEVEL_DIR                       # the level's own region meshes
    if ext in ('tga', 'ifl'):
        if low.startswith('ib_'):
            return 'INTERFACE' + BS + 'TGA' + BS
        if low in kraven_tex or is_kraven(low):
            return 'CHARACTERS' + BS + 'KRAVEN' + BS + 'TGA' + BS
        return LEVEL_DIR + 'TGA' + BS
    if is_kraven(low):
        return 'CHARACTERS' + BS + 'KRAVEN' + BS
    return LEVEL_DIR + 'ENTITIES' + BS


TRAP_DELAY = 1.0   # seconds a trap waits before making its trigger
# The level start scripts (SIN "START level_start", ZOO_C's kraven_battle)
# freeze and suspend Spider-Man on the level's first frame. Run that way on
# PC his height intermittently came out NaN at once (inside every trigger:
# traps, then level_end -> mission complete). With the debugger pausing that
# first suspend() it never did; so the start scripts first wait START_DELAY.
START_DELAY = 0.25
START_OBJECTS = {'"level_start"', '"kraven_battle"'}
SKIP_INTRO_SCENE = False     # tried 2026-11-05: the no-scene path crashes on load
STRIP_START_TUNING = False   # tried 2026-11-05: made no difference to the NaN
# Where each level's start script puts Spider-Man (ZOO_B: level_start.start's
# own set_rel_position; ZOO_C: its .sin HERO_START).
HERO_SPOT = {'ZOO_B': (63.233, 0.994, 18.727), 'ZOO_C': (21.549, 0.994, 0.159)}


def pc_script(raw, sxl, hero_spot=None, menu_src=None):
    """-> (.sx, .sxl) for the PC.

    Button font: the Xbox scripts pick a button-icon font per console and
    have no PC case, so on PC they fall through to a font the PC does not
    ship (the game exits: "Unable to load font metrics file ..."). That
    fallback string (any E_button_icons_*_36px other than PC, xbox and gc) is
    pointed at the PC font in COMMON.pkg (strings are referenced by number;
    the bytecode is unchanged).

    Traps: every trap's setup waits one frame and then calls
    create_trigger(entity). On PC that frame comes before level_start's
    start() has moved Spider-Man off his spawn point, whose height is still
    NaN there (traced: the bear traps' blasts aim at 62.46, NaN, 30.28), and
    a NaN position is inside every trigger box: all traps fired at once at
    load (the explosion, the fire, the electric hum, the death). So each
    object setup function (named after its object; not the level object's
    helpers) that calls create_trigger first waits TRAP_DELAY seconds:
    "push TRAP_DELAY; _global_slc.delay(num)" put in front of its code. Being
    in front, every jump in it keeps its offset; the new code is appended to
    the .sxl and the function's code offset / size updated."""
    strings, objs, _ = sx.parse(raw)
    font = '"interface' + BS + 'font' + BS + 'E_button_icons_%s_36px"'
    pc = font % 'PC'
    known = {(font % c).lower() for c in ('PC', 'xbox', 'gc')}
    fallback = [s for s in strings if s.lower().startswith(font.split('%s')[0].lower())
                and s.endswith('_36px"') and s.lower() not in known]
    if len(fallback) == 1 and pc not in strings:
        strings[strings.index(fallback[0])] = pc

    def string_id(text):
        if text not in strings:
            strings.append(text)
        return strings.index(text)
    trig = '"create_trigger(entity)"'
    cls, fn = string_id('"_global_slc"'), string_id('"delay(num)"')
    code = bytearray(sxl)
    patched = []
    for o in objs:
        for f in o['funcs']:
            local = [strings[i] for i in f['strings']]
            # only setup functions (named after their object), never the
            # level object's helpers (barrels, damage loops run mid-game)
            starter = strings[o['name']] in START_OBJECTS and f['name'] == o['name']
            if not starter and (trig not in local or f['name'] != o['name'] or strings[o['name']].startswith('"_')):
                continue
            ids = []
            for gid in (cls, fn):
                if gid not in f['strings']:
                    f['strings'].append(gid)
                ids.append(f['strings'].index(gid))
            wait = START_DELAY if starter else TRAP_DELAY
            hi, lo = struct.unpack('<2H', struct.pack('<f', wait))[::-1]
            pre = struct.pack('<H2H', 0x1D01, hi, lo) + struct.pack('<H2H', 0x040A, ids[0], ids[1])
            body = sxl[f['code']:f['code'] + f['codesize']]
            code.extend(bytes(-len(code) % 4))
            f['code'] = len(code)
            code.extend(pre + body)
            f['codesize'] = len(pre) + len(body)
            patched.append(strings[o['name']] + '.' + strings[f['name']])

    # Cut-scene skip. The Xbox scenes hook "X_PRESSED or START_PRESSED" on
    # script_controller CONTROLLER_1:
    #   push m11 <script_controller, CONTROLLER_1>; push m16 <X_PRESSED>
    #   push m11 <script_controller, CONTROLLER_1>; push m16 <START_PRESSED>
    #   op 0x29 (either signal)  ... then the skip callback.
    # The PC's own scenes also listen on script_keyboard KEYBOARD_1 KEY_DOWN
    # (key_down_cine_1), which is what a pad through the PC's input mapping
    # sends; the zoo's never skipped. The second half becomes KEY_DOWN on
    # KEYBOARD_1 (same instructions, new string operands), so X on a pad or
    # any key skips.
    want = {'"script_controller"', '"CONTROLLER_1"', '"script_controller::X_PRESSED"',
            '"script_controller::START_PRESSED"'}
    kb = [string_id(t) for t in ('"script_keyboard"', '"KEYBOARD_1"', '"script_keyboard::KEY_DOWN"')]
    for o in objs:
        for f in o['funcs']:
            local = [strings[i] for i in f['strings']]
            if not want <= set(local):
                continue
            ctl = (local.index('"script_controller"'), local.index('"CONTROLLER_1"'))
            start = local.index('"script_controller::START_PRESSED"')
            a, end = f['code'], f['code'] + f['codesize']
            seq = struct.pack('<H2H', 0x1D0B, *ctl) + struct.pack('<HH', 0x1D10, start) + b'\x00\x29'
            p = code.find(seq, a, end)
            while p >= 0:
                ids = []
                for gid in kb:
                    if gid not in f['strings']:
                        f['strings'].append(gid)
                    ids.append(f['strings'].index(gid))
                code[p:p + 10] = struct.pack('<H2H', 0x1D0B, ids[0], ids[1]) + struct.pack('<HH', 0x1D10, ids[2])
                patched.append('skip: ' + strings[o['name']] + '.' + strings[f['name']])
                p = code.find(seq, p + 1, end)

    # Hero reset. On PC a race in the level's first frames intermittently
    # leaves Spider-Man's physics NaN; position set by script came straight
    # back NaN, so velocity is NaN too. He is suspended through the opening
    # scene (triggers ignore him), so just before the start script
    # unsuspends him, put him on his spot and zero his velocity.
    if hero_spot:
        ids_needed = ('"vector3d"', '"vector3d(num,num,num)"', '"entity"',
                      '"set_rel_position(vector3d)"', '"set_velocity(vector3d)"', '"unsuspend()"', '"HERO"')
        gids = {t: string_id(t) for t in ids_needed}
        for o in objs:
            if strings[o['name']] not in START_OBJECTS:
                continue
            for f in o['funcs']:
                local = [strings[i] for i in f['strings']]
                if '"unsuspend()"' not in local or '"HERO"' not in local:
                    continue
                for t, g in gids.items():
                    if g not in f['strings']:
                        f['strings'].append(g)
                L = {t: f['strings'].index(g) for t, g in gids.items()}
                body = bytes(code[f['code']:f['code'] + f['codesize']])
                hero = struct.pack('<H2H', 0x1D0B, L['"entity"'], L['"HERO"'])
                unsus = hero + struct.pack('<H2H', 0x040A, L['"entity"'], L['"unsuspend()"'])
                at = body.find(unsus)
                if at < 0:
                    continue

                def push(v):
                    hi, lo = struct.unpack('<2H', struct.pack('<f', v))[::-1]
                    return struct.pack('<H2H', 0x1D01, hi, lo)
                vec = struct.pack('<H2H', 0x040A, L['"vector3d"'], L['"vector3d(num,num,num)"'])
                blob = (hero + b''.join(push(v) for v in hero_spot) + vec +
                        struct.pack('<H2H', 0x040A, L['"entity"'], L['"set_rel_position(vector3d)"']) +
                        hero + push(0.0) * 3 + vec +
                        struct.pack('<H2H', 0x040A, L['"entity"'], L['"set_velocity(vector3d)"']))
                new = insert_code(body, at, blob)
                code.extend(bytes(-len(code) % 4))
                f['code'] = len(code)
                code.extend(new)
                f['codesize'] = len(new)
                patched.append('hero reset: ' + strings[o['name']] + '.' + strings[f['name']])
    # Experiment (SKIP_INTRO_SCENE): level_start.start plays the opening
    # scene anim unless in_goblin_mode(); pointing that call at
    # get_max_level_unlocked() (non-zero once any level is unlocked) takes
    # the path without the scene, to test whether the scene anim is what
    # leaves Spider-Man's physics NaN.
    if SKIP_INTRO_SCENE:
        for o in objs:
            if strings[o['name']] != '"level_start"':
                continue
            for f in o['funcs']:
                if strings[f['name']] != '"start"':
                    continue
                body = bytes(code[f['code']:f['code'] + f['codesize']])
                # at 12: op 0x12 (negate), bf -> 74. Becomes: pop the
                # in_goblin_mode() result, jmp to 74 (now 76: two bytes longer)
                assert body[12:14] == bytes((0x00, 0x12)) and body[14:16] == bytes((0x05, 0x02)), body[12:18].hex()
                target_old = 18 + struct.unpack_from('<h', body, 16)[0]
                blob_len = 8
                new_end = 12 + blob_len
                target_new = target_old + (blob_len - 6)
                blob = struct.pack('<Hh', 0x2104, -4) + struct.pack('<Hh', 0x0305, target_new - new_end)
                new = replace_code(body, 12, 6, blob)
                code.extend(bytes(-len(code) % 4))
                f['code'] = len(code)
                code.extend(new)
                f['codesize'] = len(new)
                patched.append('experiment: no opening scene')

    # Experiment (STRIP_START_TUNING): the start scripts retune Spider-Man
    # before anything else (set_ai_loco_num("JUMP_FORCE", n),
    # spiderman_set_max_zip_length(n)); no PC level does the first. Each call
    # is replaced by a pop of its arguments to see whether the NaN goes away.
    if STRIP_START_TUNING:
        drop = {'"set_ai_loco_num(str,num)"': (0x0B, '"entity"', -12),
                '"spiderman_set_max_zip_length(num)"': (0x0A, '"_global_slc"', -4)}
        for o in objs:
            if strings[o['name']] not in START_OBJECTS:
                continue
            for f in o['funcs']:
                local = [strings[i] for i in f['strings']]
                for fn, (_, cls, pop) in drop.items():
                    if fn not in local or cls not in local:
                        continue
                    call = struct.pack('<H2H', 0x040A, local.index(cls), local.index(fn))
                    body = bytes(code[f['code']:f['code'] + f['codesize']])
                    at = body.find(call)
                    if at < 0:
                        continue
                    new = replace_code(body, at, len(call), struct.pack('<Hh', 0x2104, pop))
                    code.extend(bytes(-len(code) % 4))
                    f['code'] = len(code)
                    code.extend(new)
                    f['codesize'] = len(new)
                    patched.append('dropped %s: %s.%s' % (fn, strings[o['name']], strings[f['name']]))
    # Menus (pause, results, game over, yes/no): the PC's own menu code, which
    # takes a pad through the keyboard path; the Xbox menus froze. pc_menus.py.
    if menu_src:
        patched += pc_menus.transplant(strings, objs, code, *menu_src)
    return sx.write(strings, objs), bytes(code), patched


def insert_code(body, at, blob):
    """Put blob in front of the instruction at offset `at` (see replace_code)."""
    return replace_code(body, at, 0, blob)


def replace_code(body, at, old_len, blob):
    """Replace the old_len bytes of whole instructions at `at` by blob;
    relative jumps (opcodes 2 and 3, mode 5: offset from the end of the
    instruction) are adjusted. A jump to `at` lands on the new code."""
    import sxdis
    shift = len(blob) - old_len

    def moved(x):
        return x + shift if x >= at + old_len and (old_len or x > at) else x
    out = bytearray()
    p = 0
    while p < len(body):
        if p == at:
            out += blob
        w = struct.unpack_from('<H', body, p)[0]
        mode, op = w & 0x7F, w >> 8
        q = p + 2 + (2 if w & 0x80 else 0)
        size = sxdis.operand_size(mode)
        if at <= p < at + old_len:
            p = q + size
            continue
        ins = bytearray(body[p:q + size])
        if op in (2, 3) and mode == 5:
            end = q + size
            target = end + struct.unpack_from('<h', body, q)[0]
            new_end = end + (shift if p >= at + old_len else 0)
            struct.pack_into('<h', ins, q - p, moved(target) - new_end)
        out += ins
        p = q + size
    return bytes(out)


def glass_fix(mat):
    """The sniper walkway's glass (bronxzoo_b112r/113r and the like, flags
    0x000494A0: a cut-out in slot 2, a reflection in slot 4, nothing in
    slot 1) drew solid on PC. The PC picks a material's blend mode
    (SpiderMan.exe FUN_007FEA70) from +0x38, or from +0xB0 when the only
    texture is in slot 4 (flags & 0xF0 == 0x80). These glass materials have
    +0x38 = 0 (opaque) and +0xB0 = 5 (alpha blend), and the Xbox draws them
    with the +0xB0 mode; so +0x38 takes the +0xB0 value. (Tried first: clear
    bit 0x800, black material colour, the cut-out moved to slot 1 with and
    without bit 0x01 -- solid, solid, black, black; then +0x38 = 5 alone:
    see-through but the corridor behind it missing.)"""
    fl = int.from_bytes(mat[:4], 'little')
    blend, env_blend = struct.unpack_from('<I', mat, 0x38)[0], struct.unpack_from('<I', mat, 0xB0)[0]
    if fl & 0x20 and not fl & 0x10 and fl & 0x80 and blend == 0 and env_blend:
        # Laid out like the PC's own glass (OSCORPBX obx_glasspane, flags
        # 0x49891): the cut-out texture in slot 1 with that slot's mode 2 (alpha
        # blend) and strength 255, the reflection in slot 4 as it was, flags
        # 0x10 | 0x800 for slot 1 instead of 0x20 | 0x400 for slot 2, and bit
        # 0x01, which every see-through PC material has (drawn in the sorted
        # pass after the solid world). Each slot is a 0x20-byte name then
        # (mode u32, strength u32). The Xbox gives slot 2 mode 4, which the PC
        # draws as a solid tint.
        m = bytearray(mat)
        m[0x18:0x38] = mat[0x40:0x60]
        struct.pack_into('<2I', m, 0x38, 2, 255)
        m[0x40:0x68] = bytes(0x28)
        fl = (fl & ~0x420) | 0x810 | 0x01
        struct.pack_into('<I', m, 0, fl)
        mat = bytes(m)
    return mat


GLASS_FIX = True
MENU_SOURCE = 'OSCORP_B'   # the PC level whose menu code the Kraven levels get


def build(xbox_data, pc_data, out_dir):
    pcmesh.MAT_FIX = glass_fix if GLASS_FIX else None
    common = pc_pkg.Pkg(os.path.join(pc_data, 'COMMON.pkg'))
    in_common = {f[0].upper() for f in common.files}
    common_entries = {}
    for d, indices in common.dirs:
        for i in indices:
            n = common.files[i][0].upper()
            common_entries.setdefault(n, []).append((d, n, common.data(i)))
    elsewhere = {}                              # NAME -> (dirs, bytes)
    for f in sorted(glob.glob(os.path.join(pc_data, 'levels', '*.pkg'))):
        if os.path.basename(f)[:-4].upper() in LEVELS:
            continue
        p = pc_pkg.Pkg(f)
        dirs = defaultdict(set)
        for i in range(len(p.files)):
            dirs[p.files[i][0].upper()].add(p.dir_of.get(i, ''))
        for i in range(len(p.files)):
            n = p.files[i][0].upper()
            if n not in elsewhere:
                elsewhere[n] = (sorted(dirs[n]), p.data(i))
    os.makedirs(out_dir, exist_ok=True)
    ref = pc_pkg.Pkg(os.path.join(pc_data, 'levels', MENU_SOURCE + '.pkg'))
    ref_files = {ref.files[i][0].upper(): ref.data(i) for i in range(len(ref.files))}
    menu_src = tuple(next(d for n, d in ref_files.items() if n.endswith(ext)) for ext in ('.SX', '.SXL'))
    for lv in LEVELS:
        x = xbs.Package(os.path.join(xbox_data, 'levels', lv + '.xbs'))
        # textures the Kraven model uses go with him
        kraven_tex = set()
        for e in x.entries:
            if e.name.lower().startswith('kraven') and e.name.lower().endswith('.xbmesh'):
                for m in pcmesh.read_xbox(x.data(e.name)):
                    for s in m['subs']:
                        for o in (0x18, 0x40, 0x68, 0x90):
                            t = s['mat'][o:o + 0x28].split(b'\0')[0].decode('latin1').lower()
                            if t:
                                kraven_tex.add(t + '.tga')
        files = []
        stats = Counter()
        sx_name = [e.name for e in x.entries if e.name.lower().endswith('.sx')][0]
        new_sx, new_sxl, patched = pc_script(x.data(sx_name), x.data(sx_name + 'l'), HERO_SPOT.get(lv), menu_src)
        for p in patched:
            if p.startswith('menus: '):
                print(lv, p)
        patched = [p for p in patched if not p.startswith('menus: ')]
        skips = [p for p in patched if p.startswith('skip: ')]
        print('%s: %d trap functions wait %.1f s before their trigger; %d cut-scene skips also take a key'
              % (lv, len(patched) - len(skips), TRAP_DELAY, len(skips)))
        for e in sorted(x.entries, key=lambda e: e.name.lower()):
            n = pc_name(e.name)
            if n in in_common:
                files.extend(common_entries[n])
                stats['in COMMON'] += 1
                continue
            if n in elsewhere:
                dirs, data = elsewhere[n]
                for d in dirs:
                    files.append((d, n, data))
                stats['PC copy'] += 1
                continue
            raw = x.data(e.name)
            if n.endswith('.TGA'):
                data = dds2tga.convert(raw)
                stats['texture converted'] += 1
            elif n.endswith('.PCMESH'):
                data = pcmesh.convert(raw)
                stats['mesh converted'] += 1
            elif n.endswith('.SX'):
                data = new_sx
                stats['script patched'] += 1
            elif n.endswith('.SXL'):
                data = new_sxl
                stats['script patched'] += 1
            else:
                data = raw
                stats['as is'] += 1
            files.append((folder(n, kraven_tex), n, data))
        # Xbox lookup is name-based; PC lookup includes the directory. Keep
        # canonical PC paths but also expose reused assets in the zoo's search
        # directories. Scene imports such as KRAVEN_SNIPER_RIFLE use bare names.
        # (The fallback button font is handled in pc_script(): copies of the
        # PC font under the fallback name were tried and the loader did not
        # find them.)
        aliases = {}
        variants = defaultdict(set)
        for d, n, data in files:
            aliases[(d.upper(), n)] = data
            variants[n].add(data)
        for d, n, data in files:
            if len(variants[n]) != 1:
                continue  # Same basename can identify distinct common assets.
            ext = n.rsplit('.', 1)[-1]
            extra = []
            if ext in ('TGA', 'IFL'):
                extra = [LEVEL_DIR + 'TGA' + BS,
                         LEVEL_DIR + 'ENTITIES' + BS + 'TGA' + BS]
            elif ext in ('ENT', 'ENX', 'PCMESH', 'COLL', 'ANML', 'ANI', 'TBB'):
                extra = [LEVEL_DIR + 'ENTITIES' + BS]
            for alias in extra:
                key = (alias, n)
                if key in aliases and aliases[key] != data:
                    raise ValueError('Conflicting asset alias: ' + alias + n)
                aliases[key] = data
        files = [(d, n, data) for (d, n), data in aliases.items()]
        # one directory record per folder, files grouped
        files.sort(key=lambda f: (f[0], f[1]))
        out = os.path.join(out_dir, lv + '.pkg')
        write_alias_package(out, files)
        print('%s: %d files, %.1f MB  %s' % (lv, len(files), os.path.getsize(out) / 1e6, dict(stats)))
        for d, c in sorted(Counter(f[0] for f in files).items()):
            print('    %-34s %d' % (d, c))


if __name__ == '__main__':
    build(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else 'pc_zoo_out')
