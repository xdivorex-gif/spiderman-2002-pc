"""Read and write the level scripts: <level>.sx (string table and object /
function directory, text) and <level>.sxl (the bytecode, binary).

    strngtbl / N / N quoted strings
    scrobjs
      scrobj <name> standard|global statsize <n> datasize <n> Nfuncs <F>
        F x xecutabl <name> [Nparms <P> <P numbers>] parmsize <n>
              Nstrings <K> <K string numbers> codesiz <bytes> code <offset in .sxl>
    chunkend

Names are string numbers. A function's code uses its own string list, so the
bytecode never holds a global string number; library calls are made by name
through those strings.
"""
import sys


def parse(sx):
    t = [l.rstrip('\r') for l in sx.decode('latin1').split('\n')]
    i = 0

    def nxt():
        nonlocal i
        i += 1
        return t[i - 1]
    assert nxt() == 'strngtbl'
    n = int(nxt())
    strings = [nxt() for _ in range(n)]
    assert nxt() == 'scrobjs'
    objs = []
    while True:
        k = nxt()
        if k == '':
            k = nxt()
        if k == 'chunkend':
            break
        assert k == 'scrobj', (i, k)
        o = {'name': int(nxt()), 'kind': nxt()}
        assert nxt() == 'statsize'
        o['statsize'] = int(nxt())
        assert nxt() == 'datasize'
        o['datasize'] = int(nxt())
        assert nxt() == 'Nfuncs'
        o['funcs'] = []
        for _ in range(int(nxt())):
            assert nxt() == 'xecutabl'
            f = {'name': int(nxt()), 'parms': None}
            k = nxt()
            if k == 'Nparms':
                f['parms'] = [int(nxt()) for _ in range(int(nxt()))]
                k = nxt()
            assert k == 'parmsize'
            f['parmsize'] = int(nxt())
            assert nxt() == 'Nstrings'
            f['strings'] = [int(nxt()) for _ in range(int(nxt()))]
            assert nxt() == 'codesiz'
            f['codesize'] = int(nxt())
            assert nxt() == 'code'
            f['code'] = int(nxt())
            o['funcs'].append(f)
        objs.append(o)
    return strings, objs, t[i:]


def write(strings, objs):
    """The .sx text, laid out as the game's own files are (blank line before
    every object after the first, and before chunkend)."""
    out = ['strngtbl', str(len(strings))] + strings + ['scrobjs']
    for n, o in enumerate(objs):
        if n:
            out.append('')
        out += ['scrobj', str(o['name']), o['kind'], 'statsize', str(o['statsize']),
                'datasize', str(o['datasize']), 'Nfuncs', str(len(o['funcs']))]
        for f in o['funcs']:
            out += ['xecutabl', str(f['name'])]
            if f['parms'] is not None:
                out += ['Nparms', str(len(f['parms']))] + [str(p) for p in f['parms']]
            out += ['parmsize', str(f['parmsize']), 'Nstrings', str(len(f['strings']))]
            out += [str(s) for s in f['strings']]
            out += ['codesiz', str(f['codesize']), 'code', str(f['code'])]
    out += ['', 'chunkend', '']
    return '\n'.join(out).encode('latin1')


if __name__ == '__main__':
    s, o, tail = parse(open(sys.argv[1], 'rb').read())
    for ob in o:
        print(s[ob['name']], ob['kind'], len(ob['funcs']))
