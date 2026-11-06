"""USM on-disk channel tables, traced through 0x734060 and 0x7378a0.

These are structural readers, not an animation decoder. Animation +0x40
contains presence flags, NOT NAL track type identifiers.
"""
import struct


def word(data, offset):
    if offset < 0 or offset + 4 > len(data):
        raise ValueError('channel word outside resource')
    return struct.unpack_from('<I', data, offset)[0]


def relative_table(data, offset):
    count = word(data, offset)
    if count > (len(data) - offset - 4) // 4:
        raise ValueError('channel table outside resource')
    result = []
    for i in range(count):
        target = offset + word(data, offset + 4 + i * 4)
        if not offset + 4 + count * 4 <= target < len(data):
            raise ValueError('channel payload outside resource or inside table')
        result.append(target)
    return result


def animation_channels(data, channel_count):
    flags_at = word(data, 0x40)
    metadata = relative_table(data, word(data, 0x44))
    streams = relative_table(data, word(data, 0x48))
    mi = si = 0
    channels = []
    for i in range(channel_count):
        flags = word(data, flags_at + i * 4)
        if flags & ~3:
            raise ValueError('unknown animation channel flags')
        if flags & 2 and mi >= len(metadata) or flags & 1 and si >= len(streams):
            raise ValueError('channel flag count exceeds table')
        channels.append(dict(index=i, flags=flags,
                             metadata=metadata[mi] if flags & 2 else None,
                             stream=streams[si] if flags & 1 else None))
        mi += bool(flags & 2)
        si += bool(flags & 1)
    if mi != len(metadata) or si != len(streams):
        raise ValueError('channel flag count does not consume tables')
    return channels


def skeleton_channels(data):
    count = word(data, 0x64)
    at = word(data, 0x70)
    metadata = relative_table(data, word(data, 0x74))
    mi = 0
    result = []
    for i in range(count):
        channel_id, decoder_hash, flags = (word(data, at + i * 12 + k * 4) for k in range(3))
        if flags & 4 and mi >= len(metadata):
            raise ValueError('skeleton metadata count mismatch')
        result.append(dict(index=i, channel_id=channel_id,
                           decoder_hash=f'{decoder_hash:08x}', flags=flags,
                           metadata=metadata[mi] if flags & 4 else None))
        mi += bool(flags & 4)
    if mi != len(metadata):
        raise ValueError('unused skeleton metadata entries')
    return result
