"""Bounds-checked reader for the PC edition's resource-pack directory.

Reads the primary resource locations, rather than guessing resource sizes
from the secondary lookup tables. No game data is included with this tool.
"""
from dataclasses import dataclass
from pathlib import Path
import struct


@dataclass(frozen=True)
class Resource:
    hash: int
    type: int
    offset: int
    size: int


class Pack:
    def __init__(self, path):
        self.path = Path(path)
        self.raw = self.path.read_bytes()
        self.resources = []
        self._range(0, 0x2c)
        directory, self.base = self.words(0x18, 2)
        self._range(directory, 0x10 + 0x2bc)
        desc = directory + 0x10
        parents = self.short(desc + 4)
        count = self.short(desc + 12)
        cursor = (desc + 0x2bc + 7) & ~7
        self._range(cursor, parents * 4)
        cursor = (cursor + parents * 4 + 7) & ~7
        self._range(cursor, count * 16)
        if cursor + count * 16 > self.base:
            raise ValueError('resource directory overlaps payload')
        for i in range(count):
            h, kind, offset, size = self.words(cursor + i * 16, 4)
            self._range(self.base + offset, size)
            self.resources.append(Resource(h, kind, self.base + offset, size))

    def _range(self, offset, size):
        if offset < 0 or size < 0 or offset + size > len(self.raw):
            raise ValueError(f'{self.path.name}: invalid range {offset:#x}+{size:#x}')

    def words(self, offset, count):
        self._range(offset, count * 4)
        return struct.unpack_from(f'<{count}I', self.raw, offset)

    def short(self, offset):
        self._range(offset, 2)
        return struct.unpack_from('<H', self.raw, offset)[0]

    def data(self, resource):
        self._range(resource.offset, resource.size)
        return self.raw[resource.offset:resource.offset + resource.size]

    def find(self, hash_, type_):
        matches = [r for r in self.resources if (r.hash, r.type) == (hash_, type_)]
        if len(matches) != 1:
            raise ValueError(f'expected one resource {hash_:08x}/{type_}, got {len(matches)}')
        return self.data(matches[0])

    def animations(self):
        """Embedded named animation records inside PCANIM resources only."""
        for resource in self.resources:
            if resource.type != 1:
                continue
            data = self.data(resource)
            cursor = 0
            while True:
                cursor = data.find(bytes.fromhex('a8390c10'), cursor)
                if cursor < 0:
                    break
                if cursor + 0x60 > len(data):
                    raise ValueError('truncated animation header')
                size = struct.unpack_from('<I', data, cursor + 4)[0]
                # The bank can contain a named, zero-size placeholder.
                # It has no animation bytes to decode or export.
                if size == 0:
                    cursor += 0x60
                    continue
                if size < 0x60 or cursor + size > len(data):
                    raise ValueError('animation extends outside its resource')
                record = data[cursor:cursor + size]
                name = record[12:44].split(b'\0')[0].decode('ascii')
                if not name or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789_' for c in name):
                    raise ValueError('invalid animation name')
                yield name, record
                cursor += size
