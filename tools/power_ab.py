#!/usr/bin/env python3
"""In-session energy A/B for a live cvar (run next to tools/smoke_test.py --still N).

  tools/power_ab.py <cvar> <value> <value> [--period 10] [--duration 120] [--draws 2000]

Waits until the newest game log shows world frames ([perf] lines with at least
--draws draws), then writes <build>/cvar_now alternately (the game applies it
within 30 frames) and samples once a second: whole-Mac power draw (the battery
controller's SystemLoad), the GPU's busy percentage and the game's CPU use.
The first 3 samples after each switch are dropped. Prints the means per value;
tools/ab.py analyse <log> <cvar> gives the frame and GPU times for the same run.
"""
import argparse
import glob
import os
import re
import statistics
import subprocess
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.environ.get("FABLE2_BUILD") or os.path.join(REPO, "out", "build", "mac-arm64-native")
RE_DRAWS = re.compile(r"\[perf\] .*?\((\d+) draws")


def read(cmd, regex):
    try:
        m = re.search(regex, subprocess.run(cmd, capture_output=True, text=True, timeout=5).stdout)
        return float(m[1]) if m else None
    except (OSError, subprocess.SubprocessError):
        return None


def game_pid():
    out = subprocess.run(["pgrep", "-x", "Fable2"], capture_output=True, text=True).stdout.split()
    return out[0] if out else None


def wait_for_world(min_draws, timeout=400):
    start = time.time()
    while time.time() - start < timeout:
        logs = glob.glob(os.path.join(BUILD, "logs", "Fable2_*.log"))
        if logs:
            log = max(logs, key=os.path.getmtime)
            if os.path.getmtime(log) > start - 600:
                with open(log, errors="replace") as f:
                    hits = [int(m[1]) for line in f if (m := RE_DRAWS.search(line))]
                if sum(1 for d in hits[-3:] if d >= min_draws) == 3 and game_pid():
                    return log
        time.sleep(1)
    raise SystemExit("power_ab: no world frames in the game log")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cvar")
    ap.add_argument("values", nargs="+")
    ap.add_argument("--period", type=float, default=10)
    ap.add_argument("--duration", type=float, default=120)
    ap.add_argument("--draws", type=int, default=2000)
    o = ap.parse_args()
    log = wait_for_world(o.draws)
    pid = game_pid()
    time.sleep(3)
    samples = {v: [] for v in o.values}
    end = time.time() + o.duration
    i = 0
    while time.time() < end and game_pid() == pid:
        value = o.values[i % len(o.values)]
        tmp = os.path.join(BUILD, "cvar_now.tmp")
        with open(tmp, "w") as f:
            f.write("%s=%s\n" % (o.cvar, value))
        os.replace(tmp, os.path.join(BUILD, "cvar_now"))
        switch_end = time.time() + o.period
        n = 0
        while time.time() < switch_end:
            t = time.time()
            watts = read(["ioreg", "-rw0", "-c", "AppleSmartBattery"], r'"SystemLoad"=(\d+)')
            gpu = read(["ioreg", "-r", "-d", "1", "-c", "IOAccelerator"], r'"Device Utilization %"=(\d+)')
            cpu = read(["ps", "-o", "%cpu=", "-p", pid], r"([\d.]+)")
            n += 1
            if n > 3 and None not in (watts, gpu, cpu):
                samples[value].append((watts / 1000, gpu, cpu / 100))
            time.sleep(max(0.0, 1.0 - (time.time() - t)))
        i += 1
    print("log", os.path.basename(log))
    for v, s in samples.items():
        if not s:
            print("%s=%s: no samples" % (o.cvar, v))
            continue
        col = lambda k: [x[k] for x in s]
        sd = statistics.stdev(col(0)) if len(s) > 1 else 0.0
        print("%s=%s: n=%d power %.1f W (sd %.1f) gpu %.0f%% cpu %.2f cores" %
              (o.cvar, v, len(s), statistics.fmean(col(0)), sd, statistics.fmean(col(1)), statistics.fmean(col(2))))


if __name__ == "__main__":
    main()
