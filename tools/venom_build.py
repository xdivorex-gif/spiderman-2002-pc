"""Experimental USM Venom mesh conversion using the existing XBS tools.

This produces a development package, not a finished Venom moveset. It keeps
the destination hero's animations. Nothing is installed or selected by this
tool. Supply paths to your own PC USM packs and unpacked Xbox game data.
"""
import argparse
import json
import math
from pathlib import Path
import re
import struct

from usm_pack import Pack
import xbs


def bone_map(bone):
    if bone <= 4:
        return bone
    if bone <= 7:
        return 5
    if bone <= 26:
        return bone - 2
    # Auxiliary arm twists have no counterpart in the 52-bone rig.
    # 28/50 affect the elbow end of the upper arm; 29/51 the forearm.
    # Collapsing these is a prototype approximation, not animation retargeting.
    if bone == 28:
        return 7
    if bone == 50:
        return 26
    if bone == 29:
        return 8
    if 30 <= bone <= 48:
        return bone - 5
    if bone == 51:
        return 27
    if 52 <= bone <= 59:
        return bone - 8
    raise ValueError(f'unmapped weighted bone {bone}')


# Which USM model the slot wears. A PCM holds its levels of detail one after
# another, each a table of 0x60-byte section records whose pointers are
# relative to the PCM: the viewer Venom's full detail is the table at +0x340
# (17 sections, 6336 vertices), the 1720-vertex one Codex first converted is
# its second level (+0x9980). Materials are name records (hash, name):
#   VENOM_SPIDER (his spider-emblem form): +0x100A6 "venom spider solid" (the
#   body, one texel: lit by the sheen), +0x10126 "venom spider logo",
#   +0x10146 "venom spider body", +0x10026 "venom_s_teeth", +0x10166
#   "venom_s_no_outline" -- these on the atlas "venom_spider" (0xA98B5BCB) --
#   and +0x100E6 "venom_s_eye" on "venom_s_eye_01" (0x155A8759).
SOURCE = dict(pack='VENOM_SPIDER', mesh=0xA98B5BCB, table=0x3C0, sections=19,
              body=0x100A6,
              textures={0x100A6: 'vnbody', 0x10126: 'vnatlas', 0x10146: 'vnatlas',
                        0x10026: 'vnatlas', 0x10166: 'vnatlas', 0x100E6: 'vneye'},
              dds={'vnatlas': 0xA98B5BCB, 'vneye': 0x155A8759})
# The plain viewer Venom, full detail (kept for reference / switching back).
SOURCE_PLAIN = dict(pack='CH_VWR_VENOM_VIEWER', mesh=0x08909065, table=0x340, sections=17,
                    body=0xD2C8,
                    textures={0xD2C8: 'vnbody', 0xD268: 'vnmouth', 0xD388: 'vnmouth',
                              0xD368: 'vnbodydetail', 0xD328: 'vneye'},
                    dds={'vnmouth': 0x3B03E9F1, 'vneye': 0x8983B1A7,
                         'vnbodydetail': 0xBBF25212})


def source_sections(pack, src=SOURCE):
    pcm = pack.find(src['mesh'], 21)
    if pcm[:8] != b'PCM \x01\x06\0\0':
        raise ValueError('unsupported PCM version')
    mesh = pcm
    result = []
    for i in range(src['sections']):
        words = struct.unpack_from('<24I', mesh, src['table'] + i * 0x60)
        material, _, nb, palette = words[:4]
        primitive, ni, ip, _, nv, vp, byte_count = words[10:17]
        if not 0 < nb <= 64 or not 0 < nv < 65536 or byte_count != nv * 64:
            raise ValueError(f'invalid section {i}')
        if primitive not in (4, 5):
            raise ValueError(f'unsupported primitive {primitive}')
        bones = struct.unpack_from(f'<{nb}H', mesh, palette)
        indices = struct.unpack_from(f'<{ni}H', mesh, ip)
        if max(indices, default=0) >= nv:
            raise ValueError('section index outside vertex buffer')
        # PCM vertex pointer addresses position, not the preceding weights.
        # 64 bytes: position3, normal3, uv2, palette indices4, weights4.
        start = vp
        if start < 0 or start + byte_count > len(mesh):
            raise ValueError('section vertex buffer outside mesh')
        vertices = []
        for j in range(nv):
            v = struct.unpack_from('<16f', mesh, start + j * 64)
            if not all(math.isfinite(x) for x in v):
                raise ValueError('nonfinite source vertex')
            if abs(sum(v[12:]) - 1) > 1e-4 or min(v[12:]) < 0:
                raise ValueError('invalid source skin weights')
            influences = []
            for weight, index in zip(v[12:], v[8:12]):
                if weight > 0:
                    if index != int(index) or not 0 <= index < nb:
                        raise ValueError('invalid skin palette index')
                    influences.append((bone_map(bones[int(index)]), weight))
            vertices.append((v[:3], v[3:6], v[6:8], influences))
        result.append((material, primitive, indices, vertices))
    return result


def normal_word(normal):
    length = math.sqrt(sum(x*x for x in normal))
    if length < 1e-6:
        raise ValueError('zero normal')
    values = [round(max(-1, min(1, x / length)) * scale)
              for x, scale in zip(normal, (1023, 1023, 511))]
    return (values[0] & 2047) | ((values[1] & 2047) << 11) | ((values[2] & 1023) << 22)


# USM's body material is one texel of an atlas (u .911, v .531) that its
# cel shader lights: every body vertex has the same UV, so drawn by this
# engine Venom is one flat purple. Instead the body gets a ramp texture,
# black to his lavender highlight, and each vertex a place on it from its
# normal under a key light from front and above (model space: +y up, +z
# front). The engine's own lighting still applies on top.
BODY_MATERIAL = SOURCE['body']
# Hard cel bands, as USM draws him: shadow, mid, highlight, sheen.
RAMP = [(0.78, (5, 5, 9)), (0.90, (34, 26, 66)), (0.97, (150, 100, 215)),
        (1.01, (215, 185, 255))]
RAMP_BLOCKS = 16
KEY_LIGHT = (0.35, 0.75, 0.55)
# Ink outline: a copy of the body pushed out along its normals, wound the
# other way and drawn black, so only its back faces past the silhouette
# show (the inverted-hull outline). Source units; the model is ~3.4 tall.
INK_MATERIAL = 'ink'
INK_WIDTH = 0.022
# Off: the game draws hero meshes without back-face culling, so the shell
# covers him (in-game he showed all grey).
INK_ENABLED = False


def ramp_colour(s):
    for top, colour in RAMP:
        if s < top:
            return colour
    return RAMP[-1][1]


def solid_dds(colour):
    """4x4 DXT1 of one colour."""
    r, g, b = colour
    c = ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | (b * 31 + 127) // 255
    header = struct.pack('<4s7I44x8I5I', b'DDS ', 124, 0x81007, 4, 4, 8, 0, 1,
                         32, 4, 0x31545844, 0, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0)
    return header + struct.pack('<HHI', c, c, 0)


def ramp_dds():
    """64x4 DXT1, one solid colour per 4x4 block, no mipmaps."""
    def rgb565(c):
        r, g, b = c
        return ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | (b * 31 + 127) // 255
    blocks = b''.join(struct.pack('<HHI', rgb565(ramp_colour(i / (RAMP_BLOCKS - 1))),
                                  rgb565(ramp_colour(i / (RAMP_BLOCKS - 1))), 0)
                      for i in range(RAMP_BLOCKS))
    header = struct.pack('<4s7I44x8I5I', b'DDS ', 124, 0x81007, 4, RAMP_BLOCKS * 4,
                         len(blocks), 0, 1, 32, 4, 0x31545844, 0, 0, 0, 0, 0,
                         0x1000, 0, 0, 0, 0)
    return header + blocks


# What USM really does for his sheen: the viewer pack carries a 64x64 sphere
# (environment) map, 0x550cc72b, glossy black chrome. USM reflects it off the
# normals; this engine can't, so each body vertex gets the sphere-map place
# of its model-space normal (seen from the front; the back mirrors it).
SPHERE_MAP = 0x550cc72b
SHEEN_TINT = (0.52, 0.40, 0.68)      # glossy black, lavender sheen (ac71e6)


def sphere_uv(normal):
    length = math.sqrt(sum(x * x for x in normal)) or 1
    nx, ny = normal[0] / length, normal[1] / length
    return (0.5 + 0.47 * nx, 0.5 - 0.47 * ny)


def tint_dxt1(dds, tint):
    """Scale every DXT1 block's two end colours (all mips). A block whose
    colour order the tint reverses gets its ends and indices swapped, so its
    4-colour or 3-colour mode is unchanged."""
    def scale(c):
        r, g, bl = (c >> 11) & 31, (c >> 5) & 63, c & 31
        return (min(31, round(r * tint[0])) << 11 | min(63, round(g * tint[1])) << 5
                | min(31, round(bl * tint[2])))
    out = bytearray(dds)
    for at in range(128, len(out) - 7, 8):
        a, b, bits = struct.unpack_from('<HHI', out, at)
        na, nb = scale(a), scale(b)
        if a > b:                           # 4-colour: needs na > nb
            if na < nb:
                na, nb = nb, na
                bits ^= 0x55555555          # 0<->1, 2<->3
            elif na == nb:
                bits = 0                    # one colour left
                na = nb + 1 if nb < 0xffff else nb
        else:                               # 3-colour + black: needs na <= nb
            if na > nb:
                na, nb = nb, na
                # 0<->1, 2 stays, 3 (black) stays
                bits = sum(({0: 1, 1: 0}.get((bits >> 2 * i) & 3, (bits >> 2 * i) & 3)) << 2 * i
                           for i in range(16))
        struct.pack_into('<HHI', out, at, na, nb, bits)
    return bytes(out)


def ramp_u(normal):
    length = math.sqrt(sum(x * x for x in normal)) or 1
    l_len = math.sqrt(sum(x * x for x in KEY_LIGHT))
    s = 0.5 + 0.5 * sum(n * l for n, l in zip(normal, KEY_LIGHT)) / (length * l_len)
    block = min(RAMP_BLOCKS - 1, int(s * RAMP_BLOCKS))
    return (block * 4 + 2) / (RAMP_BLOCKS * 4)


def native_mesh(template, sections, scale, extra_bones=(), extra_sections=()):
    """extra_bones: bind matrices appended after the slot's; extra_sections:
    body-material sections already in slot bone numbering (tentacles)."""
    u32 = lambda o: struct.unpack_from('<I', template, o)[0]
    if template[:4] != b'XBXM' or u32(0x6c) not in (28, 52) or u32(0x7c) != 44:
        raise ValueError('unsupported destination rig')
    bone_count = u32(0x6c)
    def target_bone(b):
        if bone_count == 52 or b <= 5:
            return b
        if b <= 9:
            return b + 4
        if b <= 24:
            return 13  # Full finger chains are absent in the KNUCKLES rig.
        if b <= 28:
            return b - 10
        if b <= 43:
            return 18
        return b - 24
    sections = [(m, p, idx, [(pos, normal, uv, [(target_bone(b), w) for b, w in skin])
                 for pos, normal, uv, skin in verts]) for m, p, idx, verts in sections]
    sections = sections + [(BODY_MATERIAL, p, idx, verts) for _, p, idx, verts in extra_sections]
    out = bytearray(template[:0x80])
    def append(data, align=4):
        out.extend(bytes(-len(out) % align))
        at = len(out)
        out.extend(data)
        return at
    bones = append(template[u32(0x70):u32(0x70) + bone_count * 64]
                   + b''.join(struct.pack('<16f', *[x for row in m for x in row]) for m in extra_bones), 16)
    bone_count += len(extra_bones)
    textures = {**SOURCE['textures'], INK_MATERIAL: 'vnink'}
    if INK_ENABLED:
        # The ink shell: every body section again, pushed out, winding flipped.
        sections = sections + [(INK_MATERIAL, p, idx,
                                [(tuple(x + n * INK_WIDTH / (math.sqrt(sum(c * c for c in normal)) or 1)
                                        for x, n in zip(pos, normal)), normal, (0.5, 0.5), skin)
                                 for pos, normal, uv, skin in verts])
                               for m, p, idx, verts in sections if m == BODY_MATERIAL]
    # Allocated after the ink sections are added: one record per section.
    sub_at = append(bytes(len(sections) * 0x38), 16)
    old_mat = struct.unpack_from('<I', template, u32(0x78) + 12)[0]
    materials = {}
    for material in [m for m in textures if m in set(s[0] for s in sections)]:
        if material not in textures:
            raise ValueError(f'unrecognised source material {material:#x}')
        mat = bytearray(template[old_mat:old_mat + 0xe0])
        # Use the standard lit hero material rather than the thug's flags.
        struct.pack_into('<I', mat, 0, 0x149410)
        mat[0x18:0x38] = textures[material].encode().ljust(32, b'\0')
        materials[material] = append(mat, 16)
    palettes, vertices, indices, subs = [], bytearray(), [], []
    positions = []
    for material, primitive, source_indices, source_vertices in sections:
        palette = sorted(set(b for v in source_vertices for b, w in v[3]))
        palette_at = append(struct.pack(f'<{len(palette)}H', *palette))
        base = len(vertices) // 44
        for pos, normal, uv, influences in source_vertices:
            if material == BODY_MATERIAL:
                uv = sphere_uv(normal)
            combined = {}
            for bone, weight in influences:
                combined[bone] = combined.get(bone, 0) + weight
            skin = sorted(combined.items(), key=lambda v: -v[1])
            weights = [round(w * 255) for _, w in skin]
            weights[0] += 255 - sum(weights)
            palette_indices = [palette.index(b) for b, _ in skin]
            p = tuple(x * scale for x in pos)
            positions.append(p)
            vertices.extend(struct.pack('<3fII2fII4B4B', *p, 0xffffffff,
                            normal_word(normal), *uv, 0, 0,
                            *(palette_indices + [0] * (4-len(skin))),
                            *(weights + [0] * (4-len(skin)))))
        first = len(indices)
        local = list(source_indices)
        if primitive == 4:
            if len(local) % 3:
                raise ValueError('triangle list has an incomplete triangle')
            strip = []
            for k in range(0, len(local), 3):
                a, b, c = local[k:k+3]
                if strip:
                    # Repeated indices join triangles without visible faces.
                    strip.extend((strip[-1], a))
                    if len(strip) % 2:
                        strip.append(a)
                strip.extend((a, b, c))
            local = strip
        # USM winds front faces the other way from this engine (in-game he
        # drew inside out and the one-sided eyes vanished): one repeated
        # index at the strip's start flips every triangle.
        local = [local[0]] + local
        if material == INK_MATERIAL:
            local = [local[0]] + local  # the shell faces inward: flip back
        indices.extend(base + j for j in local)
        subs.append((first, len(local), materials[material], len(palette), palette_at))
    if len(vertices) // 44 > 65535:
        raise ValueError('converted mesh exceeds 16-bit indices')
    unknown = append(bytes(32), 16)
    vbh = append(bytes(36), 16)
    append(struct.pack('<I', len(indices)))
    ilist = append(struct.pack(f'<{len(indices)}H', *indices), 1)
    vdata = append(vertices, 0x1000)
    struct.pack_into('<I', out, 0x14, vbh)
    struct.pack_into('<I', out, 0x18, ilist)
    struct.pack_into('<I', out, 0x2c, unknown)
    struct.pack_into('<5I', out, 0x6c, bone_count, bones, len(subs), sub_at, 44)
    radius = max(math.sqrt(sum(x*x for x in p)) for p in positions)
    struct.pack_into('<5f', out, 0x50, 0, 0, 0, 1, radius)
    struct.pack_into('<I', out, vbh, vdata)
    struct.pack_into('<2I', out, vbh+0x10, len(vertices)//44, ilist)
    for i, (first, count, mat, nb, palette_at) in enumerate(subs):
        struct.pack_into('<4I5f5I', out, sub_at+i*0x38, vbh, first, count, mat,
                         radius, 0, 0, 0, 1, nb, palette_at, 0, 0, 0)
    return bytes(out)


def validate_mesh(data):
    """Check native ranges and skin bindings before allowing package output."""
    def words(at, count=1):
        if at < 0 or at + count * 4 > len(data):
            raise ValueError('native mesh pointer outside record')
        return struct.unpack_from(f'<{count}I', data, at)
    nb, bones, ns, subs, stride = words(0x6c, 5)
    if bones + nb * 64 > len(data) or stride != 44:
        raise ValueError('invalid native bone or vertex layout')
    triangles = 0
    for section in range(ns):
        at = subs + section * 0x38
        vbh, first, count, material = words(at, 4)
        palette_count, palette = words(at + 0x24, 2)
        vertex_data, = words(vbh)
        vertex_count, indices = words(vbh + 0x10, 2)
        index_count, = words(indices - 4)
        if first + count > index_count or indices + index_count * 2 > len(data):
            raise ValueError('native index range outside buffer')
        if palette + palette_count * 2 > len(data):
            raise ValueError('native palette outside record')
        bone_ids = struct.unpack_from(f'<{palette_count}H', data, palette)
        if not bone_ids or max(bone_ids) >= nb:
            raise ValueError('native palette references missing bone')
        if material + 0xe0 > len(data) or vertex_data + vertex_count * stride > len(data):
            raise ValueError('native material or vertices outside record')
        idx = struct.unpack_from(f'<{count}H', data, indices + first * 2)
        for vertex in set(idx):
            if vertex >= vertex_count:
                raise ValueError('native index references missing vertex')
            v = vertex_data + vertex * stride
            skin = data[v + 36:v + 40]
            weights = data[v + 40:v + 44]
            if sum(weights) != 255:
                raise ValueError('native skin weights do not sum to 255')
            if any(w and b >= palette_count for b, w in zip(skin, weights)):
                raise ValueError('native skin index outside section palette')
        triangles += sum(len(set(idx[i:i+3])) == 3 for i in range(count - 2))
    return dict(bones=nb, sections=ns, triangles=triangles,
                vertices=vertex_count, bytes=len(data))


# Venom animation -> the Spider-Man entry the state table already names.
# Spider-Man entry -> Venom's USM animation. Templates give the root motion
# and the event list (footsteps, strikes), retimed. venexttrn45r has no
# decoded channels yet, so smexttrn45r stays Spider-Man's.
ANIMATIONS = {
    # standing, running, jumping
    'smidl': 'venidl', 'smrun18': 'venrun', 'smcrchidl': 'vencrchidl',
    'smidl2boredlook': 'venidlfidget01', 'smidl2boredcrchlook': 'venidlfidget02',
    'smjmptwolnch': 'venjmplch', 'smjmptwofly': 'venjmpfly', 'smfall': 'venjmpfly',
    'smjmptwolnd': 'venjmplnd2idl', 'smlnd2run': 'venjmplnd2run',
    # wall crawling
    'smcrch2crl': 'vencrch2crl', 'smcrch2crltight': 'vencrch2crltight',
    'smcrlrhnf1': 'vencrlrhnf1', 'smcrllhnf1': 'vencrllhnf1',
    'smcrlrhnf2crch': 'vencrlrhnf2crch', 'smcrllhnf2crch': 'vencrllhnf2crch',
    'smcrltight': 'vencrltight', 'smcrltight2crch': 'vencrltight2crch',
    'smcrlhopfence': 'vencrlhopfence',  # smrun2wallcrl: 75-track rig, kept
    'smexttrnwallcrl2idltemp': 'venexttrnwallcrl2idl',
    # crawl turns over inside / outside edges
    **{f'sminttrn{s}': f'veninttrn{s}' for s in
       ('', '135', '45l', '45r', 'tight', 'tight135', 'tight135_45l', 'tight135_45r',
        'tight45l', 'tight45r', '135_45l', '135_45r')},
    **{f'smexttrn{s}': f'venexttrn{s}' for s in
       ('', '135', '45l', 'tight', 'tight135', 'tight135_45l', 'tight135_45r',
        'tight45l', 'tight45r', '135_45l', '135_45r')},
    # hit, attacks
    'smidlhithigh': 'venhitstpbk', 'smpunr': 'venclaw_2',
    'smjmpairpunhit': 'venairrake', 'smjmpairkckhit': 'venaerialflail',
    # tentacle combo on the punch chain (PUNCH, PUNCH_PUNCH, PUNCH_PUNCH_PUNCH),
    # the kicks (KICK, KICK_KICK) and the PUNCH_PUNCH_KICK finisher: body
    # motion only, the tentacles themselves are separate USM meshes.
    'smphookr': 'vententa_1', 'smppcrossl': 'vententa_2', 'smpppupper': 'vententa_3',
    'smksidekck': 'vententa_4', 'smkkgutkck': 'vententa_5', 'smppkthrust': 'vententaslamcng',
    # zips: USM Venom travels by tentacle zip-line (launch, flight, landing)
    'smziplnchv': 'venidlzipline2fly', 'smziplnchf': 'venidlzipline2fly',
    'smcrchziplnchv': 'venidlzipline2fly',
    'smzipflyv': 'vencrlbigjmpfly', 'smzipflyf': 'vencrlbigjmpfly', 'smcrchzipflyv': 'vencrlbigjmpfly',
    'smziplndv': 'venzipline2crl', 'smziplndf': 'venzipline2crl',
}
# The slot's package outgrows the 4,092,800 bytes aux_xbox.txt reserves per
# hero; an aux_xbox.txt beside it in the costume overlay raises THUGSPIDEY's
# line (as shocker_build.py does for SHOCKSPIDEY).
BUDGET = 6000000


def add_animations(hero, viewer, bank, mesh, channel_dir):
    """Move the slot's skeleton to Venom's proportions and replace the
    entries in ANIMATIONS with his converted USM animations."""
    import venom_anim as va
    import usm_venom_pose as vp
    pcm = viewer.find(0x08909065, 21)
    sk = vp.Skeleton(bank.find(0x08909065, 2), pcm)
    import venom_tentacles as vt
    thug_bind, _ = va.mesh_bones(mesh)
    thug_bind = thug_bind[:28]                       # the slot's own bones
    new_bind = va.venom_bind(thug_bind, sk.bind)
    full_bind, _, _ = vt.add_bones(new_bind)
    mesh = va.put_bind(mesh, full_bind)
    ent = hero.data('thugspidey.ent').decode('latin1')
    ent_new = vt.add_to_entity(va.edit_entity(ent, new_bind))
    hero.put('thugspidey.ent', ent_new.encode('latin1'))
    durations = {n: struct.unpack_from('<f', d, 0x38)[0] for n, d in bank.animations()}
    records = dict(bank.animations())
    import usm_tentacle_channel as tc
    activity = {}

    def frames(name):
        for f in (channel_dir / f'{name}-channels.json', channel_dir / 'decoded' / f'{name}-channels.json'):
            if f.exists():
                return [c['frames'] for c in json.loads(f.read_text())['channels']]
        raise FileNotFoundError(f'decoded channels for {name} (tools/usm_decode_channels.py)')

    # Ground: Venom's lowest idle foot where Spider-Man's lowest idle foot is.
    sm = va.Anmx(hero.data('smidl.anml'))
    sm_low = min(min(va.fk_2002(sm, rest_from_entity(ent, sm), k)[t][3][1] for t in (47, 51))
                 for k in range(len(sm.block(1, 1))))
    idle = frames('venidl')
    v_low = min(min(vp.full_pose(sk, idle, i)[b][3][1] for b in (54, 58)) for i in range(len(idle[0])))
    dy = sm_low - v_low * va.SCALE
    report = dict(ground_offset=dy, animations={})
    cache = {}
    stretch = {}                       # entry -> Venom duration / Spider-Man's
    jobs = [(n, n, u) for n, u in ANIMATIONS.items()] + \
           [(n, t, u) for n, (t, u) in EXTRA_ANIMATIONS.items()]
    # every template read before any entry is replaced (extra moves reuse
    # entries that ANIMATIONS also replaces)
    templates = {t: hero.data(t + '.anml') for _, t, _ in jobs}
    for out_name, tmpl_name, usm_name in jobs:
        template = templates[tmpl_name]
        if usm_name not in cache:
            cache[usm_name] = frames(usm_name)
        mode = TENTACLES.get(out_name, {})
        if mode in ('strike', 'both') and usm_name not in activity:
            activity[usm_name] = tc.slots(records[usm_name])   # USM's own tentacle timing
        data = va.convert(sk, cache[usm_name], durations[usm_name], template,
                          thug_bind, new_bind, dy, tentacles=mode,
                          activity=activity.get(usm_name))
        stretch[out_name] = durations[usm_name] / va.Anmx(template).duration
        hero.put(out_name + '.anml', data, xbs.TYPE_ANML)
        report['animations'][out_name] = dict(source=usm_name, template=tmpl_name,
                                              duration=durations[usm_name], bytes=len(data))
    mbo = hero.data('spiderman.mbo').decode('latin1')
    hero.put('spiderman.mbo', edit_moves(mbo, stretch, durations).encode('latin1'))
    report['mesh'] = mesh
    return report


# Tentacle use per entry: mode for the lead hand ('strike' peaks on the
# move's strike event; 'both' strikes with both), zips shoot up / forward,
# hold in flight, retract on landing. Others keep them hidden.
TENTACLES = {**{n: 'strike' for n in ('smphookr', 'smppcrossl', 'smpppupper', 'vententa4',
                                      'vententa5', 'smksidekck', 'smkkgutkck')},
             'smppkthrust': 'both',
             'smziplnchv': 'up', 'smcrchziplnchv': 'up', 'smziplnchf': 'forward',
             'smzipflyv': 'hold_up', 'smcrchzipflyv': 'hold_up', 'smzipflyf': 'hold_forward',
             'smziplndv': 'retract', 'smziplndf': 'retract'}


# New entries (name -> (template for root motion/events, USM animation)).
EXTRA_ANIMATIONS = {'vententa4': ('smksidekck', 'vententa_4'),
                    'vententa5': ('smkkgutkck', 'vententa_5')}
# USM's tentacle string is five hits; the game's punch chain stops at three.
# Two moves continue PUNCH_PUNCH_PUNCH from the punch button.
NEW_MOVES = '''
\tmove VEN_PUNCH4
\t\tanim VenTenta4
\t\tdamage 30
\t\tr_hand
\t\tl_hand
    non_require_target
\tchunkend

\tmove VEN_PUNCH5
\t\tanim VenTenta5
\t\tdamage 50
    big_hit
\t\texplosive
\t\tr_hand
\t\tl_hand
    non_require_target
\tchunkend
'''.replace('\n', '\r\n')
# Window as a share of the move it leaves, from Spider-Man's PUNCH_PUNCH ->
# PUNCH_PUNCH_PUNCH (0.36 s of 0.67 s).
CHAIN_SHARE = 0.54


def edit_moves(text, stretch, durations):
    """Retime combo windows out of Venom-animated moves; add the 4th and 5th
    tentacle hits."""
    anim_of = {m.group(1): m.group(2).lower()
               for m in re.finditer(r'move\s+(\w+)\s+anim\s+(\w+)', text)}

    def retime(m):
        k = stretch.get(anim_of.get(m.group(1), ''))
        if not k:
            return m.group(0)
        return re.sub(r'(button_begin_time|button_end_time|animation_time)(\s+)([\d.]+)',
                      lambda n: f'{n.group(1)}{n.group(2)}{float(n.group(3)) * k:.3f}', m.group(0))
    text = re.sub(r'combo\s+(\w+)\s+to\s+\w+.*?chunkend', retime, text, flags=re.S)
    # tentacle moves strike with the hands (the tentacle tips), not the feet
    tent = {n for n, m in TENTACLES.items() if m in ('strike', 'both')}

    def hands(m):
        if m.group(2).lower() not in tent:
            return m.group(0)
        body = re.sub(r'\b[rl]_foot\b', 'r_hand', m.group(0))
        for limb in ('r_hand', 'l_hand'):              # both tips count
            if not re.search(r'\b' + limb + r'\b', body):
                body = body.replace('chunkend', '\t\t' + limb + '\r\n\tchunkend', 1)
        return body
    text = re.sub(r'move\s+(\w+)\s+anim\s+(\w+).*?chunkend', hands, text, flags=re.S)
    if 'VEN_PUNCH4' not in text:
        at = text.index('moves:')
        at = text.index('\n', at) + 1
        text = text[:at] + NEW_MOVES + text[at:]

        def link(a, b, dur):
            w = dur * CHAIN_SHARE
            return ('\r\n\tcombo %s to %s\r\n\t  button_begin_time 0.01\r\n\t  button_end_time\t%.3f\r\n'
                    '\t  animation_time\t%.3f\r\n\t  button\t\t\tBUTTON_PUNCH\r\n\tchunkend\r\n'
                    % (a, b, w, w))
        links = link('PUNCH_PUNCH_PUNCH', 'VEN_PUNCH4', durations['vententa_3']) + \
            link('VEN_PUNCH4', 'VEN_PUNCH5', durations['vententa_4'])
        end = text.rindex('chunkend')
        text = text[:end] + links + '\r\n' + text[end:]
    return text


def rest_from_entity(ent_text, anim):
    """Track -> rest translation from the (original) entity's po, by name order."""
    import shlex
    toks = shlex.split(ent_text)
    po = []
    for i, t in enumerate(toks):
        if t == 'po':
            po.append([float(x) for x in toks[i + 1:i + 13]])
    # entity node order -> track (jaw/lip nodes 7..10 have no track)
    node_of = {0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 6: 6, 7: 11, 8: 12, 9: 13, 10: 14, 11: 15,
               26: 16, 27: 17, 28: 18, 29: 19, 30: 20}
    for k in range(8):
        node_of[45 + k] = 21 + k
    return {t: [po[n][3], po[n][7], po[n][11]] for t, n in node_of.items()}


def compact(pkg):
    """Drop the bytes put() left behind: rebuild each stream from the live
    entries in their order, each kept at its original alignment."""
    for stream_id in set(e.stream for e in pkg.entries):
        live = sorted((e for e in pkg.entries if e.stream == stream_id), key=lambda e: e.offset)
        old = bytes(pkg.stream_of(live[0]))
        new = bytearray()
        for e in live:
            align = 0x1000
            while e.offset % align:
                align //= 2
            new.extend(bytes(-len(new) % align))
            data = old[e.offset:e.offset + e.space]
            e.offset = len(new)
            new.extend(data)
        stream = pkg.stream_of(live[0])
        stream[:] = new


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--usm-packs', required=True, type=Path)
    ap.add_argument('--game-data', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    ap.add_argument('--channels', type=Path,
                    default=Path(__file__).resolve().parent.parent / 'cap' / 'usm' / 'venom_prototype',
                    help='folder of decoded USM channel JSON (usm_decode_channels.py)')
    args = ap.parse_args()
    viewer = Pack(args.usm_packs / 'CH_VWR_VENOM_VIEWER.PCPACK')
    bank = Pack(args.usm_packs / 'VENOM.PCPACK')
    hero = xbs.Package(args.game_data / 'heroes' / 'THUGSPID.xbs')
    candidates = [e.name for e in hero.entries if e.type == xbs.TYPE_MESH
                  and e.name.startswith('thugspidey')]
    if len(candidates) != 1:
        raise ValueError(f'ambiguous destination mesh: {candidates}')
    model = Pack(args.usm_packs / (SOURCE['pack'] + '.PCPACK'))
    sections = source_sections(model)
    import venom_anim as va
    import venom_tentacles as vt
    import usm_venom_pose as vp
    sk = vp.Skeleton(bank.find(0x08909065, 2), viewer.find(0x08909065, 21))
    thug_bind, _ = va.mesh_bones(hero.data(candidates[0]))
    full_bind, _, which = vt.add_bones(va.venom_bind(thug_bind, sk.bind))
    converted = native_mesh(hero.data(candidates[0]), sections, 0.59,
                            extra_bones=full_bind[len(thug_bind):],
                            extra_sections=vt.geometry(full_bind, which, 0.59))
    animated = add_animations(hero, viewer, bank, converted, args.channels)
    converted = animated.pop('mesh')
    validation = validate_mesh(converted)
    hero.put(candidates[0], converted, align=0x1000)
    for name, hash_ in [('vnbody', None)] + list(SOURCE['dds'].items()):
        dds = (tint_dxt1(viewer.find(SPHERE_MAP, 6), SHEEN_TINT) if name == 'vnbody'
               else model.find(hash_, 6))
        if not dds.startswith(b'DDS '):
            raise ValueError('texture is not a DDS file')
        hero.put(name + '.dds', dds, xbs.TYPE_DDS)
    hero.put('vnink.dds', solid_dds((0, 0, 0)), xbs.TYPE_DDS)
    # The thug's body texture is unused with his mesh replaced; shrinking it
    # pays for the ink shell within the slot's stream allocation.
    hero.put('thug2.dds', solid_dds((0, 0, 0)), xbs.TYPE_DDS)
    compact(hero)
    args.output.mkdir(parents=True, exist_ok=True)
    target = args.output / 'THUGSPID.xbs'
    if len(hero.s0) > BUDGET:
        raise ValueError(f'package stream 0 {len(hero.s0)} exceeds budget {BUDGET}')
    hero.write(target)
    aux = (args.game_data / 'heroes' / 'aux_xbox.txt').read_bytes().decode('latin1')
    aux = re.sub(r'(THUGSPIDEY,THUGSPID,\d+,)\d+', r'\g<1>%d' % BUDGET, aux)
    aux = re.sub(r'(SHOCKSPIDEY,SHOCKSPI,\d+,)\d+', r'\g<1>%d' % BUDGET, aux)
    (args.output / 'aux_xbox.txt').write_bytes(aux.encode('latin1'))
    rebuilt = xbs.Package(target)
    if rebuilt.data(candidates[0]) != converted:
        raise ValueError('XBS mesh roundtrip failed')
    animations = [dict(name=n, bytes=len(d), duration=struct.unpack_from('<f',d,0x38)[0],
                       frames=struct.unpack_from('<I',d,0x50)[0]) for n,d in bank.animations()]
    report = dict(validation=validation, stream0_bytes=len(hero.s0), venom_animation=animated,
                  status='experimental mesh only; native Spider-Man animations retained',
                  installed=False, slot='thugspidey', scale=0.59,
                  sections=len(sections), vertices=sum(len(s[3]) for s in sections),
                  usm_animations=animations, animation_conversion_complete=False,
                  limitations=['global scale; proportions need visual verification',
                               'auxiliary arm twists collapsed onto arm bones',
                               '28-bone slot: finger weights collapsed onto hands',
                               'Venom animation tracks and moves are not implemented',
                               'package is not installed or runtime-tested'])
    (args.output / 'venom_build.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f'Built {target}: {report["vertices"]} vertices; {len(animations)} USM animations catalogued')
    print('Development output only; Venom animation conversion and moves are still required.')


if __name__ == '__main__':
    main()
