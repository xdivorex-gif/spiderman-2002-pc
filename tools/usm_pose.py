"""Matrix and quaternion helpers for USM -> 2002 animation retargeting.

Matrices are 4x4 row-major lists, row vectors (v' = v M), translation in
row 3: the convention of USM's bind matrices (PCM +0xa70, 60 bones) and of
the 2002 game's own mesh bones. Pure Python (no numpy on this machine).
"""
import math
import struct


def quat(xyz):
    x, y, z = xyz
    return [x, y, z, math.sqrt(max(0.0, 1 - x * x - y * y - z * z))]


def qmat(q):
    """Row-vector 3x3 rotation of quaternion (x, y, z, w)."""
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)],
            [2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)],
            [2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)]]


def mat_quat(r):
    """Inverse of qmat (r is 3x3 or 4x4, row-vector)."""
    # column-convention matrix c = r^T
    c = [[r[j][i] for j in range(3)] for i in range(3)]
    t = c[0][0] + c[1][1] + c[2][2]
    if t > 0:
        s = math.sqrt(t + 1) * 2
        q = [(c[2][1] - c[1][2]) / s, (c[0][2] - c[2][0]) / s, (c[1][0] - c[0][1]) / s, s / 4]
    elif c[0][0] > c[1][1] and c[0][0] > c[2][2]:
        s = math.sqrt(1 + c[0][0] - c[1][1] - c[2][2]) * 2
        q = [s / 4, (c[0][1] + c[1][0]) / s, (c[0][2] + c[2][0]) / s, (c[2][1] - c[1][2]) / s]
    elif c[1][1] > c[2][2]:
        s = math.sqrt(1 + c[1][1] - c[0][0] - c[2][2]) * 2
        q = [(c[0][1] + c[1][0]) / s, s / 4, (c[1][2] + c[2][1]) / s, (c[0][2] - c[2][0]) / s]
    else:
        s = math.sqrt(1 + c[2][2] - c[0][0] - c[1][1]) * 2
        q = [(c[0][2] + c[2][0]) / s, (c[1][2] + c[2][1]) / s, s / 4, (c[1][0] - c[0][1]) / s]
    n = math.sqrt(sum(v * v for v in q))
    return [v / n for v in q]


def local(q, t):
    r = qmat(q)
    return [r[0] + [0.0], r[1] + [0.0], r[2] + [0.0], list(t) + [1.0]]


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def rot3(m):
    return [row[:3] for row in m[:3]]


def mul3(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def tr3(a):
    return [[a[j][i] for j in range(3)] for i in range(3)]


def inverse(m):
    """Inverse of a rigid row-vector 4x4 (rotation + translation)."""
    r = tr3(rot3(m))
    t = m[3][:3]
    ti = [-sum(t[k] * r[k][j] for k in range(3)) for j in range(3)]
    return [r[0] + [0.0], r[1] + [0.0], r[2] + [0.0], ti + [1.0]]


def pos(m):
    return m[3][:3]


def vmul(v, m):
    """Point v (3) through row-vector 4x4 m."""
    return [sum(v[k] * m[k][j] for k in range(3)) + m[3][j] for j in range(3)]


def vrot(v, r):
    return [sum(v[k] * r[k][j] for k in range(3)) for j in range(3)]


def sub(a, b):
    return [x - y for x, y in zip(a, b)]


def add(a, b):
    return [x + y for x, y in zip(a, b)]


def scale(a, s):
    return [x * s for x in a]


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def norm(a):
    n = math.sqrt(dot(a, a)) or 1.0
    return [x / n for x in a]


def length(a):
    return math.sqrt(dot(a, a))


def bind_matrices(pcm):
    """USM bind world matrices, 60 bones, at PCM +0xa70."""
    out = []
    for i in range(60):
        f = struct.unpack_from('<16f', pcm, 0xa70 + i * 64)
        out.append([list(f[0:4]), list(f[4:8]), list(f[8:12]), list(f[12:16])])
    return out


def f3(d, at):
    return list(struct.unpack_from('<3f', d, at))


def i32(d, at):
    return struct.unpack_from('<i', d, at)[0]
