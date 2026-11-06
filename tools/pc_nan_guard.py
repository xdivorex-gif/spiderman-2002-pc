"""Patch SpiderMan.exe (2002 PC): no NaN velocity on a zero time step.

    py -3 tools/pc_nan_guard.py <SpiderMan.exe in> <SpiderMan.exe out>

FUN_004C5060 is the per-entity physics step (param_2 = the step's time).
On one path it rebuilds the velocity from how far the entity moved:

  4C54D9 fld [898048] (1.0); fdiv [esp+114] (dt)        ; 1/dt
         ... displacement * (1/dt) + ..., two vector ctors (callee-popped)
  4C553C lea eax,[esp+2C]; mov ecx,ebp (velocity); push eax
  4C5543 call 40A957                                    ; velocity = that
  4C5548 ...

With dt = 0 that is displacement * infinity = NaN. In Bronx Zoo (an Xbox
level) that hit Spider-Man in the first moments: his vertical velocity
went NaN and stayed NaN, so his height was NaN, every trigger counted him
inside (all traps, then level_end -> mission complete) and Kraven's upper
body vanished. A debugger slowing those frames made it go away.

The 6 bytes at 4C54D9 become a jump to a cave in the .text int3 padding:
if dt is +-0, jump to 4C5548 (the velocity keeps its value; the stack is
balanced there, every push in between is popped by its callee); otherwise
do the original fld and continue at 4C54DF. Nothing else changes.

(First attempt, 2026-11-05: zeroing any NaN/inf velocity or acceleration at
the integrator. It hid the NaN but killed gravity and jumping, since the
NaN was regenerated every frame. Replaced by this.)
"""
import struct
import sys

BASE = 0x400000
HOOK = 0x4C54D9
ORIGINAL = bytes.fromhex('d90548808900')           # fld dword [0x898048]
RESUME = 0x4C54DF                                  # the fdiv
SKIP = 0x4C5548                                    # after "velocity = ..."
CAVE = 0x5D9120                                    # inside a 5424-byte 0xCC run at 0x5D9110


def cave_code():
    c = bytearray()
    c += bytes.fromhex('8b842414010000')           # mov eax,[esp+114]   (dt)
    c += bytes.fromhex('a9ffffff7f')               # test eax,7FFFFFFF   (+-0?)
    jz_at = len(c)
    c += bytes.fromhex('0f84') + bytes(4)          # jz SKIP (filled below)
    c += ORIGINAL                                  # fld 1.0
    jmp_at = len(c)
    c += bytes.fromhex('e9') + bytes(4)            # jmp RESUME
    struct.pack_into('<i', c, jz_at + 2, SKIP - (CAVE + jz_at + 6))
    struct.pack_into('<i', c, jmp_at + 1, RESUME - (CAVE + jmp_at + 5))
    return bytes(c)


def patch(data):
    d = bytearray(data)
    h, cv = HOOK - BASE, CAVE - BASE                # .text: file offset == VA - image base
    if bytes(d[h:h + len(ORIGINAL)]) != ORIGINAL:
        raise SystemExit('unexpected bytes at %x (already patched?)' % HOOK)
    cave = cave_code()
    if set(d[cv:cv + len(cave)]) != {0xCC}:
        raise SystemExit('cave at %x is not free padding' % CAVE)
    d[cv:cv + len(cave)] = cave
    d[h:h + len(ORIGINAL)] = bytes.fromhex('e9') + struct.pack('<i', CAVE - (HOOK + 5)) + bytes.fromhex('90')
    return bytes(d)


if __name__ == '__main__':
    src, dst = sys.argv[1:3]
    open(dst, 'wb').write(patch(open(src, 'rb').read()))
    print('patched', dst)
