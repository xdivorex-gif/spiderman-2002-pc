"""Disassemble level-script bytecode (.sxl) with the .sx string tables.

    py -3 tools/sxdis.py <level.sx> <level.sxl> <object> [function ...]

Instruction (SpiderMan.exe 0x7E9C70, the PC VM; the Xbox one is the same):
  u16 word: low 7 bits = operand mode, bit 7 = a u16 size follows,
            high byte = opcode.
  operand bytes by mode: 1-3 -> 4, 4-7, 0x0F, 0x10 -> 2, 8-0x0B -> 4, else 0.
Known opcodes: 0x02 branch if false, 0x03 jump (signed 16-bit offset from
after the instruction), 0x04 library call (mode 10: class string, function
string, both indices into the function's string list), 0x05 script call
(mode 9: object string, function index in that object), 0x1D push,
0x1C push address, 0x1E return.
Strings are the function's own list (Nstrings), resolved to text here.
"""
import struct
import sys

import sx

OPS = {0x00: 'add', 0x01: 'and', 0x02: 'bf', 0x03: 'jmp', 0x04: 'lib', 0x05: 'call', 0x06: 'op6',
       0x07: 'op7', 0x08: 'dec', 0x09: 'div', 0x0A: 'mov', 0x0B: 'eq', 0x1C: 'pusha', 0x1D: 'push',
       0x1E: 'ret', 0x21: 'pop'}


def operand_size(mode):
    if mode in (1, 2, 3, 8, 9, 10, 11):
        return 4
    if mode in (4, 5, 6, 7, 0x0F, 0x10):
        return 2
    return 0


def disasm(code, strs, objs_by_name=None, all_strings=None):
    out = []
    p = 0
    while p + 2 <= len(code):
        w = struct.unpack_from('<H', code, p)
        w = w[0]
        mode, op = w & 0x7F, w >> 8
        q = p + 2
        size = None
        if w & 0x80:
            size = struct.unpack_from('<H', code, q)[0]
            q += 2
        n = operand_size(mode)
        arg = code[q:q + n]
        q += n
        name = OPS.get(op, 'op%02x' % op)
        text = ''
        if op == 4 and mode == 10:
            a, b = struct.unpack('<2H', arg)
            text = '%s.%s' % (strs[a] if a < len(strs) else a, strs[b] if b < len(strs) else b)
        elif op == 5 and mode == 9:
            a, b = struct.unpack('<2H', arg)
            obj = strs[a] if a < len(strs) else a
            fn = b
            if objs_by_name and obj in objs_by_name and b < len(objs_by_name[obj]['funcs']):
                fn = '%d:%s' % (b, all_strings[objs_by_name[obj]['funcs'][b]['name']])
            text = '%s.%s' % (obj, fn)
        elif op in (2, 3) and n == 2:
            text = '-> %d' % (q + struct.unpack('<h', arg)[0])
        elif mode == 1 and n == 4:
            hi, lo = struct.unpack('<2H', arg)       # the VM joins them high first
            text = '%g' % struct.unpack('<f', struct.pack('<I', hi << 16 | lo))[0]
        elif n == 2:
            text = '%d' % struct.unpack('<h', arg)[0]
        elif n == 4:
            a, b = struct.unpack('<2H', arg)
            if mode == 8 and a < len(strs):
                text = 'str %s +%d' % (strs[a], b)
            else:
                text = '%d, %d' % (a, b)
        out.append('%5d  %-6s m%-2d%s %s' % (p, name, mode, (' s%d' % size) if size is not None else '', text))
        p = q
    return out


def main():
    sxf, sxlf, obj = sys.argv[1:4]
    want = sys.argv[4:]
    strings, objs, _ = sx.parse(open(sxf, 'rb').read())
    code = open(sxlf, 'rb').read()
    by = {strings[o['name']]: o for o in objs}
    o = by['"%s"' % obj]
    for f in o['funcs']:
        fname = strings[f['name']].strip('"')
        if want and fname not in want:
            continue
        strs = [strings[i] for i in f['strings']]
        print('== %s.%s  (%d bytes, parms %s)' % (obj, fname, f['codesize'], f['parms']))
        for line in disasm(code[f['code']:f['code'] + f['codesize']], strs, by, strings):
            print(line)


if __name__ == '__main__':
    main()
