"""Packs the game-derived extras beside the built exe into build/extras.pak,
which CMakeLists.txt then embeds in SpiderManSetup.exe (resource 204) and
src/setup.c unpacks into the install folder.

The extras: texpack/ (HD textures), movies/ (upscaled FMVs), overlay/
(Shocker package, PS button text, legal screen), panel/ and previews/ (the
costume panel), subs.txt (subtitles). Each is made from the game by the
other tools; run them first.

Format, little-endian: b"SMXP", u32 count, then per file
  u16 path length, path (UTF-8, '/' separated), u32 size, data.

    python tools/make_extras_pack.py [build/Release] [build/extras.pak]
"""
import os
import struct
import sys

EXTRAS = ["texpack", "movies", "overlay", "panel", "previews", "subs.txt"]


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join("build", "Release")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join("build", "extras.pak")
    files = []
    for name in EXTRAS:
        top = os.path.join(src, name)
        if os.path.isfile(top):
            files.append(name)
        elif os.path.isdir(top):
            for root, _, names in os.walk(top):
                for n in sorted(names):
                    rel = os.path.relpath(os.path.join(root, n), src)
                    files.append(rel.replace(os.sep, "/"))
        else:
            print("missing:", top)
    total = 0
    with open(out, "wb") as f:
        f.write(b"SMXP" + struct.pack("<I", len(files)))
        for rel in files:
            data = open(os.path.join(src, rel), "rb").read()
            p = rel.encode("utf-8")
            f.write(struct.pack("<H", len(p)) + p + struct.pack("<I", len(data)) + data)
            total += len(data)
    print(f"{out}: {len(files)} files, {total / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
