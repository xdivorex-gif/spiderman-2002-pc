"""Venom's tentacles for the 2002 slot.

USM draws them procedurally: "tentacle engines" (strike, swirl strike, slack,
follow, dangle) scripted per move, rendered as generated lines
(USM.exe strings: tentacle_add_strike_engine, tentacle_create_line,
"Tentacle Base Diameter"). The 2002 engine has none of that, so here they
are rig data: a chain of SEGMENTS bones on each hand with a tapered tube
skinned to it.

  rest pose (entity po): every segment collapsed into the hand, so any
    animation without tentacle tracks (Spider-Man's swings...) hides them;
  bind (mesh): extended along the hand's local -x (its finger axis);
  tentacle moves: tracks extend the lead hand's tentacle toward a point in
    front of him, reaching full length on the move's strike event; zips
    shoot it up / forward and hold it through the flight.

Track names are packed animids (xbs.pack_name), which is how ANMX tracks bind
to entity nodes.
"""
import math

import usm_pose as up
import xbs

SEGMENTS = 8
LENGTH = 3.6                  # slot units, extended (his arm is ~0.65); USM-like reach
SEG = LENGTH / SEGMENTS
COLLAPSED = 0.002             # segment length when hidden
RADIUS0, RADIUS1 = 0.055, 0.007
SIDES = 7
HANDS = {'l': 13, 'r': 18}    # slot mesh bones
HAND_TRACK = {'l': 10, 'r': 29}
LAST_FINGER_TRACK = {'l': 23, 'r': 42}


# Tried 2026-11-05 and reverted: naming the tips L_HAND / R_HAND (real hands
# L_HAND0 / R_HAND0) gave no extra hit reach in play and lifted him off the
# ground when attacking (the game moves him to bring that limb to the
# target). Several strike events along the lash didn't add reach either. The
# hit test is not limb-position based as far as play shows; not yet traced.
HAND_ANIMID = {'l': ('L_HAND', 'L_HAND'), 'r': ('R_HAND', 'R_HAND')}


def bone_names(side):
    return [f'{side}_tent{k + 1}' for k in range(SEGMENTS)]


def hand_track_name(side):
    return HAND_ANIMID[side][1].lower()


def add_bones(bind):
    """Extended bind matrices for both chains, appended after the slot's bones.
    Returns (bind list, {bone: parent}, {bone: (side, k)})."""
    out = [m for m in bind]
    parent, which = {}, {}
    for side, hand in HANDS.items():
        h = bind[hand]
        axis = up.scale(up.norm(h[0][:3]), -1.0)      # finger direction
        prev = hand
        for k in range(SEGMENTS):
            m = [row[:] for row in h]
            m[3][:3] = up.add(up.pos(h), up.scale(axis, SEG * (k + 1)))
            b = len(out)
            out.append(m)
            parent[b] = prev
            which[b] = (side, k)
            prev = b
    return out, parent, which


def geometry(bind, which, scale):
    """Tapered tubes skinned to the chains, as a USM-style section list:
    (material, 4, triangle indices, [(pos/scale, normal, uv, [(bone, w)])]).
    Triangles wound like USM's (clockwise to the outward normal); the build
    flips every section for this engine."""
    sections = []
    for side, hand in HANDS.items():
        chain = [hand] + [b for b, (s, k) in sorted(which.items()) if s == side]
        h = bind[hand]
        axis = up.scale(up.norm(h[0][:3]), -1.0)
        u = up.norm(h[1][:3])
        v = up.cross(axis, u)
        verts, idx = [], []
        rings = []
        for j, b in enumerate(chain):
            r = RADIUS0 + (RADIUS1 - RADIUS0) * j / (len(chain) - 1)
            c = up.pos(bind[b])
            ring = []
            for s in range(SIDES):
                a = 2 * math.pi * s / SIDES
                n = up.add(up.scale(u, math.cos(a)), up.scale(v, math.sin(a)))
                p = up.add(c, up.scale(n, r))
                skin = [(b, 1.0)] if j else [(hand, 1.0)]
                ring.append(len(verts))
                verts.append((tuple(x / scale for x in p), tuple(n), (0.5, 0.5), skin))
            rings.append(ring)
        tip = len(verts)
        verts.append((tuple(x / scale for x in up.add(up.pos(bind[chain[-1]]), up.scale(axis, SEG * 0.6))),
                      tuple(axis), (0.5, 0.5), [(chain[-1], 1.0)]))
        for a, b in zip(rings, rings[1:]):
            for s in range(SIDES):
                p0, p1 = a[s], a[(s + 1) % SIDES]
                q0, q1 = b[s], b[(s + 1) % SIDES]
                idx += [p0, p1, q0, p1, q1, q0]
        last = rings[-1]
        for s in range(SIDES):
            idx += [last[s], last[(s + 1) % SIDES], tip]
        # wind clockwise to the outward normal, as USM does
        fixed = []
        for i in range(0, len(idx), 3):
            a, b, c = idx[i:i + 3]
            A, B, C = (verts[k][0] for k in (a, b, c))
            n = up.cross(up.sub(B, A), up.sub(C, A))
            out = [sum(verts[k][1][d] for k in (a, b, c)) for d in range(3)]
            fixed += [a, b, c] if up.dot(n, out) < 0 else [a, c, b]
        sections.append((None, 4, fixed, verts))
    return sections


def entity_nodes(side, first_bone):
    """Entity text for one chain (nested), collapsed rest translations."""
    names = bone_names(side)
    out = []
    for k, n in enumerate(names):
        out.append('node\r\n"VENOM %s TENTACLE%d"\r\nanimid\r\n"%s"\r\npo\r\n'
                   '1.000000 0.000000 0.000000 %f 0.000000 1.000000 0.000000 0.000000 '
                   '0.000000 0.000000 1.000000 0.000000 \r\nentity\r\nbone\r\n%d\r\nchunkend\r\nchunkend\r\n'
                   % (side.upper(), k + 1, n.upper(), -COLLAPSED, first_bone + k))
    out.append('chunkend\r\n' * SEGMENTS)
    return ''.join(out)


def add_to_entity(text):
    """Insert both chains inside the hand nodes, after the finger node."""
    for side, first in (('l', 28), ('r', 28 + SEGMENTS)):
        key = '"BIP01 %s FINGER0"' % side.upper()
        at = text.index(key)
        # the finger node ends after its bone and entity chunks and its own close
        end = at
        for _ in range(3):
            end = text.index('chunkend', end) + len('chunkend')
        end = text.index('\n', end) + 1
        text = text[:end] + entity_nodes(side, first) + text[end:]
    return text


# --- animation ---------------------------------------------------------------

STRIKE_OUT = 0.12            # seconds from hidden to full reach
ACTIVE_FULL = 0.45           # USM tentacle activity counted as full reach
STRIKE_STEPS = 0             # strike sweep off: it didn't add reach in play


def extension(t, t_strike, out_time=STRIKE_OUT, hold=0.16, back=0.24):
    if t < t_strike - out_time or t > t_strike + hold + back:
        return 0.0
    if t < t_strike:
        return (t - (t_strike - out_time)) / out_time
    if t < t_strike + hold:
        return 1.0
    return 1.0 - (t - t_strike - hold) / back


# Following his arm: in an attack each segment points the way his forearm
# pointed a little earlier (TRAIL_FRAMES per segment, at 30 frames/s), pulled
# toward the target by AIM; so a sweep swings the tentacle round after the arm
# and curls it, as USM's tentacles follow his blows, and a lash still reaches.
TRAIL_FRAMES = 0.8
AIM = 0.35


def trail_pose(hand_w, arm_dirs, i, target, e, ground=None):
    """World matrices for one chain trailing the arm's recent directions."""
    base = up.pos(hand_w)
    d_t = up.norm(up.sub(target, base))
    p = base
    seg = max(COLLAPSED, SEG * e)
    out = []
    for k in range(SEGMENTS):
        lag = (k + 1) * TRAIL_FRAMES
        j0 = max(0, int(i - lag))
        j1 = max(0, j0 - 1)
        f = (i - lag) - int(i - lag) if i - lag > 0 else 0.0
        a = up.norm(up.add(up.scale(arm_dirs[j0], 1 - f), up.scale(arm_dirs[j1], f)))
        d = up.norm(up.add(up.scale(a, 1 - AIM), up.scale(d_t, AIM)))
        q = up.add(p, up.scale(d, seg))
        if ground is not None and q[1] < ground:     # skim the floor, not through it
            flat = [d[0], 0.0, d[2]]
            d = up.norm(flat) if up.length(flat) > 1e-6 else d
            q = up.add(p, up.scale(d, seg))
            q[1] = max(q[1], ground)
            d = up.norm(up.sub(q, p)) if up.length(up.sub(q, p)) > 1e-6 else d
        p = q
        out.append((p, d))
    mats = []
    for (p, d) in out:
        x = up.scale(d, -1.0)
        y0 = hand_w[1][:3]
        y = up.norm(up.sub(y0, up.scale(x, up.dot(y0, x))))
        if up.length(y) < 1e-6:
            y = up.norm(up.cross(x, [0.0, 1.0, 0.0]))
        z = up.cross(x, y)
        mats.append([x + [0.0], y + [0.0], z + [0.0], p + [1.0]])
    return mats


def chain_pose(hand_w, target, e, phase):
    """World matrices for one chain from the hand toward target, extended e."""
    out = []
    h_axis = up.scale(up.norm(hand_w[0][:3]), -1.0)
    base = up.pos(hand_w)
    d_t = up.norm(up.sub(target, base))
    side_v = up.norm(up.cross(d_t, [0.0, 1.0, 0.0])) if abs(d_t[1]) < 0.95 else [1.0, 0.0, 0.0]
    p = base
    seg = max(COLLAPSED, SEG * e)
    for k in range(SEGMENTS):
        f = (k + 1) / SEGMENTS
        d = up.norm(up.add(up.scale(h_axis, 0.15 * (1 - f) ** 2), d_t))   # true to the target
        wave = (1 - e) * 0.6 * math.sin(phase + f * 4.0)
        d = up.norm(up.add(d, up.scale(side_v, wave)))
        p = up.add(p, up.scale(d, seg))
        out.append((p, d))
    mats = []
    for (p, d) in out:
        x = up.scale(d, -1.0)                                  # local -x along the tentacle
        y0 = hand_w[1][:3]
        y = up.norm(up.sub(y0, up.scale(x, up.dot(y0, x))))
        z = up.cross(x, y)
        mats.append([x + [0.0], y + [0.0], z + [0.0], p + [1.0]])
    return mats


def tracks_for(spec, M, times, root_of, hands_w, arm_dirs=None, ground=None):
    """Per-frame world matrices of the 2*SEGMENTS tentacle bones.
    spec: dict(side: (mode, t_strike or None)) where mode is 'strike', 'up',
    'forward' (zip launch: extend over the move), 'hold_up'/'hold_forward'
    (flight), 'retract' (landing)."""
    result = {s: [] for s in HANDS}
    for i, t in enumerate(times):
        root = root_of[i]
        for side in HANDS:
            mode, ts = spec.get(side, (None, None))
            dur = times[-1] or 1.0
            if mode == 'curve':                 # USM's own activity (ts = curve)
                e = ts[i]
                local_target = [0.0, -0.35, 4.0]
            elif mode == 'strike':
                e = extension(t, ts)
                local_target = [0.0, -0.35, 4.0]       # chest height, well out in front
            elif mode in ('up', 'forward'):
                e = min(1.0, max(0.0, (t / dur - 0.35) / 0.4))
                local_target = [0.0, 4.0, 0.8] if mode == 'up' else [0.0, 1.2, 4.0]
            elif mode in ('hold_up', 'hold_forward'):
                e = 1.0
                local_target = [0.0, 4.0, 0.8] if mode == 'hold_up' else [0.0, 1.2, 4.0]
            elif mode == 'retract':
                e = max(0.0, 1.0 - t / (0.35 * dur))
                local_target = [0.0, 1.5, 1.5]
            else:
                e, local_target = 0.0, [0.0, 0.0, 1.0]
            target = up.vmul(local_target, root)
            if arm_dirs and mode in ('curve', 'strike'):
                result[side].append(trail_pose(hands_w[side][i], arm_dirs[side], i, target, e,
                                               ground[i] if ground else None))
            else:
                result[side].append(chain_pose(hands_w[side][i], target, e, t * 9.0))
    return result
