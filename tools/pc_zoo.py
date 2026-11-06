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
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dds2tga  # noqa: E402
import pc_pkg  # noqa: E402
import pcmesh  # noqa: E402
import xbs  # noqa: E402

BS = '\\'
LEVEL_DIR = 'M7BRONXZOO' + BS
LEVELS = ('ZOO_B', 'ZOO_C')
CORE = {'sx', 'sxl', 'scn', 'sin', 'terx', 'snml', 'txtmesh', 'path', 'voq'}


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


def build(xbox_data, pc_data, out_dir):
    common = pc_pkg.Pkg(os.path.join(pc_data, 'COMMON.pkg'))
    in_common = {f[0].upper() for f in common.files}
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
        for e in sorted(x.entries, key=lambda e: e.name.lower()):
            n = pc_name(e.name)
            if n in in_common:
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
            else:
                data = raw
                stats['as is'] += 1
            files.append((folder(n, kraven_tex), n, data))
        # one directory record per folder, files grouped
        files.sort(key=lambda f: (f[0], f[1]))
        out = os.path.join(out_dir, lv + '.pkg')
        pc_pkg.write(out, files)
        print('%s: %d files, %.1f MB  %s' % (lv, len(files), os.path.getsize(out) / 1e6, dict(stats)))
        for d, c in sorted(Counter(f[0] for f in files).items()):
            print('    %-34s %d' % (d, c))


if __name__ == '__main__':
    build(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else 'pc_zoo_out')
