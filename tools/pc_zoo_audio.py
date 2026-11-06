"""The Bronx Zoo levels' sounds for the 2002 Windows release.

    py -3 tools/pc_zoo_audio.py <xbox data dir> <pc data dir> <out dir> <radvideo64.exe>

The PC's COMMON.pkg SOUNDS\\GLOBAL.SND already declares every zoo sound by
path ("audio\\characters\\kraven\\krbzb003 voice bink", "audio\\music\\
bronx_zoo_main loop cd music volume 60 bink", "audio\\misc_sfx\\x volume 80"
= a .wav); the retail install just lacks the files. The Xbox has them in
data\\xboxsound\\ENGLISH: the level banks zoo_b/zoo_c.XSH (entries of 0x60
bytes after a 12-byte header: name[32], offset/size in the .XSD at +0x24,
an Xbox ADPCM WAVEFORMATEX at +0x4C) and .xss streams (RIFF WAVE, Xbox
ADPCM, music 44.1 kHz stereo, cut-scenes 22.05 kHz stereo).

Each missing file is decoded (tools/adpcm.py; ffmpeg's decoder drops one
sample of every 65) and written as the PC's own files are:
  * "bink" entries: a Bink 1 file with a 4x4, 15 fps black video and the
    sound at its own rate, made with RAD Video Tools as 'BIKi' with the
    original Bink audio (/BinkAudio1Deprecated: audio flags 0xC000 mono /
    0xE000 stereo like the retail files). The default newer DCT audio
    (flag 0x1000) makes the 2002 binkw32.dll call through garbage and crash;
  * the rest: 16-bit PCM .wav.
Voice lines go in the character's ENGLISH folder (the game looks there and
in the folder itself). A file the PC already has is left alone.

The five zoo cut-scene tracks (audio\\levels\\m7bronxzoo\\zoo_b_in, zoo_b_1,
zoo_b_2, zoo_c_in, zoo_c_out) are made too, but GLOBAL.SND has their lines
commented out, so the game does not play them until those lines are on.
"""
import os
import struct
import subprocess
import sys
import tempfile
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adpcm  # noqa: E402
import pc_pkg  # noqa: E402

BS = '\\'
BANKS = ('zoo_b', 'zoo_c')
STREAMS = {   # .xss -> GLOBAL.SND name (cut-scenes are commented out there)
    'bronx_zoo_main': None, 'bronx_zoo_edit2': None,
    'zoo_b_in': 'audio\\levels\\m7bronxzoo\\zoo_b_in voice cd volume 90 bink',
    'zoo_b_1': 'audio\\levels\\m7bronxzoo\\zoo_b_1 voice cd volume 90 bink',
    'zoo_b_2': 'audio\\levels\\m7bronxzoo\\zoo_b_2 voice cd volume 90 bink',
    'zoo_c_in': 'audio\\levels\\m7bronxzoo\\zoo_c_in voice cd volume 90 bink',
    'zoo_c_out': 'audio\\levels\\m7bronxzoo\\zoo_c_out voice cd volume 90 bink',
}


def sound_list(pc_data):
    c = pc_pkg.Pkg(os.path.join(pc_data, 'COMMON.pkg'))
    text = [c.data(i) for i in range(len(c.files)) if c.files[i][0].upper() == 'GLOBAL.SND'][0]
    out = {}
    for line in text.decode('latin1').splitlines():
        t = line.strip().split()
        if t and not t[0].startswith(';'):
            out[t[0].split(BS)[-1].lower()] = (t[0], t[1:])
    return out


def bank(xbox_sound, name):
    h = open(os.path.join(xbox_sound, name + '.XSH'), 'rb').read()
    d = open(os.path.join(xbox_sound, name + '.XSD'), 'rb').read()
    n = struct.unpack_from('<I', h, 8)[0]
    for k in range(n):
        o = 12 + 0x60 * k
        nm = h[o:o + 32].split(b'\0')[0].decode('latin1').lower()
        off, size = struct.unpack_from('<2I', h, o + 0x24)
        tag, ch, rate = struct.unpack_from('<HHI', h, o + 0x4C)
        yield nm, tag, ch, rate, d[off:off + size]


def xss(path):
    b = open(path, 'rb').read()
    assert b[:4] == b'RIFF' and b[8:12] == b'WAVE'
    f = b.find(b'fmt ')
    tag, ch, rate = struct.unpack_from('<HHI', b, f + 8)
    i = b.find(b'data')
    n = struct.unpack_from('<I', b, i + 4)[0]
    return tag, ch, rate, b[i + 8:i + 8 + n]


def pcm(tag, ch, data):
    if tag != 0x69:
        raise ValueError('not Xbox ADPCM: %#x' % tag)
    chans = adpcm.decode_channels(data, ch)
    n = min(len(c) for c in chans)
    return b''.join(struct.pack('<%dh' % ch, *(c[i] for c in chans)) for i in range(n))


def write_wav(path, ch, rate, frames):
    with wave.open(path, 'wb') as w:
        w.setnchannels(ch)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(frames)


def write_bink(path, ch, rate, frames, radvideo):
    with tempfile.TemporaryDirectory() as tmp:
        wav = os.path.join(tmp, 'a.wav')
        avi = os.path.join(tmp, 'a.avi')
        write_wav(wav, ch, rate, frames)
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i', 'color=c=black:s=4x4:r=15',
                        '-i', wav, '-shortest', '-c:v', 'rawvideo', '-pix_fmt', 'bgr24',
                        '-c:a', 'pcm_s16le', avi], check=True)
        out = os.path.abspath(path)
        subprocess.run([radvideo, 'Bink', avi, out, '/V1', '/BinkAudio1Deprecated', '/O', '/#'], check=True)
    with open(path, 'rb') as f:
        head = f.read(0x34)
    flags = struct.unpack_from('<H', head, 0x32)[0]
    if head[:4] != b'BIKi' or flags & 0x1000:
        # 0x1000 = the newer DCT audio, which crashes the 2002 binkw32.dll
        raise ValueError('%s: %r audio flags %#x' % (path, head[:4], flags))


def exists(pc_data, rel):
    d, n = os.path.split(rel)
    for sub in ('', 'ENGLISH'):
        p = os.path.join(pc_data, d, sub)
        if os.path.isdir(p) and n.lower() in {f.lower() for f in os.listdir(p)}:
            return True
    return False


def build(xbox_data, pc_data, out_dir, radvideo):
    xsnd = os.path.join(xbox_data, 'xboxsound', 'ENGLISH')
    snd = sound_list(pc_data)
    jobs = {}                                    # rel path -> (kind, ch, rate, frames source)
    for name in BANKS:
        for nm, tag, ch, rate, data in bank(xsnd, name):
            if nm in snd and nm not in jobs:
                jobs[nm] = (snd[nm], tag, ch, rate, data)
    for nm, line in STREAMS.items():
        entry = snd.get(nm) or (line.split()[0], line.split()[1:])
        jobs[nm] = (entry,) + xss(os.path.join(xsnd, nm + '.xss'))
    made = []
    for nm, ((path, flags), tag, ch, rate, data) in sorted(jobs.items()):
        bink = 'bink' in flags
        rel = path.replace(BS, os.sep) + ('.bik' if bink else '.wav')
        if exists(pc_data, rel):
            continue
        d, n = os.path.split(rel)
        if 'voice' in flags and d.lower().startswith(os.path.join('audio', 'characters')):
            d = os.path.join(d, 'ENGLISH')
        dst = os.path.join(out_dir, d, n.upper() if bink else n)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        frames = pcm(tag, ch, data)
        if bink:
            write_bink(dst, ch, rate, frames, radvideo)
        else:
            write_wav(dst, ch, rate, frames)
        made.append(os.path.relpath(dst, out_dir))
        print('%-48s %d ch %5d Hz %6.1f s' % (made[-1], ch, rate, len(frames) / 2 / ch / rate))
    print(len(made), 'files')


if __name__ == '__main__':
    build(*sys.argv[1:5])
