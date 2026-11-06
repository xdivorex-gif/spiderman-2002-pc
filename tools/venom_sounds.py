"""Venom's Ultimate Spider-Man sounds in the 2002 level banks (the thugspidey slot).

USM PC keeps them in Sound\\pc\\CITY_ARENA\\VENOM.WBK (effects) and VENOM_EN.WBK
(voice): "WAVEBK11", data from header +0x10, entry count at +0x40, entries of
0x28 bytes from +0x100: name hash (USM's packs\\pc\\string_hash_dictionary.txt
names them), flags, ?, sample count (+0x0C), ..., data offset (+0x1C), sample
rate (+0x20). The data is NOT block ADPCM: USM's player (USM.exe 0x81E320,
codec 1) decodes it as one continuous IMA ADPCM stream, low nibble first,
from a zero state -- no block headers. (Copied in as Xbox ADPCM blocks it
played as a garbled mess.) So each sound is decoded to PCM, resampled to the
banks' 22050 Hz and encoded as Xbox ADPCM blocks (tools/adpcm.py).

2002 level banks (data\\xboxsound\\ENGLISH\\<level>.XSH/.XSD): 0x60-byte entries,
name[32], offset/size in the XSD at +0x24, kind at +0x38 (3 = voice), an Xbox
ADPCM WAVEFORMATEX at +0x4C. As in shocker_build.py: the byte ranges of the
Spider-Man sounds he replaces (SLOTS) take his samples, those entries point at
them with his rate, everything else stays where it was. Written to the
costume overlay, read only while he is worn (src/costume_panel.c).
Not punch_sm / punch_sm2 / swing*: the engine plays those for other
characters' blows too. webshot1 is the pause menu's.
"""
import argparse
import math
import os
from pathlib import Path
import struct

import adpcm
import shocker_build as sb

# Spider-Man's entry -> Venom's USM sound (spiderman_enx.inc sound groups)
SLOTS = {
    # footstep
    'sm_ftsteps1': 'FS_VNWalk_1', 'sm_ftsteps2': 'FS_VNWalk_2',
    'sm_ftsteps3': 'FS_VNWalk_3', 'sm_ftsteps4': 'FS_VNWalk_4',
    # jump_launch (voice), jump_land, swing_bounce (hitting a wall)
    'smaiv059': 'VEN_Exert_Jump_1', 'smaiv060': 'VEN_Exert_Jump_2',
    'smaiv061': 'VEN_Exert_1', 'smaiv062': 'VEN_Exert_2',
    'sm_landing': 'VN_JumpLand_Cement', 'sm_bump': 'VN_WallAttach',
    # attack: his tentacle lashes and swings
    'whoosh1': 'VN_QuickTentacle_Lash', 'whoosh2': 'VenomBigSwing1',
    'whoosh3': 'VenomBigSwing3', 'whoosh4': 'VN_2TentaSmash_Short',
    # pain, death
    'smaiv018': 'VEN_Exert_Pain_1', 'smaiv023': 'VEN_Exert_Pain_2',
    'smaiv024': 'VEN_Exert_Pain_5', 'smaiv025': 'VEN_Exert_3',
    'smaiv092': 'VEN_Exert_Pain_3',
    # healed, health_deny, web_deny
    'smaiv020': 'VEN_Inhale1', 'smaiv021': 'VEN_Exhale1', 'smaiv022': 'VEN_Exhale2',
    # pickup_light / heavy, throw_light / heavy
    'smaiv063': 'VEN_Exert_6', 'smaiv064': 'VEN_Exert_7',
    'smaiv065': 'VEN_Exert_8', 'smaiv066': 'VEN_Exert_3',
    # zips: his tentacle zip-line
    'weblaunch': 'VEN_TentZip', 'weblaunch_advanced': 'VEN_TentZip',
}
ADPCM_BLOCK, ADPCM_SAMPLES = 36, 64


def usm_names(usm_data):
    names = {}
    for line in open(usm_data / 'packs' / 'pc' / 'string_hash_dictionary.txt', encoding='latin1'):
        p = line.strip().split('\t')
        if len(p) == 2 and p[0].startswith('0x'):
            names[int(p[0], 16)] = p[1]
    return names


BANK_RATE = 22050


def ima_stream(data, samples):
    """USM's codec 1: continuous IMA ADPCM, low nibble first, zero state."""
    out, s, idx = [], 0, 0
    for byte in data:
        for n in (byte & 15, byte >> 4):
            step = adpcm.STEPS[idx]
            d = step >> 3
            if n & 1: d += step >> 2
            if n & 2: d += step >> 1
            if n & 4: d += step
            s = max(-32768, min(32767, s - d if n & 8 else s + d))
            idx = max(0, min(88, idx + adpcm.INDEX[n & 7]))
            out.append(s)
            if len(out) == samples:
                return out
    return out


def usm_bank(path, names):
    """name -> (BANK_RATE, Xbox ADPCM bytes): decoded, resampled, re-encoded."""
    d = path.read_bytes()
    if d[:8] != b'WAVEBK11':
        raise ValueError(f'{path.name}: not a WAVEBK11 bank')
    base, size_all = struct.unpack_from('<2I', d, 0x10)
    count = struct.unpack_from('<I', d, 0x40)[0]
    ents = [struct.unpack_from('<10I', d, 0x100 + i * 0x28) for i in range(count)]
    starts = sorted(e[7] for e in ents) + [size_all]
    out = {}
    for e in ents:
        h, flags, _, samples, _, _, _, off, rate, _ = e
        end = min(x for x in starts if x > off)
        if base + end > len(d):
            raise ValueError(f'{path.name}: entry outside the bank')
        pcm = adpcm.resample(ima_stream(d[base + off:base + end], samples), rate, BANK_RATE)
        out[names.get(h, f'{h:08x}')] = (BANK_RATE, adpcm.encode(pcm))
    return out


def venom_bank(xsh, his):
    """One level bank with his sounds in Spider-Man's: (XSH, XSD, placed) or None."""
    hdr, ents = sb.bank_entries(xsh)
    data = bytearray(open(str(xsh)[:-4] + '.XSD', 'rb').read())
    slots = [e for e in ents if sb.entry_name(e) in SLOTS]
    if not slots:
        return None
    taken = set()
    for e in ents:
        if e not in slots:
            off, size = struct.unpack_from('<II', e, 0x24)
            taken.update(range(off // ADPCM_BLOCK, (off + size + ADPCM_BLOCK - 1) // ADPCM_BLOCK))
    blocks = set()
    for e in slots:
        off, size = struct.unpack_from('<II', e, 0x24)
        blocks.update(range(off // ADPCM_BLOCK, (off + size) // ADPCM_BLOCK))
    free = []
    for b in sorted(blocks - taken):
        if free and free[-1][0] + free[-1][1] == b * ADPCM_BLOCK:
            free[-1][1] += ADPCM_BLOCK
        else:
            free.append([b * ADPCM_BLOCK, ADPCM_BLOCK])
    placed = {}
    for snd in sorted({SLOTS[sb.entry_name(e)] for e in slots}, key=lambda s: -len(his[s][1])):
        rate, pcm = his[snd]
        fits = [r for r in free if r[1] >= len(pcm)]
        if not fits and not free:
            break
        r = min(fits, key=lambda r: r[1]) if fits else max(free, key=lambda r: r[1])
        n = min(len(pcm), r[1]) // ADPCM_BLOCK * ADPCM_BLOCK
        if not n:
            continue
        data[r[0]:r[0] + n] = pcm[:n]
        placed[snd] = (r[0], n, rate)
        r[0] += n
        r[1] -= n
    for e in slots:
        snd = SLOTS[sb.entry_name(e)]
        if snd in placed:
            off, n, rate = placed[snd]
            struct.pack_into('<II', e, 0x24, off, n)
            tag, ch = struct.unpack_from('<HH', e, 0x4C)
            if tag != 0x69 or ch != 1:
                raise ValueError(f'{sb.entry_name(e)}: not mono Xbox ADPCM')
            struct.pack_into('<II', e, 0x50, rate, rate * ADPCM_BLOCK // ADPCM_SAMPLES)
    return bytes(hdr) + b''.join(ents), bytes(data), placed


def build(usm_data, game_data, out_dir):
    names = usm_names(usm_data)
    snd = usm_data / 'Sound' / 'pc' / 'CITY_ARENA'
    his = {**usm_bank(snd / 'VENOM.WBK', names), **usm_bank(snd / 'VENOM_EN.WBK', names)}
    missing = sorted(set(SLOTS.values()) - set(his))
    if missing:
        raise ValueError(f'not in his banks: {missing}')
    src = game_data / 'xboxsound' / 'ENGLISH'
    dst = out_dir / 'data' / 'xboxsound' / 'ENGLISH'
    dst.mkdir(parents=True, exist_ok=True)
    done, cut = 0, set()
    for f in sorted(os.listdir(src)):
        if not f.upper().endswith('.XSH') or f.lower() == 'menu.xsh':
            continue
        out = venom_bank(src / f, his)
        if not out:
            continue
        xsh, xsd, placed = out
        for s, (off, n, rate) in placed.items():
            if n < len(his[s][1]):
                cut.add(s)
        (dst / f).write_bytes(xsh)
        (dst / (f[:-4] + '.XSD')).write_bytes(xsd)
        done += 1
    print('sound: %d level banks with his sounds%s'
          % (done, '; cut short: ' + ', '.join(sorted(cut)) if cut else ''))


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--usm-data', required=True, type=Path, help='Ultimate Spider-Man\\Data')
    ap.add_argument('--game-data', required=True, type=Path, help='the 2002 game\'s data folder')
    ap.add_argument('--output', required=True, type=Path, help='costume overlay folder (overlay\\thugspidey)')
    a = ap.parse_args()
    build(a.usm_data, a.game_data, a.output)
