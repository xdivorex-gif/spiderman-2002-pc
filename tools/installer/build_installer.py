"""Build KravenModInstaller.exe (one file, the mod files embedded).

    py -3 tools/installer/build_installer.py [build_pc_zoo dir] [original SpiderMan.exe]

Run pc_zoo.py first. The payload is game-derived: the .exe goes into
build_pc_zoo/ and must never be committed or published.

Compiled with the C# compiler that ships with Windows' .NET Framework 4
(no download). Before compiling, the installer's byte signature for the
physics fix is checked against an unpatched SpiderMan.exe: it must match
exactly once, at 0x4C54D9, and end at 0x4C5548 (pc_nan_guard.py).
"""
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CSC = Path('C:/Windows/Microsoft.NET/Framework/v4.0.30319/csc.exe')
GAME = Path('C:/Program Files (x86)/Activision/Spider-Man (tm) Movie')


def payload(build):
    """(source file, path inside the game folder)"""
    out = [(build / 'ZOO_B.pkg', 'data/levels/ZOO_B.pkg'),
           (build / 'ZOO_C.pkg', 'data/levels/ZOO_C.pkg'),
           (build / 'animatic/av_zoo.bik', 'animatic/av_zoo.bik'),
           (build / 'movies/kraven.bik', 'movies/kraven.bik')]
    out += [(f, 'data/tips/' + f.name) for f in sorted((build / 'tips').iterdir()) if f.is_file()]
    out += [(f, 'data/' + f.relative_to(build / 'data').as_posix()) for f in sorted((build / 'data').rglob('*')) if f.is_file()]
    for f, _ in out:
        if not f.is_file():
            raise SystemExit('missing ' + str(f))
    return out


def check_signature(src, exe):
    hexs = re.search(r'Sig = Hex\(\s*((?:"[^"]*"\s*\+?\s*)+)\)', src).group(1)
    sig = ''.join(re.findall(r'"([^"]*)"', hexs)).split()
    pat = re.compile(b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in sig), re.S)
    d = exe.read_bytes()
    hits = [m.start() + 0x400000 for m in pat.finditer(d)]
    if hits != [0x4C54D9] or 0x4C54D9 + len(sig) != 0x4C5548:
        raise SystemExit('signature check failed: %s, length %d' % ([hex(h) for h in hits], len(sig)))
    print('signature: one match at 0x4C54D9, %d bytes' % len(sig))


def main():
    build = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent.parent / 'build_pc_zoo'
    exe = Path(sys.argv[2]) if len(sys.argv) > 2 else GAME / 'SpiderMan.exe.before-nan-guard.bak'
    src = (HERE / 'KravenModInstaller.cs').read_text(encoding='utf-8')
    check_signature(src, exe)
    files = payload(build)
    out = build / 'KravenModInstaller.exe'
    rsp = build / 'installer.rsp'
    lines = ['/nologo', '/target:exe', '/platform:anycpu', '/optimize+',
             '/win32manifest:"%s"' % (HERE / 'app.manifest'), '/out:"%s"' % out]
    lines += ['/resource:"%s",payload/%s' % (f, rel) for f, rel in files]
    lines.append('"%s"' % (HERE / 'KravenModInstaller.cs'))
    rsp.write_text('\n'.join(lines), encoding='utf-8')
    r = subprocess.run([str(CSC), '@' + str(rsp)], capture_output=True, text=True)
    print(r.stdout, r.stderr)
    if r.returncode:
        raise SystemExit('csc failed')
    print('%s: %d files embedded, %.1f MB' % (out, len(files), out.stat().st_size / 1e6))


if __name__ == '__main__':
    main()
