"""Give a converted Xbox level script the PC's menu code.

On PC the pause menu, results screen, game-over (quit / retry) and the
yes/no dialogs are driven through the keyboard path: each menu's
set_*_controls clears KEYBOARD_1's callbacks, calls clear_keyboard_input()
and hooks script_keyboard::KEY_DOWN to a key_down_* handler, which reads
keyboard_next_key_down() (a pad's buttons come through the PC input mapping
that way). The Xbox scripts only hook script_controller CONTROLLER_1 button
signals, which the PC does not deliver to them, so in the Kraven levels every
menu froze: the pause menu, the level results screen and game over.

This takes those pieces from a PC level's script (any; OSCORP_B is used):
  - the menu objects (MENU_OBJECTS) are replaced whole, function list,
    parameters and data size included;
  - in the level object (the "_<level>" one holding the shared library),
    library functions whose names match LIB_NAMES get the PC body, and PC
    library functions the Xbox level lacks (key_down_quit_retry,
    select_quit_retry, the mouse helpers, ...) are added at the end of its
    function list when transplanted code calls them (existing function
    numbers stay put).

Bytecode refers to a function by (object string, function number) (operand
mode 9: calls, callback hooks) and to the level object's globals by (object
string, byte offset) (mode 8). So every mode-9 operand in the whole script is
re-pointed by function name (the replaced objects' numbering changed), and
the PC code's mode-8 offsets into the level object are mapped onto the Xbox
level's layout: offsets used by functions both versions share are paired
up instruction by instruction; offsets only the PC uses get a fresh copy of
the PC's global block appended after the Xbox globals (same relative layout,
so arrays stay contiguous; zero at start, as statics are).
"""
import re
import struct
from collections import Counter, defaultdict

import sx
import sxdis

MENU_OBJECTS = ('"pause_menu"', '"bonus_reward_screen"', '"fast_times_display"', '"level_ender"')
LIB_NAMES = re.compile(r'quit_retry|quit_try|dialog|window_|yesno|yes_no|res_get|"text_|tag_to_button|'
                       r'button_icons|button_str|level_skip|"go_on"|pause_menu|chose_to_not_save|'
                       r'begin_adventure|return_to_training|training_play|key_down|mouse')


def level_object(strings, objs):
    c = [o for o in objs if strings[o['name']].startswith('"_') and strings[o['name']] != '"_common_preload"']
    assert len(c) == 1, [strings[o['name']] for o in c]
    return c[0]


def instructions(body):
    """(operand offset, mode, operand size) per instruction."""
    p = 0
    while p + 2 <= len(body):
        w = struct.unpack_from('<H', body, p)[0]
        q = p + 2 + (2 if w & 0x80 else 0)
        n = sxdis.operand_size(w & 0x7F)
        yield q, w & 0x7F, n
        p = q + n


def refs(body, local, name, mode):
    out = []
    for q, m, n in instructions(body):
        if m == mode and n == 4:
            a, b = struct.unpack_from('<2H', body, q)
            if a < len(local) and local[a] == name:
                out.append(b)
    return out


def transplant(strings, objs, code, src_sx, src_sxl):
    """strings / objs / code (bytearray) of the target script, changed in
    place. Returns a list of notes."""
    s_strings, s_objs, _ = sx.parse(src_sx)
    s_by = {s_strings[o['name']]: o for o in s_objs}
    t_by = {strings[o['name']]: o for o in objs}
    s_L, t_L = level_object(s_strings, s_objs), level_object(strings, objs)
    sL, tL = s_strings[s_L['name']], strings[t_L['name']]
    notes = []

    def string_id(text):
        if text not in strings:
            strings.append(text)
        return strings.index(text)

    def s_local(f):
        return [s_strings[i] for i in f['strings']]

    def t_local(f):
        return [strings[i] for i in f['strings']]

    def s_body(f):
        return src_sxl[f['code']:f['code'] + f['codesize']]

    def t_body(f):
        return bytes(code[f['code']:f['code'] + f['codesize']])

    # Functions are told apart by name and parameter types: the library
    # overloads some (move_selection_icon, quit_retry_screen, window_construct,
    # text_create, cine_on, ...), and a call by name alone picked the wrong
    # one (the pause menu then read arguments that were never passed).
    def s_key(f):
        return (s_strings[f['name']], tuple(s_strings[t] for t in (f['parms'] or [])))

    def t_key(f):
        return (strings[f['name']], tuple(strings[t] for t in (f['parms'] or [])))
    # function keys per object, as the code was compiled against
    s_names = {n: [s_key(f) for f in o['funcs']] for n, o in s_by.items()}
    t_names = {n: [t_key(f) for f in o['funcs']] for n, o in t_by.items()}

    # 1. global offset map (source level object -> target level object)
    votes = defaultdict(Counter)
    for n, so in s_by.items():
        tn = tL if so is s_L else n
        if tn not in t_by:
            continue
        tf = {t_key(f): f for f in t_by[tn]['funcs']}
        for f in so['funcs']:
            g = tf.get(s_key(f))
            if not g:
                continue
            a = refs(s_body(f), s_local(f), sL, 8)
            b = refs(t_body(g), t_local(g), tL, 8)
            if len(a) == len(b):
                for x, y in zip(a, b):
                    votes[x][y] += 1
    omap = {x: c.most_common(1)[0][0] for x, c in votes.items()}
    mirror = (t_L['statsize'] + 15) & ~15

    keys = sorted(omap)

    def map_offset(x):
        # paired; or between two paired offsets with the same shift (a
        # variable both have that no shared function happens to use): that
        # shift; else the PC-only copy
        if x in omap:
            return omap[x]
        lo = [k for k in keys if k < x]
        hi = [k for k in keys if k > x]
        if lo and hi and omap[lo[-1]] - lo[-1] == omap[hi[0]] - hi[0]:
            return x + omap[hi[0]] - hi[0]
        return mirror + x

    # 2. what moves: whole menu objects, chosen library functions, and the
    #    PC library functions they need that the target lacks
    t_lnames = t_names[tL]
    replace = [k for k in s_names[sL] if k in t_lnames and LIB_NAMES.search(k[0])]
    added = []
    s_lf0 = {s_key(f): f for f in s_L['funcs']}
    t_lf0 = {t_key(f): f for f in t_L['funcs']}

    def shape(lines):
        # instruction text without function numbers, data offsets and the
        # level object's name
        return [re.sub(r'\.\d+(:\S+)?|\+\d+|"_[a-z0-9_]+"', '', l.split(None, 1)[1]) for l in lines]

    def differs(key):
        a, b = s_lf0[key], t_lf0[key]
        return shape(sxdis.disasm(s_body(a), s_local(a))) != shape(sxdis.disasm(t_body(b), t_local(b)))
    todo = [s_by[n] for n in MENU_OBJECTS]
    todo_funcs = [f for o in todo for f in o['funcs']] + [f for f in s_L['funcs'] if s_key(f) in replace]
    seen = set()
    while todo_funcs:
        f = todo_funcs.pop()
        if id(f) in seen:
            continue
        seen.add(id(f))
        body, local = s_body(f), s_local(f)
        for q, m, n in instructions(body):
            if m == 9 and n == 4:
                a, b = struct.unpack_from('<2H', body, q)
                if local[a] == sL:
                    name = s_names[sL][b]
                    if name not in t_lnames and name not in added:
                        added.append(name)
                        todo_funcs.append(s_L['funcs'][b])
                    elif name in t_lnames and name not in replace and differs(name):
                        # e.g. move_selection_icon: the PC menus call it with
                        # other arguments than the Xbox one takes (it read past
                        # the script stack in the pause menu)
                        replace.append(name)
                        todo_funcs.append(s_L['funcs'][b])
    final_names = dict(t_names)
    for n in MENU_OBJECTS:
        final_names[n] = s_names[n]
    final_names[tL] = t_lnames + added

    def rename(t):
        return tL if t == sL else t

    def rewrite(body, local, names_before, from_source):
        """Re-point mode-9 operands by function name; map the source's mode-8
        level-object offsets."""
        out = bytearray(body)
        for q, m, n in instructions(body):
            if n != 4 or m not in (8, 9):
                continue
            a, b = struct.unpack_from('<2H', body, q)
            if a >= len(local):
                continue
            obj = local[a]
            if m == 9:
                if obj not in names_before:
                    continue
                fname = names_before[obj][b]
                dst = final_names[rename(obj) if from_source else obj]
                if fname not in dst:
                    raise SystemExit('pc_menus: %s.%s%s has no target' % (obj, fname[0], fname[1]))
                struct.pack_into('<2H', out, q, a, dst.index(fname))
            elif from_source and obj == sL:
                struct.pack_into('<2H', out, q, a, map_offset(b))
        return bytes(out)

    def place(src_f, target_f=None):
        """Copy a source function into the target (new dict, or over target_f)."""
        local = s_local(src_f)
        body = rewrite(s_body(src_f), local, s_names, True)
        f = target_f if target_f is not None else {}
        f['name'] = string_id(s_strings[src_f['name']])
        # parameter types are string numbers too (num, str, vector3d, ...)
        f['parms'] = [string_id(s_strings[t]) for t in src_f['parms']] if src_f['parms'] is not None else None
        f['parmsize'] = src_f['parmsize']
        f['strings'] = [string_id(rename(t)) for t in local]
        code.extend(bytes(-len(code) % 4))
        f['code'] = len(code)
        code.extend(body)
        f['codesize'] = len(body)
        return f

    # 3. target code first (its own numbering), in place
    for o in objs:
        if strings[o['name']] in MENU_OBJECTS:
            continue
        for f in o['funcs']:
            new = rewrite(t_body(f), t_local(f), t_names, False)
            code[f['code']:f['code'] + f['codesize']] = new

    # 4. menu objects
    for n in MENU_OBJECTS:
        so, to = s_by[n], t_by[n]
        to['kind'], to['statsize'], to['datasize'] = so['kind'], so['statsize'], so['datasize']
        to['funcs'] = [place(f) for f in so['funcs']]
    # 5. level object: replaced bodies, then added functions
    s_lf, t_lf = s_lf0, t_lf0
    for name in replace:
        place(s_lf[name], t_lf[name])
    for name in added:
        t_L['funcs'].append(place(s_lf[name]))
    t_L['statsize'] = mirror + s_L['statsize']
    notes.append('menus: %d objects; level object: %d library functions replaced, %d added (%s); '
                 '%d global offsets paired, statics now %d'
                 % (len(MENU_OBJECTS), len(replace), len(added), ', '.join(k[0] for k in added), len(omap), t_L['statsize']))
    return notes
