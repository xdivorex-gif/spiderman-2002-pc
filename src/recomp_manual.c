/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern __declspec(thread) uint32_t g_eax;  /* guest registers are per thread */
extern ptrdiff_t g_xbox_mem_offset;

/* ── D3D GPU semaphore ─────────────────────────────────────── */

/*
 * Where the GPU reports progress. Spider-Man's statically linked D3D (XDK
 * 4134) keeps its device at 0x00300E00, read through the pointer at
 * 0x003038E0; device+0x30 is the newest fence it submitted, and device+0x34
 * points at the semaphore the GPU releases fences into (0x80003000 at boot).
 * Later XDKs move that pointer to +0x30 (d3d8ltcg-device-context.md).
 *
 * The executor writes BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) values only
 * once told where. Untold, the semaphore stayed 0, and the fence wait at
 * 0x002F2B00 spun for good during D3D's first init, with PUT == GET.
 *
 * An entry hook, so it runs on every call: D3D rewrites the field on a
 * device reset. Wants RECOMP_PB_EXEC and the generator's --exclude-manual.
 */
#define SM_D3D_DEVICE_PTR  0x003038E0u
#define SM_D3D_SEMAPHORE   0x34u

void nv2a_pb_set_semaphore_target(uint32_t guest_va);   /* nv2a_backend.h */

#define GUEST32(va) (*(volatile uint32_t *)((uint8_t *)g_xbox_mem_offset + (va)))

void sub_002F2B00_enter(void)
{
    static uint32_t told;
    uint32_t dev = GUEST32(SM_D3D_DEVICE_PTR), sem;

    if (!dev)
        return;
    sem = GUEST32(dev + SM_D3D_SEMAPHORE);
    if (sem && sem != told) {
        nv2a_pb_set_semaphore_target(sem);
        told = sem;
        fprintf(stderr, "  [D3D] GPU semaphore at 0x%08X\n", sem);
        fflush(stderr);
    }
}

/* ── Costume (costume_panel.c) ─────────────────────────────── */

/*
 * sub_000F0690 is the level start that picks the hero from the costume the
 * player profile marks as worn (this = the app object). Entering it, the
 * chosen costume -- settings.ini [Game] Costume, or the pause-menu panel --
 * is written there first. See costume_panel.c.
 */
extern __declspec(thread) uint32_t g_ecx;  /* guest registers are per thread */
void costume_level_start(uint32_t app);

void sub_000F0690_enter(void)
{
    costume_level_start(g_ecx);
}

/* ── The title's own warnings ──────────────────────────────── */

/*
 * sub_00013DE0 is the title's debug print (cdecl: format, arguments), as in
 * "Script bug.  Could not find sound source for %s". The retail build prints
 * nowhere. RECOMP_GAME_LOG=1 sends each distinct message to stderr as
 * "[GAME] ...": %s reads a guest string, the integer conversions a dword,
 * %f a double (two dwords); anything else is printed raw.
 */
#include <stdlib.h>
#include <string.h>

extern __declspec(thread) uint32_t g_esp;

static const char *guest_str(uint32_t va)
{
    if (!va || va >= 0x04000000)
        return "(null)";
    return (const char *)((uint8_t *)g_xbox_mem_offset + va);
}

static void game_log(const char *tag)
{
    static int on = -1;
    static uint32_t seen[256];
    static int nseen;
    char out[512];
    const char *f;
    uint32_t arg = g_esp + 8, h = 2166136261u;
    size_t o = 0;
    int i;

    if (on < 0) {
        const char *e = getenv("RECOMP_GAME_LOG");
        on = e && *e && *e != '0';
    }
    if (!on)
        return;
    f = guest_str(GUEST32(g_esp + 4));
    while (*f && o < sizeof out - 64) {
        char spec[16];
        size_t k = 0;
        if (*f != '%') {
            out[o++] = *f++;
            continue;
        }
        spec[k++] = *f++;
        while (*f && strchr("-+ #0123456789.l", *f) && k < sizeof spec - 2)
            spec[k++] = *f++;
        spec[k++] = *f;
        spec[k] = 0;
        switch (*f) {
        case 's':
            o += _snprintf(out + o, sizeof out - o - 1, spec, guest_str(GUEST32(arg)));
            arg += 4;
            break;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'c':
            o += _snprintf(out + o, sizeof out - o - 1, spec, GUEST32(arg));
            arg += 4;
            break;
        case 'f': case 'g': case 'e': {
            double v;
            uint32_t lo = GUEST32(arg), hi = GUEST32(arg + 4);
            uint64_t bits = ((uint64_t)hi << 32) | lo;
            memcpy(&v, &bits, sizeof v);
            o += _snprintf(out + o, sizeof out - o - 1, spec, v);
            arg += 8;
            break;
        }
        case '%':
            out[o++] = '%';
            break;
        default:
            break;
        }
        if (*f)
            f++;
        if (o > sizeof out - 64)
            break;
    }
    out[o] = 0;
    /* once per distinct message: a warning in a loop would bury the log */
    for (i = 0; out[i]; i++)
        h = (h ^ (uint8_t)out[i]) * 16777619u;
    for (i = 0; i < nseen; i++)
        if (seen[i] == h)
            return;
    if (nseen < 256)
        seen[nseen++] = h;
    fprintf(stderr, "[GAME%s] %s%s", tag, out, (o && out[o - 1] == '\n') ? "" : "\n");
    fflush(stderr);
}

void sub_00013DE0_enter(void)
{
    game_log("");
}

/* sub_00013E20 is its error report: the same arguments, then a debug trap.
 * The script linker (sub_000505F0) sends "<name> not found." here when a
 * script calls a native nobody has registered. */
void sub_00013E20_enter(void)
{
    game_log(" ERROR");
}

/* The autoload stall: the save script's native call links to nothing, as
 * its name comes out of the script string table cut short ("loa" for
 * load_most_recent()). RECOMP_GAME_LOG also shows each script load and
 * link with its host thread, to see whether two overlap. sub_0004DA60
 * loads a script (this = the script); sub_000505F0 (cdecl: stream,
 * script) links its natives, names from the string table at
 * [[0x4C06B8]+0x170+0x20], 8 bytes an entry. */
static int script_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GAME_LOG");
        on = e && *e && *e != '0';
    }
    return on;
}

/* The Shocker's tornado and blast (src/shocker_powers.c): a tick on the
 * game thread from the script engine's entry, and the damage records it
 * turns round. */
void shocker_powers_frame(void);
void shocker_powers_damage_record(void);

void sub_00051C00_enter(void)
{
    shocker_powers_frame();
}

void sub_002B50B4_enter(void)
{
    shocker_powers_damage_record();
}

void sub_0004DA60_enter(void)
{
    if (script_log_on())
        fprintf(stderr, "[SCRIPT] load  tid %5lu this %08X\n",
                GetCurrentThreadId(), g_ecx);
}

void sub_000505F0_enter(void)
{
    uint32_t pool, table = 0;
    if (!script_log_on())
        return;
    pool = GUEST32(0x4C06B8);
    if (pool && pool < 0x04000000)
        table = GUEST32(pool + 0x170 + 0x20);
    fprintf(stderr, "[SCRIPT] link  tid %5lu stream %08X script %08X strings %08X\n",
            GetCurrentThreadId(), GUEST32(g_esp + 4), GUEST32(g_esp + 8), table);
}

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    /* The first few failures per address in full, then one line per 1000:
     * one missed function reached in a loop printed on every frame, and the
     * printing alone cost the game thread several percent and the timer
     * thread its vblanks (stderr's lock). */
    enum { SLOTS = 32, FULL = 4 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int s;

    for (s = 0; s < count && seen[s] != va; s++)
        ;
    if (s == count && count < SLOTS)
        seen[count++] = va;
    if (s < SLOTS && ++hits[s] > FULL) {
        if (hits[s] % 1000 == 0) {
            fprintf(stderr, "[ICALL] 0x%08X failed %llu times\n", va,
                    (unsigned long long)hits[s]);
            fflush(stderr);
        }
        return;
    }
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    /* Where from: the call pushed its return address, so the guest stack's
     * top is the call site, and the words under it its callers' frames. */
    {
        extern __declspec(thread) uint32_t g_esp;
        uint32_t k, sp = g_esp;
        if (g_xbox_mem_offset && sp > 0x10000 && sp < 0x04000000 - 64) {
            fprintf(stderr, "[ICALL]   from 0x%08X; stack:", GUEST32(sp));
            for (k = 1; k < 12; k++)
                fprintf(stderr, " %08X", GUEST32(sp + 4 * k));
            fprintf(stderr, "\n");
        }
    }
    /* The object the call went through: a method call (call [vtable+n])
     * leaves the object in ecx and its vtable in edx or eax. For the
     * autoload hang (a script native at 0x00050D30 with a null vtable) this
     * names which native; any field pointing at text is printed. */
    {
        extern __declspec(thread) uint32_t g_ecx, g_edx;
        uint32_t k, obj = g_ecx;
        fprintf(stderr, "[ICALL]   ecx=%08X edx=%08X eax=%08X", obj, g_edx, g_eax);
        if (g_xbox_mem_offset && obj > 0x10000 && obj < 0x04000000 - 64) {
            fprintf(stderr, "; [ecx]:");
            for (k = 0; k < 12; k++)
                fprintf(stderr, " %08X", GUEST32(obj + 4 * k));
            for (k = 0; k < 12; k++) {
                uint32_t p = GUEST32(obj + 4 * k), n = 0;
                const char *s;
                if (p < 0x10000 || p >= 0x04000000 - 64)
                    continue;
                s = (const char *)((uint8_t *)g_xbox_mem_offset + p);
                while (n < 48 && s[n] >= 0x20 && s[n] < 0x7F)
                    n++;
                if (n >= 4 && (n == 48 || s[n] == 0))
                    fprintf(stderr, " [+%u]=\"%.*s\"", 4 * k, (int)n, s);
            }
        }
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}
