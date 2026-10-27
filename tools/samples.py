"""Summarise RECOMP_SAMPLE's sample_stacks.txt with the linker map.

    py -3 tools/samples.py [sample_stacks.txt] [build/Release/spiderman_recomp.map]

Prints where a sampled thread was (exclusive: the innermost frame) and which
functions were on its stack (inclusive: any frame), as percentages of its
samples. Generated functions are named sub_XXXXXXXX after their Xbox
address, so a game function reads straight off the list.

With RECOMP_SAMPLE_ALL each line starts with T<thread id>; threads are then
ranked by how often they were inside the exe (doing work, rather than parked
in a Windows wait, which shows as "(outside exe)") and the busiest are shown.
"""
import bisect
import collections
import re
import sys


def load_map(path):
    """(sorted rva list, names) from an MSVC /MAP file."""
    syms = []
    pref = None
    with open(path, errors="replace") as f:
        for line in f:
            m = re.match(r"\s*Preferred load address is ([0-9a-fA-F]+)", line)
            if m:
                pref = int(m.group(1), 16)
            m = re.match(r"\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s", line)
            if m and pref is not None:
                syms.append((int(m.group(2), 16) - pref, m.group(1)))
    syms.sort()
    return [s[0] for s in syms], [s[1] for s in syms]


def report(label, rows, top_ex=12, top_in=20):
    total = len(rows)
    excl, incl = collections.Counter(), collections.Counter()
    for frames in rows:
        excl[frames[0]] += 1
        for n in set(frames):
            incl[n] += 1
    print(f"== {label}: {total} samples")
    print("  exclusive (where it was):")
    for n, c in excl.most_common(top_ex):
        print(f"    {100.0 * c / total:5.1f}%  {n}")
    print("  inclusive (on the stack):")
    for n, c in incl.most_common(top_in):
        print(f"    {100.0 * c / total:5.1f}%  {n}")
    print()


def main():
    samples = sys.argv[1] if len(sys.argv) > 1 else "sample_stacks.txt"
    mapf = sys.argv[2] if len(sys.argv) > 2 else "build/Release/spiderman_recomp.map"
    rvas, names = load_map(mapf)

    def name(off):
        if off == "x":
            return "(outside exe)"
        i = bisect.bisect_right(rvas, int(off, 16)) - 1
        return names[i] if i >= 0 else "?"

    by_thread = collections.defaultdict(list)
    with open(samples) as f:
        for line in f:
            parts = line.split()
            tid = "main"
            if parts and parts[0].startswith("T"):
                tid, parts = parts[0][1:], parts[1:]
            if parts:
                by_thread[tid].append([name(o) for o in parts])

    if list(by_thread) == ["main"]:
        report("guest main thread", by_thread["main"])
        return
    busy = sorted(by_thread.items(),
                  key=lambda kv: -sum(fr[0] != "(outside exe)" for fr in kv[1]))
    print("threads by share of samples inside the exe:")
    for tid, rows in busy[:10]:
        inside = sum(fr[0] != "(outside exe)" for fr in rows)
        print(f"  T{tid}: {100.0 * inside / len(rows):5.1f}% of {len(rows)}")
    print()
    for tid, rows in busy[:5]:
        report(f"T{tid}", rows, 8, 14)


main()
