"""Xbox level .dds -> the uncompressed .tga the 2002 Windows release reads.

    py -3 tools/dds2tga.py <in.dds> <out.tga> [out.png]

The PC files are type-2 TGAs, bottom-up, 24-bit when opaque and 32-bit
(descriptor 8) with alpha, with the TGA 2.0 footer. Handles DXT1/3/5 and the
uncompressed A8R8G8B8 / R5G6B5 the zoo levels use (top mip only).
"""
import struct
import sys
import zlib


def _565(c):
    r, g, b = c >> 11 & 31, c >> 5 & 63, c & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def _colour_block(d, p, alpha_one_bit):
    c0, c1, bits = struct.unpack_from('<HHI', d, p)
    a, b = _565(c0), _565(c1)
    if c0 > c1 or not alpha_one_bit:
        pal = [a + (255,), b + (255,),
               tuple((2 * x + y + 1) // 3 for x, y in zip(a, b)) + (255,),
               tuple((x + 2 * y + 1) // 3 for x, y in zip(a, b)) + (255,)]
    else:
        pal = [a + (255,), b + (255,), tuple((x + y) // 2 for x, y in zip(a, b)) + (255,), (0, 0, 0, 0)]
    return [pal[bits >> (2 * j) & 3] for j in range(16)]


def decode(d):
    """-> (w, h, rows of (r, g, b, a)) top row first."""
    assert d[:4] == b'DDS '
    h, w = struct.unpack_from('<II', d, 12)
    pf_flags, four, bits = struct.unpack_from('<I4sI', d, 80)
    masks = struct.unpack_from('<4I', d, 92)
    p = 128
    px = [[None] * w for _ in range(h)]
    if four in (b'DXT1', b'DXT3', b'DXT5'):
        for by in range(0, max(h, 4), 4):
            for bx in range(0, max(w, 4), 4):
                if four == b'DXT1':
                    blk = _colour_block(d, p, True)
                    p += 8
                elif four == b'DXT3':
                    al = d[p:p + 8]
                    blk = _colour_block(d, p + 8, False)
                    blk = [c[:3] + (((al[j // 2] >> (4 * (j & 1))) & 15) * 17,) for j, c in enumerate(blk)]
                    p += 16
                else:
                    a0, a1 = d[p], d[p + 1]
                    ab = int.from_bytes(d[p + 2:p + 8], 'little')
                    if a0 > a1:
                        ap = [a0, a1] + [((7 - k) * a0 + k * a1 + 3) // 7 for k in range(1, 7)]
                    else:
                        ap = [a0, a1] + [((5 - k) * a0 + k * a1 + 2) // 5 for k in range(1, 5)] + [0, 255]
                    blk = _colour_block(d, p + 8, False)
                    blk = [c[:3] + (ap[ab >> (3 * j) & 7],) for j, c in enumerate(blk)]
                    p += 16
                for j in range(16):
                    y, x = by + j // 4, bx + j % 4
                    if y < h and x < w:
                        px[y][x] = blk[j]
    elif bits == 32:
        for y in range(h):
            for x in range(w):
                v = struct.unpack_from('<I', d, p)[0]
                p += 4
                a = (v >> 24) & 255 if masks[3] else 255
                px[y][x] = ((v >> 16) & 255, (v >> 8) & 255, v & 255, a)
    elif bits == 16 and masks[:3] == (0xF800, 0x7E0, 0x1F):
        for y in range(h):
            for x in range(w):
                px[y][x] = _565(struct.unpack_from('<H', d, p)[0]) + (255,)
                p += 2
    else:
        raise ValueError('unsupported DDS: %r %d bits masks %r' % (four, bits, masks))
    return w, h, px


def to_tga(w, h, px):
    alpha = any(c[3] != 255 for row in px for c in row)
    out = bytearray(struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, w, h,
                                32 if alpha else 24, 8 if alpha else 0))
    for row in reversed(px):
        if alpha:
            out += b''.join(bytes((c[2], c[1], c[0], c[3])) for c in row)
        else:
            out += b''.join(bytes((c[2], c[1], c[0])) for c in row)
    out += bytes(8) + b'TRUEVISION-XFILE.\0'
    return bytes(out)


def to_png(w, h, px):
    raw = b''.join(b'\0' + b''.join(bytes(c) for c in row) for row in px)
    chunk = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def convert(dds):
    return to_tga(*decode(dds))


if __name__ == '__main__':
    img = decode(open(sys.argv[1], 'rb').read())
    open(sys.argv[2], 'wb').write(to_tga(*img))
    if len(sys.argv) > 3:
        open(sys.argv[3], 'wb').write(to_png(*img))
