"""Show one thread's non-idle stacks from a RECOMP_SAMPLE_ALL run.

    py -3 tools/sample_thread.py <substring of a function on the thread> [skip]

Picks the thread whose samples mention the function (kernel_timer_loop_ns
for the timer thread, nv2a_ack_thread for the executor), drops samples
containing `skip` (default: xbox_HostTimerSleepNs, the timer's sleep), and
prints the commonest innermost-six-frame chains.
"""
import bisect
import collections
import sys

sys.argv, args = sys.argv[:1], sys.argv[1:]
src = open("tools/samples.py").read().replace("\nmain()\n", "\n")
ns = {}
exec(src, ns)
rvas, names = ns["load_map"]("build/Release/spiderman_recomp.map")


def name(o):
    if o == "x":
        return "(outside exe)"
    i = bisect.bisect_right(rvas, int(o, 16)) - 1
    return names[i] if i >= 0 else "?"


want = args[0] if args else "kernel_timer_loop_ns"
skip = args[1] if len(args) > 1 else "xbox_HostTimerSleepNs"
by = collections.defaultdict(list)
for line in open("sample_stacks.txt"):
    p = line.split()
    if p and p[0].startswith("T"):
        by[p[0]].append([name(o) for o in p[1:]])
for tid, rows in by.items():
    if not any(want in n for fr in rows for n in fr):
        continue
    busy = [fr for fr in rows if skip not in fr]
    print(f"{tid}: {len(rows)} samples, {len(busy)} without {skip}")
    chain = lambda fr: " <- ".join([n for n in fr if n != "(outside exe)"][:4]) or "(all outside)"
    for c, k in collections.Counter(chain(fr) for fr in busy).most_common(15):
        print(f"  {k:4d}  {c}")
