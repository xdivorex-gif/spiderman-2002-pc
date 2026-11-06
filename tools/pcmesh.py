"""Convert Xbox .xbmesh (XBXM 0x304) to the 2002 Windows release's .pcmesh
(PCXM 0x202), so Xbox-only levels load in SpiderMan.exe.

    py -3 tools/pcmesh.py check <pc.pkg> <xbox.xbs>   convert every mesh the two
                                                      share and compare to the PC one

Both files are a 16-byte header ('XBXM'/'PCXM', version, mesh count, and on
PC the offset of the vertex data) and then one record per mesh. Offsets are
from the start of the file.

Xbox record (R):
  +0x10 flags   +0x1C record size   +0x20 name[0x20]
  +0x40 sphere centre x,y,z,1   +0x50 radius
  +0x5C bone count  +0x60 bones (4x4 floats, the same values on PC)
  +0x64 sub-mesh count  +0x68 sub-meshes (0x38 each)  +0x6C vertex stride
  sub-mesh: +0x00 vertex buffer header, +0x04 first index, +0x08 count,
            +0x0C material (0xE0 bytes), +0x24 palette size, +0x28 palette
            (u16 bone numbers; a vertex's bone bytes index this)
  vertex buffer header: +0x00 vertices, +0x10 count, +0x14 u16 strip,
          +0x24 strip length (meshes may share one buffer)
  vertex: pos 3f, colour, packed normal (11:11:10), uv 2f, uv2 2f
          [+ 4 bone bytes, 4 weight bytes when the stride is 0x2C]

PC record (r), 0xF4 bytes + 0x100 per sub-mesh:
  +0x04 position count  +0x08 positions   (pos 3f, normal 3f; skinned:
        + 4 u16 bone numbers, 4 f32 weights = 0x30 bytes)
  +0x0C attribute count +0x10 attributes  (colour, uv 2f, uv2 2f, 8 zero)
  +0x14 index count     +0x18 indices     (u16 position, u16 attribute:
        one triangle strip, sub-meshes back to back)
  +0x1C triangles in the strip  +0x20 indices + 12 * triangles (unused)
  +0x24 flags  +0x2C record size  +0x30 name[0x20]
  +0x50 sphere centre x,y,z,1  +0x60 radius
  +0x64 bounding box: 8 corners x,y,z,1 (x min/max fastest, then z
        max/min, then y max/min)
  +0xE4 bone count  +0xE8 bones  +0xEC sub-mesh count  +0xF0 sub-meshes
  sub-mesh, 0x18 bytes: +0x00 (set at load), +0x04 index count,
        +0x08 material (0xE8 bytes: the Xbox 0xE0 + 8 zero; +0x04..+0x14
        are texture handles filled in at load)
"""
import math
import struct
import sys

u32 = lambda b, o: struct.unpack_from('<I', b, o)[0]

# Optional material rewrite (callable: material bytes -> material bytes),
# set by a caller such as pc_zoo.py; None leaves materials as on the Xbox.
MAT_FIX = None


def unpack_normal(n):
    x = n & 0x7FF
    y = (n >> 11) & 0x7FF
    z = n >> 22
    x = (x - 2048 if x >= 1024 else x) / 1023.0
    y = (y - 2048 if y >= 1024 else y) / 1023.0
    z = (z - 1024 if z >= 512 else z) / 511.0
    l = math.sqrt(x * x + y * y + z * z) or 1.0
    return x / l, y / l, z / l


def read_xbox(b):
    """[dict] per mesh record."""
    out = []
    R = 0x10
    for _ in range(u32(b, 8)):
        m = {
            'flags': u32(b, R + 0x10),
            'name': b[R + 0x20:R + 0x40],
            'sphere': b[R + 0x40:R + 0x54],
            'bones': [],
            'subs': [],
            'vbs': {},          # vertex buffer header -> (strip, vertices)
        }
        nb, bo = u32(b, R + 0x5C), u32(b, R + 0x60)
        m['bones'] = [b[bo + 64 * k:bo + 64 * k + 64] for k in range(nb)]
        ns, so, stride = u32(b, R + 0x64), u32(b, R + 0x68), u32(b, R + 0x6C)
        m['stride'] = stride
        for s in range(ns):
            o = so + 0x38 * s
            vbh, first, count, mat = struct.unpack_from('<4I', b, o)
            npal, pal = u32(b, o + 0x24), u32(b, o + 0x28)
            palette = list(struct.unpack_from('<%dH' % npal, b, pal)) if npal else []
            m['subs'].append({'vbh': vbh, 'first': first, 'count': count,
                              'mat': b[mat:mat + 0xE0], 'palette': palette})
            if vbh not in m['vbs']:
                vd, nv = u32(b, vbh), u32(b, vbh + 0x10)
                ni, il = u32(b, vbh + 0x24), u32(b, vbh + 0x14)
                m['vbs'][vbh] = (list(struct.unpack_from('<%dH' % ni, b, il)),
                                 [b[vd + stride * k:vd + stride * (k + 1)] for k in range(nv)])
        out.append(m)
        R += u32(b, R + 0x1C)
    return out


def strip_triangles(idx):
    n = 0
    for i in range(len(idx) - 2):
        a, c, d = idx[i], idx[i + 1], idx[i + 2]
        if a != c and c != d and a != d:
            n += 1
    return n


def convert(xb):
    """Xbox .xbmesh bytes -> PC .pcmesh bytes."""
    meshes = read_xbox(xb)
    recs, datas = [], []
    for m in meshes:
        skinned = bool(m['bones'])
        # Every strip entry names its own vertex in the sub-mesh that uses it;
        # skinned vertices need that sub-mesh's palette, so a vertex shared by
        # two palettes is split into one position per palette.
        # Positions and attributes are shared like the PC's own files: one
        # position per distinct (place, normal, bones, weights), one attribute
        # per distinct (colour, uv). Kraven came out with 1938 skinned
        # positions unshared, more than any PC character has (Scorpion 1419),
        # and drew with parts missing.
        pos, attr, pairs = [], [], []
        pos_of, attr_of = {}, {}
        seen = {}
        sub_counts = []
        for s in m['subs']:
            n = 0
            strip, verts = m['vbs'][s['vbh']]
            for k in range(s['first'], s['first'] + s['count']):
                vi = strip[k]
                key = (s['vbh'], vi, id(s) if skinned else 0)
                if key not in seen:
                    v = verts[vi]
                    x, y, z = struct.unpack_from('<3f', v, 0)
                    col = v[12:16]
                    nx, ny, nz = unpack_normal(u32(v, 16))
                    uv = v[20:36]
                    p = struct.pack('<6f', x, y, z, nx, ny, nz)
                    if skinned:
                        bi = v[36:40]
                        w = v[40:44]
                        pal = s['palette']
                        t = sum(w) or 1
                        infl = sorted(((w[j] / t, pal[c] if c < len(pal) else 0)
                                       for j, c in enumerate(bi) if w[j]), reverse=True)
                        infl += [(0.0, 0)] * (4 - len(infl))
                        p += struct.pack('<4H4f', *[b for _, b in infl], *[q for q, _ in infl])
                    a = col + uv + bytes(8)
                    if p not in pos_of:
                        pos_of[p] = len(pos)
                        pos.append(p)
                    if a not in attr_of:
                        attr_of[a] = len(attr)
                        attr.append(a)
                    seen[key] = (pos_of[p], attr_of[a])
                pairs.append(seen[key])
                n += 1
            sub_counts.append(n)
        if skinned:
            # The Xbox splits a character into one sub-mesh per bone palette;
            # the PC files keep one sub-mesh per material (Goblin, Mary Jane,
            # Shocker ...), and Kraven's face part did not draw while split.
            # Join neighbours with the same material into one strip, stitched
            # with repeated indices, padding so the second part keeps its
            # winding (strip triangles alternate).
            new_pairs, new_counts, new_subs = [], [], []
            k = 0
            for s, n in zip(m['subs'], sub_counts):
                part = pairs[k:k + n]
                k += n
                if new_subs and new_subs[-1]['mat'] == s['mat'] and new_pairs and part:
                    glue = [new_pairs[-1], part[0]]
                    if (new_counts[-1] + len(glue)) % 2:
                        glue.append(part[0])
                    new_pairs += glue + part
                    new_counts[-1] += len(glue) + len(part)
                else:
                    new_subs.append(s)
                    new_pairs += part
                    new_counts.append(len(part))
            m = dict(m, subs=new_subs)
            pairs, sub_counts = new_pairs, new_counts
        recs.append((m, pos, attr, pairs, sub_counts, skinned))

    head = 0x10
    rec_size = [0xF4 + 0x100 * len(m['subs']) for m, *_ in recs]
    for m, *_ in recs:
        if m['bones']:
            pass
    # bones live inside the record area, before the sub-mesh tables
    bone_size = [64 * len(m['bones']) for m, *_ in recs]
    rec_off = []
    at = head
    for i in range(len(recs)):
        rec_off.append(at)
        at += rec_size[i] + bone_size[i]
    data_start = at
    out = bytearray(data_start)
    out[0:16] = b'PCXM' + struct.pack('<3I', 0x202, len(recs), data_start)
    data = bytearray()
    for i, (m, pos, attr, pairs, sub_counts, skinned) in enumerate(recs):
        r = rec_off[i]
        po = data_start + len(data); data += b''.join(pos)
        ao = data_start + len(data); data += b''.join(attr)
        io = data_start + len(data); data += b''.join(struct.pack('<2H', *p) for p in pairs)
        strip = [p[0] for p in pairs]
        ntri = 0
        k = 0
        for c in sub_counts:
            ntri += strip_triangles(strip[k:k + c])
            k += c
        size = rec_size[i] + bone_size[i]
        struct.pack_into('<12I', out, r, 0, len(pos), po if pos else 0, len(attr), ao if attr else 0,
                         len(pairs), io if pairs else 0, ntri, io + 12 * ntri if pairs else 0,
                         m['flags'], 0, size)
        out[r + 0x30:r + 0x50] = m['name']
        if pos:
            out[r + 0x50:r + 0x64] = m['sphere']
            xs = [struct.unpack_from('<3f', p, 0) for p in pos]
            lo = [min(c[j] for c in xs) for j in range(3)]
            hi = [max(c[j] for c in xs) for j in range(3)]
            for k in range(8):
                x = hi[0] if k & 1 else lo[0]
                z = lo[2] if k & 2 else hi[2]
                y = lo[1] if k & 4 else hi[1]
                struct.pack_into('<4f', out, r + 0x64 + 16 * k, x, y, z, 1.0)
        # An empty record (every region has one beside its geometry) keeps an
        # all-zero sphere and box, as the PC's own files do. The Xbox writes a
        # sphere of radius 1.7e32 there; squared that overflows to infinity,
        # and the PC's region maths then gave Spider-Man a NaN height, inside
        # every trigger (all traps firing, the level ending at once).
        bo = r + 0xF4
        so = bo + bone_size[i]
        struct.pack_into('<4I', out, r + 0xE4, len(m['bones']), bo if m['bones'] else 0,
                         len(m['subs']), so)
        for k, bone in enumerate(m['bones']):
            out[bo + 64 * k:bo + 64 * k + 64] = bone
        mo = so + 0x18 * len(m['subs'])
        for k, s in enumerate(m['subs']):
            mat = bytearray(s['mat']) + bytes(8)
            if MAT_FIX:
                mat = bytearray(MAT_FIX(bytes(mat)))
            mat[4:0x14] = bytes(0x10)
            struct.pack_into('<6I', out, so + 0x18 * k, 0, sub_counts[k], mo + 0xE8 * k, 0, 0, 0)
            out[mo + 0xE8 * k:mo + 0xE8 * (k + 1)] = mat
    return bytes(out + data)


def read_pc(b):
    out = []
    r = 0x10
    for _ in range(u32(b, 8)):
        h = struct.unpack_from('<12I', b, r)
        nb, bo, ns, so = struct.unpack_from('<4I', b, r + 0xE4)
        skinned = nb > 0
        ps = 0x30 if skinned else 0x18
        pos = [b[h[2] + ps * k:h[2] + ps * (k + 1)] for k in range(h[1])]
        attr = [b[h[4] + 28 * k:h[4] + 28 * (k + 1)] for k in range(h[3])]
        pairs = [struct.unpack_from('<2H', b, h[6] + 4 * k) for k in range(h[5])]
        subs = []
        for s in range(ns):
            _, cnt, mo = struct.unpack_from('<3I', b, so + 0x18 * s)
            subs.append((cnt, b[mo:mo + 0xE8]))
        out.append({'h': h, 'name': b[r + 0x30:r + 0x50], 'sphere': b[r + 0x50:r + 0x64],
                    'box': struct.unpack_from('<32f', b, r + 0x64),
                    'bones': [b[bo + 64 * k:bo + 64 * k + 64] for k in range(nb)],
                    'pos': pos, 'attr': attr, 'pairs': pairs, 'subs': subs,
                    'skinned': skinned})
        r += h[11] & ~3
    return out


def corners(m):
    """Expanded strip as (x, y, z, u, v) per entry, rounded, per sub-mesh."""
    res = []
    k = 0
    for cnt, mat in m['subs']:
        part = []
        for pi, ai in m['pairs'][k:k + cnt]:
            x, y, z = struct.unpack_from('<3f', m['pos'][pi], 0)
            u, v = struct.unpack_from('<2f', m['attr'][ai], 4)
            part.append((round(x, 3), round(y, 3), round(z, 3), round(u, 3), round(v, 3)))
        res.append(part)
        k += cnt
    return res


def check(pc_pkg_path, xbs_path):
    sys.path.insert(0, __file__.rsplit('\\', 1)[0].rsplit('/', 1)[0])
    import pc_pkg
    import xbs
    p = pc_pkg.Pkg(pc_pkg_path)
    x = xbs.Package(xbs_path)
    xn = {e.name.lower(): e.name for e in x.entries}
    tot = ok = 0
    for pth, pb in p.items():
        if not pth.upper().endswith('.PCMESH'):
            continue
        name = pth.split('\\')[-1].lower().replace('.pcmesh', '.xbmesh')
        if name not in xn:
            continue
        tot += 1
        try:
            mine = read_pc(convert(x.data(xn[name])))
        except Exception as e:
            print('%-30s convert failed: %s' % (name, e))
            continue
        real = read_pc(pb)
        probs = []
        if len(mine) != len(real):
            probs.append('meshes %d vs %d' % (len(mine), len(real)))
        for a, c in zip(mine, real):
            nm = a['name'].split(b'\0')[0].decode()
            if a['h'][9] != c['h'][9]:
                probs.append('%s flags %x vs %x' % (nm, a['h'][9], c['h'][9]))
            if a['sphere'] != c['sphere']:
                probs.append('%s sphere' % nm)
            if a['bones'] != c['bones']:
                probs.append('%s bones' % nm)
            if [s[1][0x18:0xE8] for s in a['subs']] != [s[1][0x18:0xE8] for s in c['subs']] and \
               set(s[1][0x18:0xE8] for s in a['subs']) != set(s[1][0x18:0xE8] for s in c['subs']):
                probs.append('%s materials' % nm)
            box = max((abs(i - j) for i, j in zip(a['box'], c['box'])), default=0)
            if box > 0.01:
                probs.append('%s box off %.3g' % (nm, box))
            ca, cc = corners(a), corners(c)
            fa = [t for part in ca for t in part]
            fc = [t for part in cc for t in part]
            if fa != fc:
                ta, tc = set(fa), set(fc)
                probs.append('%s strip %d/%d entries, %d/%d distinct, %d shared' %
                             (nm, len(fa), len(fc), len(ta), len(tc), len(ta & tc)))
        if probs:
            print('%-30s %s' % (name, '; '.join(probs[:4])))
        else:
            ok += 1
    print('%d of %d meshes match' % (ok, tot))


if __name__ == '__main__':
    if sys.argv[1] == 'check':
        check(sys.argv[2], sys.argv[3])
