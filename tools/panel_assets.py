r"""The game's own pause-menu art and sounds for the costume panel's prompt.

    py -3 tools/panel_assets.py

From game_files: the chiselwide 36px font (bonus\MENU.xbs) and the samples
button_scroll, button_confirm, button_back (xboxsound\ENGLISH\menu.XSH/XSD).
Writes build\Release\panel\: font.bin (costume_panel.c reads it) and <name>.wav.
"""
import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xbs
from hud_upscale import decode_dxt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = r"C:\Users\adub9\xboxrecomp\game_files\data"
OUTS = [os.path.join(ROOT, "build", d, "panel") for d in ("Release", "Test")]
SOUNDS = ("button_scroll", "button_confirm", "button_back")


def font():
    p = xbs.Package(os.path.join(DATA, "bonus", "MENU.xbs"))
    names = {e.name: e for e in p.entries}
    def get(suffix):
        n = next(k for k in names if k.startswith("chiselwide") and k.endswith("36px." + suffix))
        e = names[n]
        return bytes(p.stream_of(e)[e.offset:e.offset + e.size])
    fon = get("fon").decode("latin1")
    dds = get("dds")
    h, w = struct.unpack_from("<II", dds, 12)
    px = decode_dxt(dds[128:], w, h, dds[84:88])
    glyphs = {}
    order = [dict((k, int(n)) for k, n in re.findall(r"(\w+)\s+(-?\d+)", blk))
             for blk in re.split(r";; glyphnum\s+\d+", fon)[1:]]
    # The file's "advance" is a running total: a glyph's width is the next
    # one's total less its own.
    for i, v in enumerate(order):
        v["advance"] = (order[i + 1]["advance"] - v["advance"] if i + 1 < len(order)
                        else v["glyphwidth"] + 1)
        if v["ascii"] < 256:
            glyphs[v["ascii"]] = v
    out = bytearray(struct.pack("<II", w, h))
    for c in range(256):
        g = glyphs.get(c)
        out += struct.pack("<8h", *( [g[k] for k in ("xoffset", "yoffset", "cellwidth", "cellheight",
                                                     "glyphOriginx", "glyphOriginy", "advance", "glyphwidth")]
                                     if g else [0] * 8))
    out += struct.pack("<%dI" % (w * h), *px)
    return bytes(out)


def sounds(outdirs):
    d = os.path.join(DATA, "xboxsound", "ENGLISH")
    xsh = open(os.path.join(d, "menu.XSH"), "rb").read()
    xsd = open(os.path.join(d, "menu.XSD"), "rb").read()
    done = set()
    for i in range(struct.unpack_from("<I", xsh, 8)[0]):
        e = xsh[12 + i * 0x60:12 + (i + 1) * 0x60]
        name = e[:32].split(b"\0")[0].decode().lower()
        if name not in SOUNDS or name in done:
            continue
        done.add(name)
        off, size = struct.unpack_from("<II", e, 0x24)
        f = e.find(b"\x69\x00", 0x30)         # WAVEFORMATEX, Xbox ADPCM
        tag, ch, rate, avg, align, bits = struct.unpack_from("<HHIIHH", e, f)
        fmt = struct.pack("<HHIIHHHH", tag, ch, rate, avg, align, bits, 2, 64)
        body = xsd[off:off + size]
        wav = (b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(body)) + b"WAVEfmt "
               + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(body)) + body)
        src = os.path.join(outdirs[0], name + "_adpcm.wav")
        open(src, "wb").write(wav)
        for o in outdirs:
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", src, "-c:a", "pcm_s16le",
                            os.path.join(o, name + ".wav")], check=True)
        os.remove(src)
        print(name, "%d Hz, %d ch, %d bytes" % (rate, ch, size))


def main():
    data = font()
    for o in OUTS:
        os.makedirs(o, exist_ok=True)
        open(os.path.join(o, "font.bin"), "wb").write(data)
    print("font.bin", len(data))
    sounds(OUTS)


if __name__ == "__main__":
    main()
