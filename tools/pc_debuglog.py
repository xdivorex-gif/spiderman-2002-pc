"""Run retail Spider-Man under a passive Windows debug-event logger.

No game input or executable changes. Records debug output and exception stacks.
"""
import ctypes as C
from ctypes import wintypes as W
from pathlib import Path
import struct
import time

K = C.WinDLL('kernel32', use_last_error=True)
PTR = C.c_void_p


class Startup(C.Structure):
    _fields_ = [('cb', W.DWORD), ('reserved', W.LPWSTR), ('desktop', W.LPWSTR),
                ('title', W.LPWSTR), ('x', W.DWORD), ('y', W.DWORD),
                ('xs', W.DWORD), ('ys', W.DWORD), ('xc', W.DWORD),
                ('yc', W.DWORD), ('fill', W.DWORD), ('flags', W.DWORD),
                ('show', W.WORD), ('reserved2size', W.WORD), ('reserved2', PTR),
                ('stdin', W.HANDLE), ('stdout', W.HANDLE), ('stderr', W.HANDLE)]


class ProcessInfo(C.Structure):
    _fields_ = [('process', W.HANDLE), ('thread', W.HANDLE),
                ('pid', W.DWORD), ('tid', W.DWORD)]


class Event(C.Structure):
    _fields_ = [('code', W.DWORD), ('pid', W.DWORD), ('tid', W.DWORD),
                ('pad', W.DWORD), ('data', C.c_byte * 160)]


K.CreateProcessW.argtypes = [W.LPCWSTR, W.LPWSTR, PTR, PTR, W.BOOL,
                            W.DWORD, PTR, W.LPCWSTR, C.POINTER(Startup), C.POINTER(ProcessInfo)]
K.CreateProcessW.restype = W.BOOL
K.WaitForDebugEvent.argtypes = [C.POINTER(Event), W.DWORD]
K.ContinueDebugEvent.argtypes = [W.DWORD, W.DWORD, W.DWORD]
K.ReadProcessMemory.argtypes = [W.HANDLE, PTR, PTR, C.c_size_t, C.POINTER(C.c_size_t)]
K.OpenThread.argtypes = [W.DWORD, W.BOOL, W.DWORD]
K.OpenThread.restype = W.HANDLE
K.Wow64GetThreadContext.argtypes = [W.HANDLE, PTR]
K.CloseHandle.argtypes = [W.HANDLE]


def read(process, address, size):
    buf = C.create_string_buffer(size)
    count = C.c_size_t()
    K.ReadProcessMemory(process, address, buf, size, C.byref(count))
    return buf.raw[:count.value]


K.WriteProcessMemory.argtypes = [W.HANDLE, PTR, PTR, C.c_size_t, C.POINTER(C.c_size_t)]
K.FlushInstructionCache.argtypes = [W.HANDLE, PTR, C.c_size_t]
K.Wow64SetThreadContext.argtypes = [W.HANDLE, PTR]

# Script library handlers to trace (SpiderMan.exe; each is the handler a
# registration's vtable slot 1 jumps to). At entry [esp+4] is the calling
# script thread's stack object: +8 stack top (arguments below it), +0x10 the
# script instruction pointer (into the loaded .sxl, just after the call).
TRACE = {
    0x540570: ('create_entity(str)', 'str'),
    0x540910: ('create_preloaded_static_entity(str)', 'str'),
    0x53C530: ('apply_explosive_damage A', None),
    0x53C680: ('apply_explosive_damage B', None),
    0x562700: ('create_trigger(entity)', None),
    0x555B60: ('script_mfg.raise_signal(num)', None),
    0x51F8D0: ('begin_level_complete_screens()', None),
    0x5412F0: ('entity.suspend()', 'ent'),
    # bytecode linker (FUN_007E6AE0), "library class not found" paths:
    # edi = the function being linked, esi = just past the class operand
    0x7E6C3B: ('LINK class not found (mode 10)', 'link'),
    0x7E6DC3: ('LINK class not found (mode 11)', 'link'),
}
saved, stepping = {}, {}
WATCH = {'addr': None, 'logged': 0}


def dump_state(process, ent, record):
    """Once, when the hero is first seen with a NaN height: every float in
    his entity record and in the blocks its pointers lead to, NaNs marked."""
    def floats(addr, size):
        raw = read(process, addr, size)
        return [(o, struct.unpack_from('<f', raw, o)[0], struct.unpack_from('<I', raw, o)[0])
                for o in range(0, len(raw) - 3, 4)]
    vals = floats(ent, 0x400)
    nans = [o for o, f, u in vals if f != f]
    record(f'DUMP hero entity {ent:#x}: NaN at offsets {[hex(o) for o in nans]}')
    for o, f, u in vals:
        if 0x01000000 <= u < 0x7FFF0000 and u & 3 == 0:
            sub = floats(u, 0x200)
            if not sub:
                continue
            n = [x for x, g, v in sub if g != g]
            if n:
                sample = ' '.join('%x:%g' % (x, g) for x, g, v in sub[:48])
                record(f'DUMP  entity+{o:#x} -> {u:#x}: NaN at {[hex(x) for x in n][:40]} | {sample}')


def watch_hit(process, tid, record):
    """A Dr0 write trap: log the writer when the hero's height became NaN."""
    thread = K.OpenThread(0x001A, False, tid)
    ctx = C.create_string_buffer(716)
    struct.pack_into('<I', ctx, 0, 0x10017)
    K.Wow64GetThreadContext(thread, ctx)
    dr6 = struct.unpack_from('<I', ctx, 0x14)[0]
    if dr6 & 2 and WATCH.get('vel'):
        v = struct.unpack('<f', read(process, WATCH['vel'], 4))[0]
        WATCH['vlogged'] = WATCH.get('vlogged', 0) + 1
        if v != v and WATCH['vlogged'] < 30:
            eip, esp = (struct.unpack_from('<I', ctx, o)[0] for o in (0xB8, 0xC4))
            regs = {n: hex(struct.unpack_from('<I', ctx, o)[0]) for n, o in
                    (('eax', 0xB0), ('ebx', 0xA4), ('ecx', 0xAC), ('edx', 0xA8), ('esi', 0xA0), ('edi', 0x9C), ('ebp', 0xB4))}
            fsave = ctx.raw[0x1C:0x1C + 112]
            record(f'VEL NAN after eip={eip:#x} regs={regs} code_before={read(process, eip - 64, 64).hex()} '
                   f'stack={read(process, esp, 96).hex()} fpu={fsave.hex()}')
    hit = bool(dr6 & 1)
    if hit:
        y = struct.unpack('<f', read(process, WATCH['addr'], 4))[0]
        first_nan = y != y and not WATCH.get('seen_nan')
        if y != y:
            WATCH['seen_nan'] = True
        if WATCH.get('all') and not WATCH.get('seen_nan') and WATCH['logged'] < 60:
            WATCH['logged'] += 1
            eip = struct.unpack_from('<I', ctx, 0xB8)[0]
            record(f'HEIGHT WRITE {y:g} after eip={eip:#x}')
        if (first_nan or (y != y and WATCH['logged'] < 40)):
            WATCH['logged'] += 1
            eip, ebp, esp = (struct.unpack_from('<I', ctx, o)[0] for o in (0xB8, 0xB4, 0xC4))
            frames = []
            for _ in range(16):
                raw = read(process, ebp, 8)
                if len(raw) != 8:
                    break
                nxt, ret = struct.unpack('<II', raw)
                frames.append(hex(ret))
                if nxt <= ebp or nxt > ebp + 0x100000:
                    break
                ebp = nxt
            regs = {n: struct.unpack_from('<I', ctx, o)[0] for n, o in
                    (('eax', 0xB0), ('ecx', 0xAC), ('edx', 0xA8), ('esi', 0xA0), ('edi', 0x9C))}
            record(f'ACCEL NAN WRITE after eip={eip:#x} esp={esp:#x} regs={ {k: hex(v) for k, v in regs.items()} } '
                   f'stack={read(process, esp, 64).hex()} frames={" ".join(frames)} '
                   f'code_before={read(process, eip - 48, 48).hex()}')
    struct.pack_into('<I', ctx, 0x14, 0)
    K.Wow64SetThreadContext(thread, ctx)
    K.CloseHandle(thread)
    return hit


def write(process, address, data):
    n = C.c_size_t()
    K.WriteProcessMemory(process, address, data, len(data), C.byref(n))
    K.FlushInstructionCache(process, address, len(data))


def cstr(process, address):
    raw = read(process, address, 96).split(b'\0')[0]
    return raw.decode('latin1') if raw and all(32 <= c < 127 for c in raw) else None


def trace_hit(process, tid, address, record):
    """Log one library call, then put the original byte back and single-step
    over it (the breakpoint is re-armed on the step event)."""
    thread = K.OpenThread(0x001A, False, tid)
    ctx = C.create_string_buffer(716)
    struct.pack_into('<I', ctx, 0, 0x10007)
    K.Wow64GetThreadContext(thread, ctx)
    esp = struct.unpack_from('<I', ctx, 0xC4)[0]
    name, kind = TRACE[address]
    if kind == 'link':
        try:
            esi, edi = struct.unpack_from('<I', ctx, 0xA0)[0], struct.unpack_from('<I', ctx, 0x9C)[0]
            code, words, strs = struct.unpack('<3I', read(process, edi + 0x28, 12))
            names = []
            for k in range(12):
                sp = struct.unpack('<I', read(process, strs + 4 * k, 4))[0]
                names.append(cstr(process, sp) or cstr(process, struct.unpack('<I', read(process, sp, 4))[0]) or hex(sp))
            record(f'{name}: func {edi:#x} code {code:#x} words {words} at +{esi - code} '
                   f'bytes {read(process, code, min(2 * words, 160)).hex()} strings {names}')
        except Exception as e:                        # noqa: BLE001
            record(f'{name} (unreadable: {e})')
    try:
        if kind == 'link':
            raise StopIteration
        stk = struct.unpack('<I', read(process, esp + 4, 4))[0]
        top, ip = struct.unpack('<I4xI', read(process, stk + 8, 12))
        arg = ''
        if kind == 'str':
            p = struct.unpack('<I', read(process, top - 4, 4))[0]
            arg = cstr(process, p)
            if arg is None:
                p2 = struct.unpack('<I', read(process, p, 4))[0]
                arg = cstr(process, p2) or hex(p)
        if kind is None:          # numbers / vectors: the 32 bytes below the stack top as floats
            raw = read(process, top - 32, 32)
            arg = ' '.join('%g' % v for v in struct.unpack('<8f', raw))
        if kind == 'ent' and WATCH['addr'] is None:
            # the hero's first suspend() (level_start, before the cut-scene):
            # recognised by its spawn point; watch its height from here on
            ent = struct.unpack('<I', read(process, top - 4, 4))[0]
            mat = struct.unpack('<I', read(process, ent + 0x54, 4))[0]
            x, y, z = struct.unpack('<3f', read(process, mat + 0x30, 12))
            arg = 'entity %#x at (%g, %g, %g)' % (ent, x, y, z)
            if abs(x - 62.46) < 0.5 and abs(z - 30.28) < 0.5:
                phys = struct.unpack('<I', read(process, ent + 0xBC, 4))[0]
                WATCH['addr'] = phys + 0x30          # vertical acceleration
                WATCH['all'] = False
                WATCH['vel'] = phys + 0x18
                struct.pack_into('<I', ctx, 0, 0x10017)
                K.Wow64GetThreadContext(thread, ctx)
                struct.pack_into('<I', ctx, 0x04, WATCH['addr'])
                struct.pack_into('<I', ctx, 0x08, WATCH['vel'])
                struct.pack_into('<I', ctx, 0x18, 0x00DD0005)
                record(f'WATCH early: physics {phys:#x} velocity {WATCH["vel"]:#x} = '
                       f'{struct.unpack("<f", read(process, WATCH["vel"], 4))[0]:g}')
                record(f'WATCH hero entity {ent:#x} matrix {mat:#x} from its first suspend(): height {y:g}')
        if name.startswith('apply_explosive') and WATCH['addr'] is None:
            # entity.apply_explosive_damage(num, vector3d): the entity sits
            # below num and the vector. If it's the hero with a NaN height,
            # watch writes to that height (world matrix at +0x54, y at +0x34).
            ent = struct.unpack('<I', read(process, top - 20, 4))[0]
            mat = struct.unpack('<I', read(process, ent + 0x54, 4))[0]
            x, y, z = struct.unpack('<3f', read(process, mat + 0x30, 12))
            if y != y:
                dump_state(process, ent, record)
                WATCH['addr'] = mat + 0x34
                struct.pack_into('<I', ctx, 0, 0x10017)
                K.Wow64GetThreadContext(thread, ctx)
                phys = struct.unpack('<I', read(process, ent + 0xBC, 4))[0]
                WATCH['vel'] = phys + 0x18
                struct.pack_into('<I', ctx, 0x04, WATCH['addr'])          # Dr0: height
                struct.pack_into('<I', ctx, 0x08, WATCH['vel'])           # Dr1: vertical velocity
                struct.pack_into('<I', ctx, 0x18, 0x00DD0005)             # Dr7: L0+L1, write, 4 bytes
                record(f'WATCH also physics {phys:#x} vertical velocity at {WATCH["vel"]:#x}')
                record(f'WATCH hero entity {ent:#x} matrix {mat:#x} pos ({x:g}, {y:g}, {z:g}): '
                       f'logging NaN writes to {WATCH["addr"]:#x}')
        code = read(process, ip - 32, 32).hex()
        record(f'CALL {name} {arg!r} ip={ip:#x} code={code}')
    except StopIteration:
        pass
    except Exception as e:                            # noqa: BLE001
        record(f'CALL {name} (unreadable: {e})')
    write(process, address, saved[address])
    struct.pack_into('<I', ctx, 0xB8, address)
    flags = struct.unpack_from('<I', ctx, 0xC0)[0]
    struct.pack_into('<I', ctx, 0xC0, flags | 0x100)
    K.Wow64SetThreadContext(thread, ctx)
    K.CloseHandle(thread)


def main():
    import os
    os.environ['_NO_DEBUG_HEAP'] = '1'      # no 0xBAADF00D fills that change behaviour
    game = Path('C:/Program Files (x86)/Activision/Spider-Man (tm) Movie/SpiderMan.exe')
    logfile = Path(__file__).resolve().parent.parent / 'build_pc_zoo' / 'kraven_diagnostic.log'
    si, pi = Startup(), ProcessInfo()
    si.cb = C.sizeof(si)
    command = C.create_unicode_buffer('"' + str(game) + '"')
    with logfile.open('w', encoding='utf8', buffering=1) as log:
        def record(message):
            log.write(time.strftime('%H:%M:%S ') + message + '\n')
        if not K.CreateProcessW(str(game), command, None, None, False, 2, None,
                                str(game.parent), C.byref(si), C.byref(pi)):
            raise C.WinError(C.get_last_error())
        record(f'Launched PID {pi.pid}; input remains with the user.')
        K.DebugSetProcessKillOnExit(False)
        initial_breakpoint = True
        while True:
            event = Event()
            if not K.WaitForDebugEvent(C.byref(event), 1000):
                if C.get_last_error() == 121:
                    continue
                raise C.WinError(C.get_last_error())
            data = bytes(event.data)
            status = 0x00010002
            if event.code == 1:
                code = struct.unpack_from('<I', data)[0]
                address = struct.unpack_from('<Q', data, 16)[0]
                first = struct.unpack_from('<I', data, 152)[0]
                if code in (0x80000003, 0x4000001f) and address in TRACE and first:
                    trace_hit(pi.process, event.tid, address, record)
                    stepping[event.tid] = address
                elif code in (0x80000004, 0x4000001e) and (event.tid in stepping or WATCH['addr']):
                    if WATCH['addr']:
                        watch_hit(pi.process, event.tid, record)
                    if event.tid in stepping:
                        write(pi.process, stepping.pop(event.tid), b'\xcc')
                elif code in (0x80000003, 0x4000001f) and initial_breakpoint:
                    record(f'EXCEPTION code={code:#x} address={address:#x} first_chance={first}')
                    initial_breakpoint = False
                    for a in TRACE:
                        saved[a] = read(pi.process, a, 1)
                        write(pi.process, a, b'\xcc')
                    record('tracing %d script library handlers' % len(TRACE))
                else:
                    record(f'EXCEPTION code={code:#x} address={address:#x} first_chance={first}')
                    status = 0x80010001
                    thread = K.OpenThread(0x0048, False, event.tid)
                    if thread:
                        ctx = C.create_string_buffer(716)
                        struct.pack_into('<I', ctx, 0, 0x10007)
                        if K.Wow64GetThreadContext(thread, ctx):
                            ebp, eip, esp = [struct.unpack_from('<I', ctx, o)[0] for o in (180,184,196)]
                            record(f'EIP={eip:#x} ESP={esp:#x} EBP={ebp:#x}')
                            stack = read(pi.process, esp, 256)
                            record('STACK ' + stack.hex(' '))
                            frames = []
                            for _ in range(20):
                                raw = read(pi.process, ebp, 8)
                                if len(raw) != 8:
                                    break
                                next_frame, ret = struct.unpack('<II', raw)
                                frames.append(hex(ret))
                                if next_frame <= ebp or next_frame > ebp + 0x100000:
                                    break
                                ebp = next_frame
                            record('FRAMES ' + ' '.join(frames))
                        K.CloseHandle(thread)
            elif event.code == 8:
                address, unicode, length = struct.unpack_from('<QHH', data)
                raw = read(pi.process, address, min(length * (2 if unicode else 1), 65536))
                record('DEBUG ' + raw.decode('utf-16-le' if unicode else 'latin1', errors='replace').rstrip('\0\r\n'))
            elif event.code == 6:
                handle, base = struct.unpack_from('<QQ', data)
                name = ''
                if handle:
                    buf = C.create_unicode_buffer(520)
                    if K.GetFinalPathNameByHandleW(W.HANDLE(handle), buf, 520, 0):
                        name = buf.value
                record(f'LOAD_DLL base={base:#x} {name}')
                if handle:
                    K.CloseHandle(handle)
            elif event.code == 3:
                handle = struct.unpack_from('<Q', data)[0]
                if handle:
                    K.CloseHandle(handle)
            elif event.code == 5:
                record(f'EXIT_PROCESS code={struct.unpack_from("<I",data)[0]:#x}')
            K.ContinueDebugEvent(event.pid, event.tid, status)
            if event.code == 5:
                break
        K.CloseHandle(pi.thread)
        K.CloseHandle(pi.process)


if __name__ == '__main__':
    main()
