"""Per-function times from a DUALIZE_PROFILE sample file.

    python scripts/profile.py <samples> <exe> [--thread gx|cpu] [--top N]

Symbols come from `nm -C` on the executable (MSYS2's binutils on PATH). Self
time counts the innermost frame of each sample; inclusive time counts every
function on the stack once per sample.
"""
import argparse
import bisect
import collections
import subprocess

PREFERRED_BASE = 0x140000000  # the PE image base the linker picked


def load_symbols(exe):
    out = subprocess.run(["nm", "-C", "--defined-only", exe], capture_output=True, text=True, check=True).stdout
    symbols = []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in "tTwW":
            symbols.append((int(parts[0], 16), parts[2]))
    symbols.sort()
    return [a for a, _ in symbols], [n for _, n in symbols]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("samples")
    parser.add_argument("exe")
    parser.add_argument("--thread")
    parser.add_argument("--top", type=int, default=40)
    parser.add_argument("--callers", help="who calls functions whose name contains this")
    args = parser.parse_args()

    addresses, names = load_symbols(args.exe)

    def name_of(address):
        i = bisect.bisect_right(addresses, address) - 1
        return names[i] if i >= 0 else f"?{address:x}"

    modules = []  # (start, end, name) of every module but the executable

    def label(frame):
        if base <= frame < exe_end:
            return name_of(frame - base + PREFERRED_BASE)
        for start, end, module in modules:
            if start <= frame < end:
                return f"[{module}]"
        return "[?]"

    self_time = collections.Counter()
    inclusive = collections.Counter()
    callers = collections.Counter()
    total = collections.Counter()
    base = PREFERRED_BASE
    exe_end = 1 << 64
    with open(args.samples) as f:
        for line in f:
            parts = line.split()
            if parts[0] == "base":
                base = int(parts[1], 16)
                continue
            if parts[0] == "module":
                start, size = int(parts[1], 16), int(parts[2], 16)
                if start == base:
                    exe_end = start + size
                else:
                    modules.append((start, start + size, parts[3]))
                continue
            thread, frames = parts[0], [int(x, 16) for x in parts[1:]]
            if args.thread and thread != args.thread:
                continue
            total[thread] += 1
            if not frames:
                continue
            self_time[(thread, label(frames[0]))] += 1
            stack = [label(frame) for frame in frames]
            for name in set(stack):
                inclusive[(thread, name)] += 1
            if args.callers:
                # The nearest frame outside the matching function, once per sample.
                hit = next((i for i, n in enumerate(stack) if args.callers in n), None)
                if hit is not None:
                    outer = next((n for n in stack[hit:] if args.callers not in n), "<root>")
                    callers[(thread, outer)] += 1

    for thread, count in sorted(total.items()):
        print(f"== {thread}: {count} samples")
        tables = (("callers", callers),) if args.callers else (("self", self_time), ("inclusive", inclusive))
        for title, counter in tables:
            print(f"  -- {title}")
            rows = [(n, c) for (t, n), c in counter.items() if t == thread]
            for name, c in sorted(rows, key=lambda r: -r[1])[: args.top]:
                print(f"  {100.0 * c / count:5.1f}%  {name[:150]}")


if __name__ == "__main__":
    main()
