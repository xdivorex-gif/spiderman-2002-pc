"""Xbox / IMA ADPCM (mono, 36-byte blocks: int16 sample, uint8 index, pad, then
32 bytes of nibbles, low nibble first: 1 + 64 = 65 samples a block)."""
import struct

STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
         73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
         449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
         2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
         9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]
BLOCK, PER_BLOCK = 36, 65


def decode(data):
    out = []
    for b in range(0, len(data) - BLOCK + 1, BLOCK):
        s, idx = struct.unpack_from('<hB', data, b)
        idx = min(88, idx)
        out.append(s)
        for byte in data[b + 4:b + BLOCK]:
            for n in (byte & 15, byte >> 4):
                step = STEPS[idx]
                d = step >> 3
                if n & 1: d += step >> 2
                if n & 2: d += step >> 1
                if n & 4: d += step
                s = max(-32768, min(32767, s - d if n & 8 else s + d))
                idx = max(0, min(88, idx + INDEX[n & 7]))
                out.append(s)
    return out


def decode_channels(data, channels):
    """Interleaved Xbox ADPCM (the .xss streams: 36 * channels bytes a block,
    one 4-byte header per channel, then 4 bytes of nibbles per channel in
    turn) -> one sample list per channel."""
    if channels == 1:
        return [decode(data)]
    size = BLOCK * channels
    out = [[] for _ in range(channels)]
    for b in range(0, len(data) - size + 1, size):
        for c in range(channels):
            s, idx = struct.unpack_from('<hB', data, b + 4 * c)
            idx = min(88, idx)
            ch = out[c]
            ch.append(s)
            for r in range(8):
                o = b + 4 * channels + 4 * (r * channels + c)
                for byte in data[o:o + 4]:
                    for n in (byte & 15, byte >> 4):
                        step = STEPS[idx]
                        d = step >> 3
                        if n & 1: d += step >> 2
                        if n & 2: d += step >> 1
                        if n & 4: d += step
                        s = max(-32768, min(32767, s - d if n & 8 else s + d))
                        idx = max(0, min(88, idx + INDEX[n & 7]))
                        ch.append(s)
    return out


def encode(pcm):
    """PCM (list of int) -> Xbox ADPCM blocks of 65 samples."""
    out = bytearray()
    idx = 0
    for b in range(0, len(pcm), PER_BLOCK):
        chunk = pcm[b:b + PER_BLOCK]
        chunk += [chunk[-1] if chunk else 0] * (PER_BLOCK - len(chunk))
        s = chunk[0]
        out += struct.pack('<hBB', s, idx, 0)
        nibbles = []
        for x in chunk[1:]:
            step = STEPS[idx]
            diff = x - s
            n = 8 if diff < 0 else 0
            diff = abs(diff)
            d = step >> 3
            if diff >= step: n |= 4; diff -= step; d += step
            if diff >= step >> 1: n |= 2; diff -= step >> 1; d += step >> 1
            if diff >= step >> 2: n |= 1; d += step >> 2
            s = max(-32768, min(32767, s - d if n & 8 else s + d))
            idx = max(0, min(88, idx + INDEX[n & 7]))
            nibbles.append(n)
        for i in range(0, 64, 2):
            out.append(nibbles[i] | nibbles[i + 1] << 4)
    return bytes(out)


def resample(pcm, src, dst):
    if src == dst or not pcm:
        return list(pcm)
    n = int(len(pcm) * dst / src)
    out = []
    for i in range(n):
        x = i * src / dst
        j = int(x)
        f = x - j
        a = pcm[min(j, len(pcm) - 1)]
        b = pcm[min(j + 1, len(pcm) - 1)]
        out.append(int(a + (b - a) * f))
    return out
