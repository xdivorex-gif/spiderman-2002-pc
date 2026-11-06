"""Initial scalar frames for USM entropy channels (0x5f15d0).

Does not decode subsequent entropy deltas or reconstruct channel-specific IK.
"""
import struct

DECODER_NAMES = {
    '7e916d6a': 'TorsoHead TwoNeck Entropy Compressed',
    'a556994f': 'Legs&Feet IK Entropy Compressed',
    'f0ad5c8e': 'Arm&Hands IK Entropy Compressed',
    'e7d9a8d3': 'Five Finger Top 2 Knuckle Curl Entropy Compressed',
    '464a04d8': 'Tentacles Compressed',
    'c5e45dcf': 'ArbitraryPO',
    'ec4755bd': 'Generic',
}

class Bits:
    def __init__(self, data):
        self.data = data
        self.position = 0
    def read(self, count):
        if self.position + count > len(self.data) * 8:
            raise ValueError('truncated animation bitstream')
        start, shift = divmod(self.position, 8)
        end = (self.position + count + 7) // 8
        result = (int.from_bytes(self.data[start:end], 'little') >> shift) & ((1 << count) - 1)
        self.position += count
        return result
    def signed_magnitude(self, width):
        sign = self.read(1)
        value = self.read(width)
        return -value if sign else value


def initial_scalars(stream, descriptors, scale=1.0, wide=False):
    # 0x853160 initializes 0x96a698 to float 0x3a800000 = 1/1024.
    # Animation +0x54 multiplies that base scale; q0 has a further factor 1/4.
    widths = (4, 7, 12, 30) if wide else (2, 4, 7, 20)
    reader = Bits(stream)
    initial = [reader.signed_magnitude(widths[3]) * scale / 4096 for _ in descriptors]
    velocity = [reader.signed_magnitude(widths[d >> 6]) * scale / 1024 for d in descriptors]
    return dict(initial=initial, velocity=velocity, bits_consumed=reader.position,
                delta_modes=[d & 63 for d in descriptors])
