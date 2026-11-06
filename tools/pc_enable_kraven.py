"""Enable the existing Kraven menu/progression gate, preserving other settings."""
from pathlib import Path
import argparse
import re

TARGETS = [
    Path('C:/Program Files (x86)/Activision/Spider-Man (tm) Movie/game.ini'),
    Path('C:/Users/adub9/AppData/Local/VirtualStore/Program Files (x86)/Activision/Spider-Man (tm) Movie/game.ini'),
]


def enable(data):
    text = data.decode('latin1')
    section = re.search(r'(?im)^\[Flags\][ \t]*(?=\r?$)', text)
    if section is None:
        raise ValueError('No [Flags] section; refusing to guess')
    next_section = re.search(r'(?m)^\[', text[section.end():])
    end = section.end() + next_section.start() if next_section else len(text)
    body = text[section.end():end]
    flag = re.compile(r'(?im)^[ \t]*BONUS_LEVELS_AVAILABLE[ \t]*=[^\r\n]*')
    if flag.search(body):
        body = flag.sub('BONUS_LEVELS_AVAILABLE=1', body)
    else:
        newline = '\r\n' if '\r\n' in text else '\n'
        body = newline + 'BONUS_LEVELS_AVAILABLE=1' + body
    return (text[:section.end()] + body + text[end:]).encode('latin1')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    plans = [(p, p.read_bytes()) for p in TARGETS if p.exists()]
    plans = [(p, old, enable(old)) for p, old in plans]
    for path, old, new in plans:
        assert enable(new) == new
        if new == old:
            print('Already enabled:', path)
            continue
        print('Enable BONUS_LEVELS_AVAILABLE:', path)
        if args.apply:
            backup = path.with_name(path.name + '.before-kraven-levels.bak')
            if backup.exists():
                if backup.read_bytes() != old:
                    raise ValueError('Existing backup differs: ' + str(backup))
            else:
                with backup.open('xb') as f:
                    f.write(old)
            assert backup.read_bytes() == old
            path.write_bytes(new)
            assert path.read_bytes() == new
            print('Saved and verified; original backup:', backup)


if __name__ == '__main__':
    main()
