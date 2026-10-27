"""Read Spider-Man (2002, Xbox) hero models (heroes/*.xbs) and render a preview.

    py -3 tools/xbxm.py <hero.xbs> [out.ppm] [yaw-degrees]
    py -3 tools/xbxm.py --export <game_files/data/heroes> previews

A hero .xbs is a 0x40-byte header and three zlib streams. Stream 0 is the
data: XBXM mesh records on 0x1000 boundaries (name at +0x30), then the
textures as plain DDS files. Stream 1 is the hero's entity text, stream 2 a
directory keyed by 16-byte name hashes.

XBXM record (offsets from the record):
  +0x00 'XBXM', version 0x304
  +0x6C bone count, +0x70 offset of the bones (4x4 floats each)
  +0x74 sub-mesh count, +0x78 offset of the sub-meshes (0x38 bytes each):
        +0x00 vertex buffer header offset, +0x04 first index, +0x08 index
        count, +0x0C material offset (texture name at +0x18 of it),
        +0x24 bone palette count, +0x28 bone palette offset
  vertex buffer header: +0x00 vertex data offset, +0x10 vertex count,
        +0x14 index list offset: that many uint16, the count in the dword
        just before it
  indices: one triangle strip, sub-meshes joined by repeated indices
  vertex, 0x2C bytes: position 3f, colour, packed normal, uv 2f, 3 dwords,
        4 palette indices (bytes), 4 weights (bytes)

Positions are the bind pose in model space, so a still preview needs no
skinning. The texture is not named in the file in any way this reads yet
(stream 2 hashes the names); the hero's body texture is the largest DXT1.
"""
import math
import re
import struct
import sys
import zlib


def streams(path):
    d = open(path, "rb").read()
    out, i = [], 0x40
    while i < len(d):
        o = zlib.decompressobj()
        out.append(o.decompress(d[i:]))
        i = len(d) - len(o.unused_data)
        while i < len(d) and d[i] != 0x78:
            i += 1
    return out


def meshes(s0):
    """(name, record offset) of every XBXM record."""
    res = []
    for off in range(0, len(s0), 0x1000):
        if s0[off:off + 4] == b"XBXM":
            name = s0[off + 0x30:off + 0x50].split(b"\0")[0].decode("latin1")
            res.append((name, off))
    return res


def hero_mesh(s0, path):
    """The hero's own mesh: aux_xbox.txt (beside the file) names each hero and
    its file, and the mesh record is the hero's name, lower case, + "000"."""
    import os
    aux = os.path.join(os.path.dirname(path), "aux_xbox.txt")
    base = os.path.splitext(os.path.basename(path))[0].upper()
    want = None
    for line in open(aux, errors="replace"):
        f = line.strip().split(",")
        if len(f) > 1 and f[1].upper() == base:
            want = f[0].lower()
    for name, off in meshes(s0):        # "<hero>000", or FREAKSPI's plain name
        if name in (want + "000", want):
            return name, off
    raise SystemExit("no mesh %s in %s" % (want, path))


def read_mesh(s0, r):
    u32 = lambda o: struct.unpack_from("<I", s0, r + o)[0]
    nsub, subs = u32(0x74), u32(0x78)
    vbh = struct.unpack_from("<I", s0, r + subs)[0]
    vdata, nvert, ilist = u32(vbh), u32(vbh + 0x10), u32(vbh + 0x14)
    nidx = u32(ilist - 4)
    idx = struct.unpack_from("<%dH" % nidx, s0, r + ilist)
    verts = []
    for k in range(nvert):
        o = r + vdata + k * 0x2C
        x, y, z = struct.unpack_from("<3f", s0, o)
        uu, vv = struct.unpack_from("<2f", s0, o + 0x14)
        verts.append((x, y, z, uu, vv))
    parts = []
    for s in range(nsub):
        o = subs + s * 0x38
        first, count, mat = struct.unpack_from("<3I", s0, r + o + 4)
        tex = s0[r + mat + 0x18:r + mat + 0x38].split(b"\0")[0].decode("latin1")
        parts.append((first, count, tex))
    return verts, idx, parts


def triangles(idx, first, count):
    for i in range(first, first + count - 2):
        a, b, c = idx[i], idx[i + 1], idx[i + 2]
        if a == b or b == c or a == c:
            continue
        yield (a, b, c) if (i - first) % 2 == 0 else (b, a, c)


def pack_name(name):
    """A file name as stream 2's 32-byte key: 10-character chunks, each a
    64-bit number = length + 16 * the characters as base-64 digits, first
    character highest, short chunks padded with 0. Characters: '1'-'9'
    1-9, '0' 10, '_' 12, 'a'-'z' 16-41, '.' 60."""
    def code(c):
        c = c.lower()
        if "1" <= c <= "9":
            return ord(c) - 48
        if c == "0":
            return 10
        if c == "_":
            return 12
        if "a" <= c <= "z":
            return ord(c) - 81
        if c == ".":
            return 60
        raise ValueError("no code for %r in %r" % (c, name))
    out = b""
    for i in range(0, 40, 10):
        part = name[i:i + 10]
        v = 0
        for j in range(10):
            v = v * 64 + (code(part[j]) if j < len(part) else 0)
        out += ((len(part) + 16 * v) & (2 ** 64 - 1)).to_bytes(8, "little") if part else bytes(8)
    return out[:32]


def directory(s2):
    """key -> (offset, size) in stream 0, from stream 2's 64-byte entries."""
    d = {}
    for i in range(len(s2) // 64):
        e = s2[i * 64:i * 64 + 64]
        if any(e):
            off, size = struct.unpack_from("<II", e, 0x20)
            d[e[:32]] = (off, size)
    return d


def texture_offset(s0, s2, name):
    ent = directory(s2).get(pack_name(name + ".dds"))
    return ent[0] if ent and s0[ent[0]:ent[0] + 4] == b"DDS " else None


def dds_list(s0):
    res = []
    for m in re.finditer(rb"DDS \x7c\x00\x00\x00", s0):
        o = m.start()
        h, w = struct.unpack_from("<II", s0, o + 12)
        res.append((o, w, h, s0[o + 84:o + 88]))
    return res


def body_texture(s0):
    dxt1 = [t for t in dds_list(s0) if t[3] == b"DXT1"]
    return max(dxt1, key=lambda t: t[1] * t[2]) if dxt1 else None


def decode_dds(s0, o):
    """RGB rows of the top mip of the DXT1/DXT5 DDS at o (alpha dropped)."""
    h, w = struct.unpack_from("<II", s0, o + 12)
    dxt5 = s0[o + 84:o + 88] == b"DXT5"
    px = [[(0, 0, 0)] * w for _ in range(h)]
    p = o + 128
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            if dxt5:
                p += 8                  # the alpha block
            c0, c1, bits = struct.unpack_from("<HHI", s0, p)
            p += 8
            def rgb(c):
                return ((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)
            a, b = rgb(c0), rgb(c1)
            if c0 > c1 or dxt5:
                pal = [a, b, tuple((2 * x + y) // 3 for x, y in zip(a, b)),
                       tuple((x + 2 * y) // 3 for x, y in zip(a, b))]
            else:
                pal = [a, b, tuple((x + y) // 2 for x, y in zip(a, b)), (0, 0, 0)]
            for j in range(16):
                px[by + j // 4][bx + j % 4] = pal[bits >> (2 * j) & 3]
    return px


def render(verts, tris, tex, W=360, H=640, yaw=0.0):
    """tris: (a, b, c, texture) with texture a decode_dds() result or None."""
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    pts = []
    for x, y, z, u, v in verts:
        pts.append((x * cy + z * sy, y, -x * sy + z * cy, u, v))
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    span = max(max(xs) - min(xs), max(ys) - min(ys)) * 1.1
    cx, cyy = (max(xs) + min(xs)) / 2, (max(ys) + min(ys)) / 2
    s = min(W, H) / span
    scr = [(W / 2 + (p[0] - cx) * s, H / 2 - (p[1] - cyy) * s, p[2], p[3], p[4]) for p in pts]
    img = [[(40, 40, 48)] * W for _ in range(H)]
    zb = [[-1e9] * W for _ in range(H)]
    for a, b, c, tex in tris:
        th, tw = (len(tex), len(tex[0])) if tex else (1, 1)
        A, B, C = scr[a], scr[b], scr[c]
        x0 = max(int(min(A[0], B[0], C[0])), 0); x1 = min(int(max(A[0], B[0], C[0])) + 1, W)
        y0 = max(int(min(A[1], B[1], C[1])), 0); y1 = min(int(max(A[1], B[1], C[1])) + 1, H)
        den = (B[1] - C[1]) * (A[0] - C[0]) + (C[0] - B[0]) * (A[1] - C[1])
        if abs(den) < 1e-9:
            continue
        for py in range(y0, y1):
            for px in range(x0, x1):
                l0 = ((B[1] - C[1]) * (px - C[0]) + (C[0] - B[0]) * (py - C[1])) / den
                l1 = ((C[1] - A[1]) * (px - C[0]) + (A[0] - C[0]) * (py - C[1])) / den
                l2 = 1 - l0 - l1
                if l0 < 0 or l1 < 0 or l2 < 0:
                    continue
                z = l0 * A[2] + l1 * B[2] + l2 * C[2]
                if z <= zb[py][px]:
                    continue
                zb[py][px] = z
                u = (l0 * A[3] + l1 * B[3] + l2 * C[3]) % 1.0
                v = (l0 * A[4] + l1 * B[4] + l2 * C[4]) % 1.0
                img[py][px] = tex[int(v * th) % th][int(u * tw) % tw] if tex else (150, 150, 150)
    return img


def export(heroes_dir, out_dir):
    """previews/<FILE>.hpv for every hero, for the costume panel's 3D view
    (src/costume_panel.c reads them). Little-endian:
      'HPV1', nverts, ntris, nparts, ntex (u32)
      nverts x (x, y, z, u, v) f32       bind pose, model space
      ntris  x 3 u16                     triangle list, strip winding fixed
      nparts x (first tri, tri count, texture index or -1) u32/u32/i32
      ntex   x (w, h, fourcc, size) u32, then size bytes: the top mip, DXT1/5
    """
    import glob
    import os
    os.makedirs(out_dir, exist_ok=True)
    for path in sorted(glob.glob(os.path.join(heroes_dir, "*.xbs"))):
        s0, _, s2 = streams(path)
        name, r = hero_mesh(s0, path)
        verts, idx, parts = read_mesh(s0, r)
        tris, prts, texs, tex_of = [], [], [], {}
        for f, c, tname in parts:
            t = -1
            o = texture_offset(s0, s2, tname) if tname else None
            if o is not None:
                if o not in tex_of:
                    h, w = struct.unpack_from("<II", s0, o + 12)
                    four = s0[o + 84:o + 88]
                    size = (w // 4) * (h // 4) * (8 if four == b"DXT1" else 16)
                    tex_of[o] = len(texs)
                    texs.append((w, h, four, s0[o + 128:o + 128 + size]))
                t = tex_of[o]
            tri = list(triangles(idx, f, c))
            prts.append((len(tris), len(tri), t))
            tris += tri
        base = os.path.splitext(os.path.basename(path))[0].upper()
        with open(os.path.join(out_dir, base + ".hpv"), "wb") as fh:
            fh.write(b"HPV1" + struct.pack("<4I", len(verts), len(tris), len(prts), len(texs)))
            for v in verts:
                fh.write(struct.pack("<5f", *v))
            for t in tris:
                fh.write(struct.pack("<3H", *t))
            for p in prts:
                fh.write(struct.pack("<IIi", *p))
            for w, h, four, data in texs:
                fh.write(struct.pack("<II4sI", w, h, four, len(data)) + data)
        print("%-12s %-18s %5d verts %5d tris %d parts %d textures" %
              (base, name, len(verts), len(tris), len(prts), len(texs)))


def main():
    if sys.argv[1] == "--export":
        export(sys.argv[2], sys.argv[3])
        return
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "preview.ppm"
    yaw = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
    s0, _, s2 = streams(path)
    name, r = hero_mesh(s0, path)
    verts, idx, parts = read_mesh(s0, r)
    print(name, len(verts), "vertices", len(idx), "indices", parts)
    tris = []
    for f, c, tname in parts:
        o = texture_offset(s0, s2, tname) if tname else None
        print("  part %d+%d texture %r at %s" % (f, c, tname, hex(o) if o else None))
        tex = decode_dds(s0, o) if o else None
        tris += [t + (tex,) for t in triangles(idx, f, c)]
    img = render(verts, tris, None, yaw=yaw)
    with open(out, "wb") as f:
        f.write(b"P6 %d %d 255\n" % (len(img[0]), len(img)))
        for row in img:
            f.write(bytes(c for p in row for c in p))
    print("wrote", out)


if __name__ == "__main__":
    main()
