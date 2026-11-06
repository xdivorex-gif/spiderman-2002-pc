"""The 2002 Windows release's .pkg archives (SpiderMan.exe FUN_007F7C70).

    u32 dir_count, u32 file_count, u32 data_size
    dir_count x  { char name[0x40]; u32 n; }  each followed by n x u32 file index
    file_count x { char name[0x28]; u32 size; u32 offset; }
    data: the last data_size bytes of the file; offsets are from its start.

Names are NUL-terminated inside fixed fields (the rest is whatever was in the
writer's buffer). A directory names the path its files live under, e.g.
"M8BRIDGE\\TGA\\" + "PARKINGLOTB.TGA". A file not found in a package is read
from the loose data folder instead ("Could not find %s%s in package,
attempting to use actual file...").
"""
import struct
import sys
from pathlib import Path


def cstr(b):
    return b.split(b'\0', 1)[0].decode('latin1')


class Pkg:
    def __init__(self, path):
        d = Path(path).read_bytes()
        ndir, nfile, dsize = struct.unpack_from('<3I', d, 0)
        at = 12
        self.dirs = []
        for _ in range(ndir):
            name = cstr(d[at:at + 0x40])
            n = struct.unpack_from('<I', d, at + 0x40)[0]
            at += 0x44
            idx = list(struct.unpack_from(f'<{n}I', d, at))
            at += 4 * n
            self.dirs.append((name, idx))
        self.files = []
        for _ in range(nfile):
            name = cstr(d[at:at + 0x28])
            size, off = struct.unpack_from('<2I', d, at + 0x28)
            at += 0x30
            self.files.append([name, off, size])
        self.base = len(d) - dsize
        if self.base != at:
            raise ValueError(f'{path}: directory ends at {at:#x}, data starts at {self.base:#x}')
        self.raw = d
        self.dir_of = {}
        for dname, idx in self.dirs:
            for i in idx:
                self.dir_of[i] = dname

    def path(self, i):
        return self.dir_of.get(i, '') + self.files[i][0]

    def data(self, i):
        _, off, size = self.files[i]
        return self.raw[self.base + off:self.base + off + size]

    def items(self):
        for i in range(len(self.files)):
            yield self.path(i), self.data(i)


def write(path, files):
    """files: list of (dir, name, bytes). Directories in first-seen order."""
    dirs, order = {}, []
    for d, n, b in files:
        if d not in dirs:
            dirs[d] = []
            order.append(d)
    data = bytearray()
    recs = []
    for i, (d, n, b) in enumerate(files):
        dirs[d].append(i)
        data.extend(bytes(-len(data) % 4))
        recs.append((n, len(data), len(b)))
        data.extend(b)
    out = bytearray(struct.pack('<3I', len(order), len(files), len(data)))
    for d in order:
        name = d.encode('latin1')
        if len(name) >= 0x40:
            raise ValueError(f'directory name too long: {d}')
        out += name.ljust(0x40, b'\0') + struct.pack('<I', len(dirs[d]))
        out += struct.pack(f'<{len(dirs[d])}I', *dirs[d])
    for n, off, size in recs:
        name = n.encode('latin1')
        if len(name) >= 0x28:
            raise ValueError(f'file name too long: {n}')
        out += name.ljust(0x28, b'\0') + struct.pack('<2I', size, off)
    Path(path).write_bytes(bytes(out + data))


if __name__ == '__main__':
    p = Pkg(sys.argv[1])
    for d, idx in p.dirs:
        print(f'{d}  ({len(idx)} files)')
    if len(sys.argv) > 2:
        for i in range(len(p.files)):
            print(f'  {p.path(i):60s} {p.files[i][2]:9d}')
