"""USM Venom pose reconstruction: decoded channel frames -> bone world matrices.

Follows USM.exe's own pose routines for the record layouts:
  torso FUN_005f6610 (metadata +0x110 of the PCSKEL), legs FUN_005f6da0
  (+0x190), arms FUN_005f7760 (+0x250). Channel values come from
  tools/usm_decode_channels.py (Codex), in group order.
Quaternions are applied conjugated (x,y,z negated) through usm_pose.qmat,
which is what makes the idle torso match the bind orientations.

Hands and feet are placed from their targets (pelvis space); knees and
elbows by a two-bone IK with the skeleton's lengths. USM's own solver
(FUN_005f16e0) is not reproduced: the swivel reference is ours.
"""
import math
import struct

import usm_pose as up

TORSO, LEGS, ARMS = 0x110, 0x190, 0x250
CHAIN = [1, 2, 3, 5, 6]          # torso quaternions 0..4 drive these
NECK, NECK_PARENT = 4, 3
PELVIS = 0
# limb: end bone, upper, lower, base offset (in parent), lengths, parent of base
LEG_BONES = [dict(foot=54, thigh=52, calf=53, toe=55), dict(foot=58, thigh=56, calf=57, toe=59)]
ARM_BONES = [dict(clav=8, upper=9, fore=10, hand=11), dict(clav=30, upper=31, fore=32, hand=33)]
SPINE2 = 3


def conj(q):
    return [-q[0], -q[1], -q[2], q[3]]


def rest_frame():
    return None


class Skeleton:
    def __init__(self, pcskel, pcm):
        s = pcskel
        self.bind = up.bind_matrices(pcm)
        self.torso_t = [up.f3(s, TORSO + 0x1c + 12 * k) for k in range(5)]
        self.neck_t = up.f3(s, TORSO + 0x10)
        self.toe_t = [up.f3(s, LEGS + 0x00), up.f3(s, LEGS + 0x0c)]
        self.hip_t = [up.f3(s, LEGS + 0x30), up.f3(s, LEGS + 0x48)]
        self.leg_len = [struct.unpack_from('<2f', s, LEGS + 0x60 + 0x10),
                        struct.unpack_from('<2f', s, LEGS + 0x78 + 0x10)]
        self.clav_t = [up.f3(s, ARMS + 0x00), up.f3(s, ARMS + 0x0c)]
        self.shoulder_t = [up.f3(s, ARMS + 0x30), up.f3(s, ARMS + 0x3c)]
        self.arm_len = [struct.unpack_from('<2f', s, ARMS + 0x90 + 0x10),
                        struct.unpack_from('<2f', s, ARMS + 0xa8 + 0x10)]
        ids = [up.i32(s, LEGS + 0x90 + 4 * k) for k in range(9)]
        assert ids == [55, 59, 54, 58, 52, 53, 56, 57, 0], ids
        ids = [up.i32(s, ARMS + 0xc0 + 4 * k) for k in range(13)]
        assert ids == [8, 30, 11, 33, 9, 31, 10, 32, 28, 29, 50, 51, 3], ids


def two_bone(s, t, l1, l2, pole):
    """Elbow/knee position for shoulder/hip s, target t, lengths, bend direction."""
    axis = up.sub(t, s)
    d = min(up.length(axis), (l1 + l2) * 0.9995)
    u = up.norm(axis)
    p = up.norm(up.sub(pole, up.scale(u, up.dot(pole, u))))
    a = (l1 * l1 - l2 * l2 + d * d) / (2 * d)
    h = math.sqrt(max(0.0, l1 * l1 - a * a))
    return up.add(up.add(s, up.scale(u, a)), up.scale(p, h))


def rotate_about(v, axis, angle):
    u = up.norm(axis)
    c, s = math.cos(angle), math.sin(angle)
    return up.add(up.add(up.scale(v, c), up.scale(up.cross(u, v), s)),
                  up.scale(u, up.dot(u, v) * (1 - c)))


def frame_rows(direction, pole):
    x = up.norm(direction)
    y = up.norm(up.sub(pole, up.scale(x, up.dot(pole, x))))
    z = up.cross(x, y)
    return [x, y, z]


def aim(bind, joint_b, child_b, pole_b, joint, child, pole):
    """World matrix: bind rotation carried by the rotation that takes the bind
    (direction, pole) frame to the current one; translation at joint."""
    fb = frame_rows(up.sub(child_b, joint_b), pole_b)
    f = frame_rows(up.sub(child, joint), pole)
    g = up.mul3(up.tr3(fb), f)                       # v . g maps fb -> f
    r = up.mul3(up.rot3(bind), g)
    return [r[0] + [0.0], r[1] + [0.0], r[2] + [0.0], list(joint) + [1.0]]


# Swivel: bend direction = reference rotated about the limb axis by the
# channel scalar. References are pelvis-space directions (pelvis bind rows:
# x down, y toward -x model (left), z back), picked so the idle bends read
# right: knees forward, elbows back and out.
KNEE_REF = [0.0, 0.0, -1.0]      # model +z (front) in pelvis space
ELBOW_REF = [0.0, 0.0, 1.0]      # model -z (behind)
SWIVEL_SIGN = [1.0, -1.0]


def pose(sk, frames, i):
    torso, legs, arms = frames[0][i], frames[1][i], frames[2][i]
    W = {}
    W[PELVIS] = up.local(conj(up.quat(torso[15:18])), torso[18:21])
    parent = PELVIS
    for k, b in enumerate(CHAIN):
        W[b] = up.mul(up.local(conj(up.quat(torso[3 * k:3 * k + 3])), sk.torso_t[k]), W[parent])
        parent = b
    W[NECK] = up.mul(up.local([0, 0, 0, 1], sk.neck_t), W[NECK_PARENT])
    pel = W[PELVIS]
    pel_r = up.rot3(pel)
    # legs: groups 0,1 toes; 2,3 foot targets (q, p, swivel)
    for side in (0, 1):
        g = legs[6 + 7 * side: 13 + 7 * side]
        lb = LEG_BONES[side]
        foot = up.mul(up.local(conj(up.quat(g[0:3])), g[3:6]), pel)
        hip = up.vmul(sk.hip_t[side], pel)
        axis = up.sub(up.pos(foot), hip)
        ref = up.vrot(KNEE_REF, pel_r)
        ref = rotate_about(ref, axis, SWIVEL_SIGN[side] * g[6])
        l1, l2 = sk.leg_len[side]
        knee = two_bone(hip, up.pos(foot), l1, l2, ref)
        bind = sk.bind
        fwd = [0.0, 0.0, 1.0]
        bend_b = fwd
        bend = up.sub(knee, up.add(hip, up.scale(up.norm(axis), up.dot(up.sub(knee, hip), up.norm(axis)))))
        W[lb['thigh']] = aim(bind[lb['thigh']], up.pos(bind[lb['thigh']]), up.pos(bind[lb['calf']]), bend_b,
                             hip, knee, bend)
        W[lb['calf']] = aim(bind[lb['calf']], up.pos(bind[lb['calf']]), up.pos(bind[lb['foot']]), bend_b,
                            knee, up.pos(foot), bend)
        W[lb['foot']] = foot
        W[lb['toe']] = up.mul(up.local(conj(up.quat(legs[3 * side:3 * side + 3])), sk.toe_t[side]), foot)
    # arms: groups 0,1 clavicles (spine2 space); 2,3 hand targets (pelvis space)
    for side in (0, 1):
        g = arms[6 + 7 * side: 13 + 7 * side]
        ab = ARM_BONES[side]
        clav = up.mul(up.local(conj(up.quat(arms[3 * side:3 * side + 3])), sk.clav_t[side]), W[SPINE2])
        hand = up.mul(up.local(conj(up.quat(g[0:3])), g[3:6]), pel)
        sh = up.vmul(sk.shoulder_t[side], clav)
        axis = up.sub(up.pos(hand), sh)
        ref = up.vrot(ELBOW_REF, pel_r)
        ref = rotate_about(ref, axis, SWIVEL_SIGN[side] * g[6])
        l1, l2 = sk.arm_len[side]
        elbow = two_bone(sh, up.pos(hand), l1, l2, ref)
        bind = sk.bind
        back = [0.0, 0.0, -1.0]
        bend = up.sub(elbow, up.add(sh, up.scale(up.norm(axis), up.dot(up.sub(elbow, sh), up.norm(axis)))))
        W[ab['clav']] = clav
        W[ab['upper']] = aim(bind[ab['upper']], up.pos(bind[ab['upper']]), up.pos(bind[ab['fore']]), back,
                             sh, elbow, bend)
        W[ab['fore']] = aim(bind[ab['fore']], up.pos(bind[ab['fore']]), up.pos(bind[ab['hand']]), back,
                            elbow, up.pos(hand), bend)
        W[ab['hand']] = hand
    return W


# Bones the channels don't drive ride rigidly on one that is driven.
RIGID = {5: 4, 7: 6}
for b in range(12, 28):
    RIGID[b] = 11
RIGID.update({28: 9, 29: 10})
for b in range(34, 50):
    RIGID[b] = 33
RIGID.update({50: 31, 51: 32})


def full_pose(sk, frames, i):
    W = pose(sk, frames, i)
    for b, p in RIGID.items():
        if b not in W:
            W[b] = up.mul(up.mul(sk.bind[b], up.inverse(sk.bind[p])), W[p])
    return W
