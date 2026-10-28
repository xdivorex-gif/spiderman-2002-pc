"""Read, edit and write Spider-Man (2002, Xbox) .xbs packages.

    py -3 tools/xbs.py list <file.xbs>
    py -3 tools/xbs.py roundtrip <in.xbs> <out.xbs>

A package (hero, level, menu) is a 0x40-byte header and three zlib streams,
each starting on a 0x20 boundary, the file padded with 0x20 zero bytes:

  header (dwords):
    +0x00 file offset of stream 2      +0x04 0x55AFE (all files seen)
    +0x08 directory entry count        +0x0C 0x40 (header size)
    +0x10 file offset of stream 0      +0x14 stream 0 size, unpacked
    +0x18 end of stream 2 (packed), rounded up to 4
    +0x1C 3                            +0x20 file offset of stream 1
    +0x24 stream 1 size, unpacked      +0x28 7
    +0x2C stream 2 size, packed        +0x30 stream 0 size, packed
    +0x34 stream 1 size, packed

  stream 0: binary records (XBXM meshes on 0x1000 boundaries, ANMX
            animations, DDS textures)
  stream 1: text records (.ent, .ani, .mbo, .ai, .tbb ...)
  stream 2: the directory, 64 bytes per entry, sorted by the key read as
            four little-endian uint64:
    +0x00 key: the name packed (pack_name)
    +0x20 offset in its stream         +0x24 size
    +0x28 type: 0x101 text, 0x103 .anml, 0x107 .xbmesh, 0x108 .dds
    +0x2C stream: 0x100 = stream 1 (text), 0x101 = stream 0 (binary)
    +0x30 0                            +0x34 space taken, size rounded up
    +0x38 load index (0 .. count-1)    +0x3C 0
"""
import struct
import sys
import zlib

_CODES = {}
for _c in "123456789":
    _CODES[_c] = ord(_c) - 48
_CODES.update({"0": 10, "_": 12, ".": 60})
for _c in "abcdefghijklmnopqrstuvwxyz":
    _CODES[_c] = ord(_c) - 81
_CHARS = {v: k for k, v in _CODES.items()}

TYPE_TEXT, TYPE_ANML, TYPE_MESH, TYPE_DDS = 0x101, 0x103, 0x107, 0x108
IN_TEXT, IN_BINARY = 0x100, 0x101


def pack_name(name):
    """Name -> 32-byte key: 10-character chunks, each a uint64 of the length
    in the low 4 bits and the characters as 6-bit digits above, first
    character highest, short chunks padded with 0."""
    out = b""
    for i in range(0, 40, 10):
        part = name[i:i + 10].lower()
        v = 0
        for j in range(10):
            v = v * 64 + (_CODES[part[j]] if j < len(part) else 0)
        out += ((len(part) + 16 * v) & (2 ** 64 - 1)).to_bytes(8, "little") if part else bytes(8)
    return out[:32]


def unpack_name(key):
    s = ""
    for i in range(4):
        x = int.from_bytes(key[i * 8:i * 8 + 8], "little")
        if not x:
            break
        n, v = x & 15, x >> 4
        part = "".join(_CHARS.get((v >> (6 * (9 - j))) & 63, "?") for j in range(10))
        s += part[:n or 10]
    return s


def _sort_key(key):
    return struct.unpack("<4Q", key)


class Entry:
    def __init__(self, key, raw):
        self.key = key
        (self.offset, self.size, self.type, self.stream, self.f30,
         self.space, self.index, self.f3c) = struct.unpack_from("<8I", raw, 32)

    @property
    def name(self):
        return unpack_name(self.key)

    def pack(self):
        return self.key + struct.pack("<8I", self.offset, self.size, self.type, self.stream,
                                      self.f30, self.space, self.index, self.f3c)


class Package:
    def __init__(self, path):
        d = open(path, "rb").read()
        self.header = list(struct.unpack_from("<16I", d, 0))
        h = self.header
        self.s0 = bytearray(zlib.decompress(d[h[4]:h[4] + h[12]]))
        self.s1 = bytearray(zlib.decompress(d[h[8]:h[8] + h[13]]))
        s2 = zlib.decompress(d[h[0]:h[0] + h[11]])
        assert len(self.s0) == h[5] and len(self.s1) == h[9] and len(s2) == 64 * h[2]
        self.entries = [Entry(s2[i:i + 32], s2[i:i + 64]) for i in range(0, len(s2), 64)]

    def find(self, name):
        key = pack_name(name)
        for e in self.entries:
            if e.key == key:
                return e
        return None

    def stream_of(self, e):
        return self.s0 if e.stream == IN_BINARY else self.s1

    def data(self, name):
        e = self.find(name)
        return bytes(self.stream_of(e)[e.offset:e.offset + e.size])

    def put(self, name, data, type_=None, align=0x20):
        """Set an entry's data: appended to the end of its stream (the old
        bytes stay, unreferenced). A new name needs type_; it is added at
        the end of the load order."""
        e = self.find(name)
        if e is None:
            if type_ is None:
                raise KeyError(name)
            e = Entry(pack_name(name), bytes(64))
            e.type, e.stream = type_, IN_TEXT if type_ == TYPE_TEXT else IN_BINARY
            e.index = len(self.entries)
            self.entries.append(e)
            self.entries.sort(key=lambda x: _sort_key(x.key))
        s = self.stream_of(e)
        s.extend(bytes(-len(s) % align))
        e.offset, e.size = len(s), len(data)
        e.space = (len(data) + 15) & ~15
        s.extend(data)
        s.extend(bytes(e.space - len(data)))
        return e

    def write(self, path, level=9):
        h = list(self.header)
        s2 = b"".join(e.pack() for e in self.entries)
        z0, z1, z2 = (zlib.compress(bytes(s), level) for s in (self.s0, self.s1, s2))
        out = bytearray(0x40)
        def place(z):
            out.extend(bytes(-len(out) % 0x20))
            at = len(out)
            out.extend(z)
            return at
        h[4], h[8], h[0] = place(z0), place(z1), place(z2)
        h[5], h[9], h[2] = len(self.s0), len(self.s1), len(self.entries)
        h[12], h[13], h[11] = len(z0), len(z1), len(z2)
        h[6] = (len(out) + 3) & ~3
        out.extend(bytes(h[6] - len(out) + 0x20))
        struct.pack_into("<16I", out, 0, *h)
        open(path, "wb").write(out)


def main(argv):
    if len(argv) >= 2 and argv[0] == "list":
        p = Package(argv[1])
        for e in p.entries:
            print("%-40s %5X %4d %8X %7d" % (e.name, e.type, e.index, e.offset, e.size))
    elif len(argv) == 3 and argv[0] == "roundtrip":
        p = Package(argv[1])
        p.write(argv[2])
        q = Package(argv[2])
        same = (p.s0 == q.s0 and p.s1 == q.s1
                and [e.pack() for e in p.entries] == [e.pack() for e in q.entries])
        print("streams and directory identical" if same else "MISMATCH")
    else:
        print(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
