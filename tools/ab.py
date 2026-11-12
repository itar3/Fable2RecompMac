#!/usr/bin/env python3
"""In-session A/B for live cvars (run next to tools/smoke_test.py --still 120).

toggle: ab.py toggle <build dir> <cvar> <period s> <duration s> <value> <value> [...]
  writes <build>/cvar_now alternately (the game applies it within 30 frames).
analyse: ab.py analyse <log> <cvar> [min_draws]
  pairs each [perf] line with the cvar value set before it, drops the first
  line after each switch, keeps lines with >= min_draws draws, prints means.
"""
import os, re, sys, time, statistics

def toggle(build, name, values, period, duration):
    """values: two or more, cycled ("" = empty string)."""
    end = time.time() + duration
    vals = values
    i = 0
    while time.time() < end:
        tmp = os.path.join(build, "cvar_now.tmp")
        with open(tmp, "w") as f:
            f.write("%s=%s\n" % (name, vals[i % len(vals)]))
        os.replace(tmp, os.path.join(build, "cvar_now"))
        i += 1
        time.sleep(period)

TS = re.compile(r"^\[(\d+-\d+-\d+ \d+:\d+:\d+\.\d+)\]")
PERF = re.compile(r"\[perf\] \d+ frames, ([0-9.]+) ms/frame wall.*?\((\d+) draws.*?gpu ([0-9.]+) ms/frame: small 16f\+z ([0-9.]+)")

def analyse(log, name, min_draws=1800):
    state = None
    fresh = False
    groups = {}
    with open(log, errors="replace") as f:
        for line in f:
            if "[cvar_now] %s=" % name in line and line.rstrip().endswith("set"):
                state = line.split("%s=" % name, 1)[1].split(" ->")[0].strip() or "(empty)"
                fresh = True
                continue
            m = PERF.search(line)
            if not m or state is None:
                continue
            if fresh:  # this interval spans the switch
                fresh = False
                continue
            wall, draws, gpu, main = float(m.group(1)), int(m.group(2)), float(m.group(3)), float(m.group(4))
            if draws < min_draws:
                continue
            groups.setdefault(state, []).append((wall, gpu, main))
    for k, v in sorted(groups.items()):
        w = [x[0] for x in v]; g = [x[1] for x in v]; m = [x[2] for x in v]
        sd = statistics.stdev(g) if len(g) > 1 else 0.0
        print("%s=%s: n=%d wall %.2f gpu %.2f (sd %.2f) main %.2f" %
              (name, k, len(v), statistics.mean(w), statistics.mean(g), sd, statistics.mean(m)))

if __name__ == "__main__":
    if sys.argv[1] == "toggle":
        # toggle <build> <cvar> <period s> <duration s> <value> <value> [...]
        toggle(sys.argv[2], sys.argv[3], sys.argv[6:], float(sys.argv[4]), float(sys.argv[5]))
    else:
        analyse(sys.argv[2], sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 1800)
