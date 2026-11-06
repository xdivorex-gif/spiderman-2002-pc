"""USM Venom animations -> 2002 ANMX on the thugspidey slot's 28-bone rig.

Pipeline (see cap/usm/NOTES.md, "Venom animation"):
  USM channel frames (tools/usm_decode_channels.py) -> USM bone world
  matrices (tools/usm_venom_pose.py) -> per bone the world-space rotation
  from USM bind, applied to the 2002 bind rotation -> 2002 track locals ->
  ANMX keys.

2002 ANMX (found by matching Spider-Man's own idle to his entity rest):
  +0x10 duration (s), +0x14 track count, track descriptors 0x40 each from
  +0x20 (+0x20 first child, +0x24 next sibling, +0x28 data offset). Track
  data: duration, flags, position block, rotation block, scale block
  offsets; each block = count, 0, keys. Position key = time, x, y, z
  floats; rotation key = time float, w x y z int16 (/32767), conjugate of
  usm_pose.qmat's convention; world = local x parent (row vectors).
  Animation space is the mesh's (+y up, +z front). Bones without position
  keys take the translation of their entity "po" (4th column).

The slot's skeleton is moved to Venom's proportions (USM bind x SCALE) in the
mesh bind and the entity's rest translations, so his limbs bend at his own
joints.
"""
import json
import math
import re
import shlex
import struct
from pathlib import Path

import usm_pose as up
import usm_venom_pose as vp
import venom_tentacles as vt
import xbs

SCALE = 0.59
# 2002 track -> USM bone, and 2002 track -> slot mesh bone
TRACK_USM = {1: 0, 2: 1, 3: 2, 4: 3, 5: 5, 6: 6, 7: 8, 8: 9, 9: 10, 10: 11,
             26: 30, 27: 31, 28: 32, 29: 33,
             45: 52, 46: 53, 47: 54, 48: 55, 49: 56, 50: 57, 51: 58, 52: 59}
TRACK_BONE = {1: 0, 2: 1, 3: 2, 4: 3, 5: 4, 6: 5, 7: 10, 8: 11, 9: 12, 10: 13, 11: 14,
              26: 15, 27: 16, 28: 17, 29: 18, 30: 19,
              45: 20, 46: 21, 47: 22, 48: 23, 49: 24, 50: 25, 51: 26, 52: 27}
# slot mesh bone -> parent mesh bone (Biped: clavicles on the neck)
BONE_PARENT = {1: 0, 2: 1, 3: 2, 4: 3, 5: 4, 6: 5, 7: 5, 8: 5, 9: 5, 10: 4, 11: 10, 12: 11,
               13: 12, 14: 13, 15: 4, 16: 15, 17: 16, 18: 17, 19: 18, 20: 0, 21: 20,
               22: 21, 23: 22, 24: 0, 25: 24, 26: 25, 27: 26}
POS_TRACKS = (0, 1, 7, 26)        # root motion, pelvis, clavicles
I4 = [[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, 1.0, 0], [0, 0, 0, 1.0]]


def mesh_bones(mesh):
    n, at = struct.unpack_from('<2I', mesh, 0x6c)
    return [[list(struct.unpack_from('<4f', mesh, at + i * 64 + 16 * r)) for r in range(4)]
            for i in range(n)], at


def venom_bind(thug_bind, usm_bind):
    """Slot bind: the thug's rotations at Venom's (scaled) joints."""
    usm_of = {TRACK_BONE[t]: u for t, u in TRACK_USM.items()}
    out = [None] * len(thug_bind)
    for b in range(len(thug_bind)):
        m = [row[:] for row in thug_bind[b]]
        if b in usm_of:
            m[3][:3] = up.scale(up.pos(usm_bind[usm_of[b]]), SCALE)
        else:   # jaw, lips, fingers: keep the thug's offset from the parent
            p = BONE_PARENT[b]
            m[3][:3] = up.add(out[p][3][:3], up.sub(up.pos(thug_bind[b]), up.pos(thug_bind[p])))
        out[b] = m
    return out


def put_bind(mesh, bind):
    _, at = mesh_bones(mesh)
    out = bytearray(mesh)
    for i, m in enumerate(bind):
        struct.pack_into('<16f', out, at + i * 64, *[x for row in m for x in row])
    return bytes(out)


def rest_translation(bind, b):
    p = BONE_PARENT[b]
    d = up.sub(up.pos(bind[b]), up.pos(bind[p]))
    return up.vrot(d, up.tr3(up.rot3(bind[p])))


def edit_entity(text, bind):
    """Rewrite each bone node's po translation for the new bind."""
    nodes = [m.start() for m in re.finditer(r'\bnode\s+"', text)] + [len(text)]
    out, pos, done = [], 0, set()
    for a, z in zip(nodes, nodes[1:]):
        seg = text[a:z]
        po = re.search(r'\bpo\s+((?:\S+\s+){11}\S+)', seg)
        bone = re.search(r'\bentity\s+bone\s+(\d+)', seg)
        if not po or not bone or int(bone.group(1)) == 0:
            continue
        b = int(bone.group(1))
        nums = po.group(1).split()
        t = rest_translation(bind, b)
        nums[3], nums[7], nums[11] = ('%f' % t[0], '%f' % t[1], '%f' % t[2])
        out.append(text[pos:a + po.start(1)])
        out.append(' '.join(nums))
        pos = a + po.end(1)
        done.add(b)
    out.append(text[pos:])
    if done != set(range(1, len(bind))):
        raise ValueError(f'entity bones not all rewritten: {sorted(set(range(1, len(bind))) - done)}')
    return ''.join(out)


class Anmx:
    def __init__(self, data):
        self.d = data
        self.n = struct.unpack_from('<I', data, 0x14)[0]
        self.child, self.sib, self.data = [], [], []
        for t in range(self.n):
            c, s, o = struct.unpack_from('<3I', data, 0x20 + t * 0x40 + 0x20)
            self.child.append(c)
            self.sib.append(s)
            self.data.append(o)
        self.parent = [-1] * self.n
        self.order = []

        def dfs(t, p):
            self.parent[t] = p
            self.order.append(t)
            c = self.child[t]
            while c:
                dfs(c, t)
                c = self.sib[c]
        dfs(0, -1)
        self.duration = struct.unpack_from('<f', data, 0x10)[0]

    def block(self, t, k):
        at = struct.unpack_from('<I', self.d, self.data[t] + 8 + 4 * k)[0]
        if not at:
            return []
        n = struct.unpack_from('<I', self.d, at)[0]
        size = (16, 12, 8)[k]
        return [self.d[at + 8 + i * size: at + 8 + (i + 1) * size] for i in range(n)]

    def rot(self, t, i=0):
        keys = self.block(t, 1)
        if not keys:
            return None
        w, x, y, z = (v / 32767 for v in struct.unpack_from('<4h', keys[min(i, len(keys) - 1)], 4))
        return [-x, -y, -z, w]

    def pos(self, t, i=0):
        keys = self.block(t, 0)
        return list(struct.unpack_from('<3f', keys[min(i, len(keys) - 1)], 4)) if keys else None


def build_anmx(template, duration, tracks, extra=(), strikes=()):
    """tracks: {t: dict(pos=[(time, xyz)], rot=[(time, q xyzw)])}; others keep
    the template's first key. Root's scale block is kept as constant 1.
    extra: new tracks [(name, parent track, last sibling track or None, spec)],
    a parent's first new child hung after its last existing child."""
    tp = Anmx(template)
    n_all = tp.n + len(extra)
    head = bytearray(template[:0x20 + tp.n * 0x40]) + bytearray(len(extra) * 0x40)
    struct.pack_into('<f', head, 0x10, duration)
    struct.pack_into('<I', head, 0x14, n_all)
    proto = template[0x20 + 13 * 0x40:0x20 + 14 * 0x40]      # a finger track
    for i, (name, par, after, spec) in enumerate(extra):
        t = tp.n + i
        d = bytearray(proto)
        d[0:32] = xbs.pack_name(name)
        struct.pack_into('<4I', d, 0x20, 0, 0, 0, 0)
        head[0x20 + t * 0x40:0x20 + (t + 1) * 0x40] = d
        if after is not None:          # after the parent's last child
            struct.pack_into('<I', head, 0x20 + after * 0x40 + 0x24, t)
        elif par is not None:
            struct.pack_into('<I', head, 0x20 + par * 0x40 + 0x20, t)
        else:                          # child of the previous new track
            struct.pack_into('<I', head, 0x20 + (t - 1) * 0x40 + 0x20, t)
        tracks[t] = spec
    body = bytearray()
    base = len(head)

    def here():
        return base + len(body)

    for t in range(n_all):
        spec = tracks.get(t)
        src = tp.data[t] if t < tp.n else tp.data[13]
        flags = struct.unpack_from('<I', template, src + 4)[0]
        has_scale = struct.unpack_from('<I', template, src + 16)[0] != 0 and t < tp.n
        if spec is None:
            q = tp.rot(t)
            p = tp.pos(t)
            spec = dict(rot=[(0.0, q), (duration, q)] if q else [],
                        pos=[(0.0, p), (duration, p)] if p else [])
        at = here()
        struct.pack_into('<I', head, 0x20 + t * 0x40 + 0x28, at)
        body.extend(bytes(0x14))
        ptr = [0, 0, 0]
        if spec.get('pos'):
            ptr[0] = here()
            body.extend(struct.pack('<2I', len(spec['pos']), 0))
            for time, xyz in spec['pos']:
                body.extend(struct.pack('<4f', time, *xyz))
        if spec.get('rot'):
            ptr[1] = here()
            body.extend(struct.pack('<2I', len(spec['rot']), 0))
            prev = None
            for time, q in spec['rot']:
                x, y, z, w = q
                k = [w, -x, -y, -z]
                if prev and sum(a * b for a, b in zip(prev, k)) < 0:
                    k = [-v for v in k]
                prev = k
                body.extend(struct.pack('<f4h', time, *(max(-32767, min(32767, round(v * 32767))) for v in k)))
        if has_scale:
            ptr[2] = here()
            body.extend(struct.pack('<2I', 2, 0) + struct.pack('<4f', 0.0, 1.0, duration, 1.0))
        struct.pack_into('<5I', body, at - base, struct.unpack('<I', struct.pack('<f', duration))[0],
                         flags, *ptr)
    # Event lists (descriptor +0x2c; the root's is the animation's): 0, 1,
    # count, 0, then (seconds, flag) pairs: 0x100 strike, 0x20 launch/fire,
    # 0x200/0x400 footsteps. Retimed to the new duration; negative times
    # (a convention of some looping anims) are kept.
    k = duration / tp.duration if tp.duration else 1.0
    for t in range(tp.n):
        ev = struct.unpack_from('<I', template, 0x20 + t * 0x40 + 0x2c)[0]
        if not ev:
            continue
        a, b, count, c = struct.unpack_from('<4I', template, ev)
        if (a, b, c) != (0, 1, 0) or count > 64:
            raise ValueError(f'unknown event list layout in track {t}')
        events = []
        for i in range(count):
            time, flag = struct.unpack_from('<fI', template, ev + 16 + 8 * i)
            if flag == 0x100 and strikes:
                continue                      # replaced by the sweep below
            events.append((time * k if time >= 0 else time, flag))
        if t == 0 and strikes:
            events += [(s, 0x100) for s in strikes]
            events.sort(key=lambda e: e[0])
        struct.pack_into('<I', head, 0x20 + t * 0x40 + 0x2c, here())
        body.extend(struct.pack('<4I', 0, 1, len(events), 0))
        for time, flag in events:
            body.extend(struct.pack('<fI', time, flag))
    return bytes(head + body)


def sample(keys, time, rot):
    """Linear (rotation: normalised lerp) sample of decoded keys at time."""
    if not keys:
        return None
    if time <= keys[0][0]:
        return keys[0][1]
    for (t0, a), (t1, b) in zip(keys, keys[1:]):
        if time <= t1:
            f = (time - t0) / (t1 - t0) if t1 > t0 else 0.0
            if rot and sum(x * y for x, y in zip(a, b)) < 0:
                b = [-x for x in b]
            v = [x + (y - x) * f for x, y in zip(a, b)]
            return up.norm(v) if rot else v
    return keys[-1][1]


def root_keys(tp):
    pos = [(struct.unpack_from('<f', k)[0], list(struct.unpack_from('<3f', k, 4))) for k in tp.block(0, 0)]
    rot = []
    for k in tp.block(0, 1):
        w, x, y, z = (v / 32767 for v in struct.unpack_from('<4h', k, 4))
        rot.append((struct.unpack_from('<f', k)[0], [-x, -y, -z, w]))
    return pos, rot


def fk_2002(anim, rest_t, i):
    """2002 world matrices at key i (template layout) for calibration."""
    W = {}
    for t in anim.order:
        q = anim.rot(t, i) or [0, 0, 0, 1]
        p = anim.pos(t, i) or rest_t.get(t, [0, 0, 0])
        L = up.local(q, p)
        W[t] = up.mul(L, W[anim.parent[t]]) if anim.parent[t] >= 0 else L
    return W


def strike_time(template, duration):
    tp = Anmx(template)
    ev = struct.unpack_from('<I', template, 0x4c)[0]
    if ev:
        count = struct.unpack_from('<I', template, ev + 8)[0]
        for i in range(count):
            t, flag = struct.unpack_from('<fI', template, ev + 16 + 8 * i)
            if flag & 0x100 and t >= 0:
                return t * duration / tp.duration
    return duration * 0.45


def convert(sk, frames, duration, template, thug_bind, new_bind, dy, tentacles=None,
            activity=None):
    """Venom's body over the template's root track (root motion, any root
    turn), retimed to Venom's duration."""
    tp = Anmx(template)
    rpos, rrot = root_keys(tp)
    k = tp.duration / duration if duration else 1.0
    n = len(frames[0])
    times = [duration * i / (n - 1) for i in range(n)]
    tracks = {t: dict(pos=[], rot=[]) for t in list(TRACK_USM) + [0]}
    frames_M = []
    for i in range(n):
        W = vp.full_pose(sk, frames, i)
        M = {}
        root = up.local(sample(rrot, times[i] * k, True) or [0, 0, 0, 1],
                        sample(rpos, times[i] * k, False) or [0, 0, 0])
        M[0] = root
        for t, u in TRACK_USM.items():
            b = TRACK_BONE[t]
            g = up.mul3(up.tr3(up.rot3(sk.bind[u])), up.rot3(W[u]))
            r = up.mul3(up.rot3(new_bind[b]), g)
            p = up.scale(up.pos(W[u]), SCALE)
            p[1] += dy
            # Venom's body is authored at the origin: carry it with the root.
            M[t] = up.mul([r[0] + [0.0], r[1] + [0.0], r[2] + [0.0], p + [1.0]], root)
        for t in tracks:
            par = tp.parent[t]
            L = M[t] if par < 0 else up.mul(M[t], up.inverse(M[par]))
            tracks[t]['rot'].append((times[i], up.mat_quat(up.rot3(L))))
            if t in POS_TRACKS:
                tracks[t]['pos'].append((times[i], L[3][:3]))
        frames_M.append(M)
    ts = strike_time(template, duration)
    # A strike tests its limbs at that instant; the limbs are the tentacle
    # tips, so one strike at full reach misses anything nearer. Strike all the
    # way out instead: the tip is tested along its whole path.
    strikes = ()
    if tentacles in ('strike', 'both') and vt.STRIKE_STEPS:
        out = vt.STRIKE_OUT
        strikes = [max(0.0, ts - out * (1 - j / vt.STRIKE_STEPS)) for j in range(1, vt.STRIKE_STEPS + 1)]
    data = bytearray(build_anmx(template, duration, tracks,
                                tentacle_tracks(frames_M, times, tentacles, ts, activity), strikes))
    return bytes(data)


def tentacle_tracks(frames_M, times, mode, t_strike, activity=None):
    """New tracks for both chains: collapsed unless the move uses them.
    activity: USM's own tentacle channel for the move, per frame the five
    slots' activity (usm_tentacle_channel.py); when given it times the lash:
    extension = the strongest slot / ACTIVE_FULL, and both tentacles come out
    where USM has two or more slots out at once."""
    spec = {}
    if activity and mode in ('strike', 'both') and max(max(a) for a in activity) > 0.05:
        curve = [min(1.0, max(a) / vt.ACTIVE_FULL) for a in activity]
        many = max(sum(1 for x in a if x > 0.05) for a in activity) >= 2
        i = max(range(len(times)), key=lambda k: curve[k])
        root_inv = up.inverse(frames_M[i][0])
        fwd = {s: up.vmul(up.pos(frames_M[i][vt.HAND_TRACK[s]]), root_inv)[2] for s in vt.HANDS}
        lead = max(fwd, key=fwd.get)
        for s in vt.HANDS:
            if many or mode == 'both' or s == lead:
                spec[s] = ('curve', curve)
    elif mode in ('strike', 'both'):
        # lead hand: the one further in front of him at the strike
        i = min(range(len(times)), key=lambda k: abs(times[k] - t_strike))
        root_inv = up.inverse(frames_M[i][0])
        fwd = {s: up.vmul(up.pos(frames_M[i][vt.HAND_TRACK[s]]), root_inv)[2] for s in vt.HANDS}
        lead = max(fwd, key=fwd.get)
        for s in vt.HANDS:
            if mode == 'both' or s == lead:
                spec[s] = ('strike', t_strike)
    elif mode:
        lead = 'r'
        spec[lead] = (mode, None)
    roots = [M[0] for M in frames_M]
    hands = {s: [M[vt.HAND_TRACK[s]] for M in frames_M] for s in vt.HANDS}
    # his forearm's direction each frame: what an attack's tentacle follows
    fore = {'l': 9, 'r': 28}
    arm_dirs = {s: [up.norm(up.sub(up.pos(M[vt.HAND_TRACK[s]]), up.pos(M[fore[s]])))
                    for M in frames_M] for s in vt.HANDS}
    # the floor: just under his lower foot that frame (feet: tracks 47, 51)
    ground = [min(M[47][3][1], M[51][3][1]) - 0.08 for M in frames_M]
    poses = vt.tracks_for(spec, None, times, roots, hands, arm_dirs, ground)
    extra = []
    for s in vt.HANDS:
        names = vt.bone_names(s)
        keys = [dict(pos=[], rot=[]) for _ in names]
        for i, t in enumerate(times):
            parent = hands[s][i]
            for k, W in enumerate(poses[s][i]):
                L = up.mul(W, up.inverse(parent))
                keys[k]['rot'].append((t, up.mat_quat(up.rot3(L))))
                keys[k]['pos'].append((t, L[3][:3]))
                parent = W
        for k, n in enumerate(names):
            if k == 0:
                extra.append((n, vt.HAND_TRACK[s], vt.LAST_FINGER_TRACK[s], keys[k]))
            else:
                extra.append((n, None, None, keys[k]))
    return extra
