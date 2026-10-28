r"""Upscale HUD art into the texture pack (texpack\<hash>.png).

    py -3 tools/hud_upscale.py [--dry]

The head icons beside the health bar (ib_*.dds: each hero's head_icon and
its stealth variant, and the bosses'), the compass and the height meter
(altimiter*, alt_*), read from every package under game_files\data, decoded
exactly as the renderer decodes them (d3d8_dxt_decode_texel), named by the
renderer's FNV-1a hash of the decoded texels (gpu_backend.c texels_hash),
and upscaled x4 with Real-ESRGAN (tools\realesrgan, anime model, as the
button strip). Writes texpack\ and build\Release\texpack\. --dry lists
names and hashes only.
"""
import glob
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xbs

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = r"C:\Users\adub9\xboxrecomp\game_files\data"
ESRGAN = os.path.join(ROOT, "tools", "realesrgan", "realesrgan-ncnn-vulkan.exe")
MODEL = "realesrgan-x4plus-anime"
OUT = (os.path.join(ROOT, "texpack"), os.path.join(ROOT, "build", "Release", "texpack"))

WANT = re.compile(r"^(ib_\w+|compass_\w+|compas_\w+|e_compass_\w+|altimiter\w*|alt_\w+)\.dds$")


def expand(v, bits):
    return v * 255 // ((1 << bits) - 1)


def decode_dxt(data, w, h, fourcc):
    """ARGB texels, row-major: d3d8_dxt_decode_texel, texel for texel."""
    bb = 8 if fourcc == b"DXT1" else 16
    bpr = (w + 3) // 4
    out = [0] * (w * h)
    for by in range((h + 3) // 4):
        for bx in range(bpr):
            b = data[(by * bpr + bx) * bb:(by * bpr + bx + 1) * bb]
            alphas = [255] * 16
            if bb == 16:
                if fourcc == b"DXT3":
                    for t in range(16):
                        nib = b[t >> 1]
                        alphas[t] = expand((nib >> 4) if t & 1 else (nib & 15), 4)
                else:
                    a0, a1 = b[0], b[1]
                    bits = int.from_bytes(b[2:8], "little")
                    a = [a0, a1] + [0] * 6
                    if a0 > a1:
                        for i in range(1, 7):
                            a[i + 1] = ((7 - i) * a0 + i * a1) // 7
                    else:
                        for i in range(1, 5):
                            a[i + 1] = ((5 - i) * a0 + i * a1) // 5
                        a[6], a[7] = 0, 255
                    for t in range(16):
                        alphas[t] = a[(bits >> (3 * t)) & 7]
                b = b[8:]
            c0 = b[0] | b[1] << 8
            c1 = b[2] | b[3] << 8
            c = [0, 0, 0, 0]
            for i, rgb in enumerate((c0, c1)):
                c[i] = (expand(rgb >> 11 & 31, 5) << 16 | expand(rgb >> 5 & 63, 6) << 8
                        | expand(rgb & 31, 5))
            four = c0 > c1 or bb == 16
            for sh in (0, 8, 16):
                m0, m1 = c[0] >> sh & 255, c[1] >> sh & 255
                if four:
                    c[2] |= ((2 * m0 + m1) // 3) << sh
                    c[3] |= ((m0 + 2 * m1) // 3) << sh
                else:
                    c[2] |= ((m0 + m1) // 2) << sh
            idxs = int.from_bytes(b[4:8], "little")
            for t in range(16):
                x, y = bx * 4 + (t & 3), by * 4 + (t >> 2)
                if x >= w or y >= h:
                    continue
                idx = idxs >> (2 * t) & 3
                alpha = alphas[t]
                if bb == 8 and not four and idx == 3:
                    alpha = 0
                out[y * w + x] = (alpha << 24) | (c[idx] & 0xFFFFFF)
    return out


def fnv(px):
    h = 2166136261
    for p in px:
        h = ((h ^ p) * 16777619) & 0xFFFFFFFF
    return h


def write_png(path, px, w, h):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for p in px[y * w:(y + 1) * w]:
            raw += bytes((p >> 16 & 255, p >> 8 & 255, p & 255, p >> 24 & 255))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def collect():
    """hash -> (name, w, h, texels), one per distinct image."""
    found = {}
    for f in sorted(glob.glob(os.path.join(GAME, "**", "*.xbs"), recursive=True)):
        try:
            p = xbs.Package(f)
        except Exception:
            continue
        for e in p.entries:
            name = e.name
            if not WANT.match(name):
                continue
            d = bytes(p.stream_of(e)[e.offset:e.offset + e.size])
            h, w = struct.unpack_from("<II", d, 12)
            fourcc = d[84:88]
            if fourcc not in (b"DXT1", b"DXT3", b"DXT5"):
                print("skip %s: %r" % (name, fourcc))
                continue
            px = decode_dxt(d[128:], w, h, fourcc)
            found.setdefault(fnv(px), (name, w, h, px))
    return found


def main(dry):
    found = collect()
    for hsh, (name, w, h, _) in sorted(found.items(), key=lambda kv: kv[1][0]):
        print("%08X %-32s %dx%d" % (hsh, name, w, h))
    if dry:
        return
    for d in OUT:
        os.makedirs(d, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        src, dst = os.path.join(tmp, "in"), os.path.join(tmp, "out")
        os.makedirs(src)
        os.makedirs(dst)
        for hsh, (name, w, h, px) in found.items():
            write_png(os.path.join(src, "%08X.png" % hsh), px, w, h)
        subprocess.run([ESRGAN, "-i", src, "-o", dst, "-n", MODEL, "-s", "4", "-f", "png"],
                       cwd=os.path.dirname(ESRGAN), check=True)
        for f in os.listdir(dst):
            data = open(os.path.join(dst, f), "rb").read()
            for d in OUT:
                open(os.path.join(d, f), "wb").write(data)
    print("%d images upscaled into %s" % (len(found), ", ".join(OUT)))


if __name__ == "__main__":
    main("--dry" in sys.argv)
