#!/usr/bin/env python3
"""Scripted smoke test for the Fable 2 native renderer build.

Each run launches the game (keyboard as controller: --mnk_mode=true), presses
through the title screen and the main menu (Continue, 4th save card) with a scripted controller (the
window can stay in the background: src/scripted_input.cpp), waits
until the game is in the world, measures a still view and a camera pan, can
follow an extra key route (e.g. into Castle Fairfax), takes window screenshots,
then kills the game and puts the save folder back as it was before the run.

Metrics come from the game log and the captured stdout/stderr: frame and GPU
time ([perf]), pops and occlusion-query flickers ([pops]), the exposure
readback ([writeback] min/max/avg: frozen = identical values), the hero coat
composite ([writeback] hero average), device lost and crashes.

  tools/smoke_test.py --label castle --runs 3 --route "A:5500:1500"
  tools/smoke_test.py --label base --args --fable2_query_force_visible=false
"""
import argparse
import datetime
import glob
import hashlib
import json
import os
import re
import shutil
import signal
import statistics
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(REPO, "out", "build", "mac-arm64-native")
LOGS = os.path.join(BUILD, "logs")
SAVES = os.path.expanduser("~/.local/share/Fable2/B13EBABEBABEBABE/4D5307F1")
BIN = os.path.join(BUILD, "smoke-bin")

TITLE_DRAWS = (60, 85)   # title screen: ~71 draws per frame
MENU_DRAWS = 120         # main menu / save cards: ~155
WORLD_DRAWS = 400        # in the world: 1000+
# Scripted controller items (src/scripted_input.cpp): NAME:holdMs[:pauseMs].
def menu_keys(card):
    """Continue, then the card-th save card (1 = first)."""
    return ["LY-:120:600", "A:150:4000"] + ["LX+:150:900"] * (card - 1) + ["A:150:500"]
PAN_KEYS = ["RX-:2500:100", "RX+:2500:100"] * 3
# Walking around while turning the camera (--walk): streams in new geometry,
# textures and pipelines, where the stutters show.
WALK_KEYS = ["LY+&RX+:2500:50", "LY+:1500:50", "LY+&RX-:3000:50", "LY+:1200:50",
             "LY+&RX+:1500:50", "LY-&RX-:2000:50", "LY+&RX-:1500:50", "LY-:1500:50"]

RE_TIME = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]")
RE_PERF = re.compile(r"\[perf\] \d+ frames, [\d.]+ ms/frame wall \(p50 ([\d.]+).*? draw [\d.]+ \((\d+) draws"
                     r".*?gpu ([\d.]+) ms/frame")
RE_POPS = re.compile(r"\[pops\] ([\d.]+) per second .*queries: ([\d.]+) flickers/s, (\d+)% zero of (\d+)/s")
RE_EXPO = re.compile(r"\[writeback\] (0x[0-9a-f]+): min (\S+) max (\S+) avg (\S+)")
RE_HERO = re.compile(r"\[writeback\] hero .*average rgba \(([\d.]+), ([\d.]+), ([\d.]+), ([\d.]+)\)")
RE_NOFETCH = re.compile(r"\[layout\] vs ([0-9a-f]{16}): no vertex fetches")
LOST = ("Lost VkDevice", "renderer disabled", "failed (0xfffffffc)", "error code 0xFFFFFFFC")


def tool(name):
    """tools/<name>.swift compiled once into the build folder."""
    src = os.path.join(REPO, "tools", name + ".swift")
    exe = os.path.join(BIN, name)
    if not os.path.exists(exe) or os.path.getmtime(exe) < os.path.getmtime(src):
        os.makedirs(BIN, exist_ok=True)
        subprocess.run(["swiftc", "-O", "-o", exe, src], check=True, stdout=subprocess.DEVNULL)
    return exe


def gray_small(path, w=48, h=27):
    """A tiny grayscale copy of an image (rows top to bottom) via sips BMP output."""
    tmp = path + ".small.bmp"
    subprocess.run(["sips", "-s", "format", "bmp", "-z", str(h), str(w), path, "--out", tmp], capture_output=True)
    with open(tmp, "rb") as f:
        b = f.read()
    os.remove(tmp)
    off = int.from_bytes(b[10:14], "little")
    bw = int.from_bytes(b[18:22], "little", signed=True)
    bh = int.from_bytes(b[22:26], "little", signed=True)
    bpp = int.from_bytes(b[28:30], "little") // 8
    stride = (bw * bpp + 3) & ~3
    rows = []
    for y in range(abs(bh)):
        r = b[off + y * stride: off + y * stride + bw * bpp]
        rows.append([0.299 * r[x * bpp + 2] + 0.587 * r[x * bpp + 1] + 0.114 * r[x * bpp] for x in range(bw)])
    return rows[::-1] if bh > 0 else rows


def corr(a, b):
    a = [v for r in a for v in r]
    b = [v for r in b for v in r]
    ma, mb = statistics.fmean(a), statistics.fmean(b)
    num = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    den = (sum((x - ma) ** 2 for x in a) * sum((y - mb) ** 2 for y in b)) ** 0.5
    return num / den if den else 0.0


def orientation(shot, ref):
    """'upright' / 'FLIPPED' / '?': correlation with the reference vs the vertically flipped reference."""
    if not (os.path.exists(shot) and os.path.exists(ref)):
        return None
    s, r = gray_small(shot), gray_small(ref)
    up, down = corr(s, r), corr(s, r[::-1])
    tag = "upright" if up > down + 0.1 else "FLIPPED" if down > up + 0.1 else "?"
    return "%s (%.2f/%.2f)" % (tag, up, down)


def tree_digest(path):
    h = hashlib.sha1()
    for root, dirs, files in sorted(os.walk(path)):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(root, f)
            h.update(os.path.relpath(p, path).encode())
            with open(p, "rb") as fh:
                h.update(fh.read())
    return h.hexdigest()


class Tail:
    def __init__(self, path):
        self.path, self.pos, self.rest = path, 0, ""

    def lines(self):
        try:
            with open(self.path, "r", errors="replace") as f:
                f.seek(self.pos)
                data = f.read()
                self.pos = f.tell()
        except FileNotFoundError:
            return []
        data = self.rest + data
        out = data.split("\n")
        self.rest = out.pop()
        return out


class Abort(Exception):
    pass


class Run:
    def __init__(self, opts, out_dir):
        self.o, self.dir = opts, out_dir
        os.makedirs(out_dir, exist_ok=True)
        self.t0 = time.monotonic()
        self.phase = "boot"
        self.marks = {}
        self.perf, self.pops, self.expo, self.hero = [], [], [], []
        self.lost = None
        self.exit = None
        self.notes = []
        self.nofetch, self.nolayout, self.stage_mismatch, self.reused = [], 0, 0, 0

    # --- game process and log ----------------------------------------------------------------
    def launch(self):
        before = set(glob.glob(os.path.join(LOGS, "Fable2_*.log")))
        for stale in ("input_now", "diag_now", "cvar_now"):
            if os.path.exists(os.path.join(BUILD, stale)):
                os.remove(os.path.join(BUILD, stale))
        args = [os.path.join(BUILD, "Fable2"), os.path.join(REPO, "assets"), "--fullscreen=false",
                "--window_width=1280", "--window_height=720",
                "--fable2_shader_dump_dir=" + os.path.join(REPO, "shader_dump"),
                "--eot_perf_frames=120", "--fable2_pop_counter=true"] + self.o.args
        env = dict(os.environ)
        env["SDL_MAC_BACKGROUND_APP"] = "1"  # the game window doesn't take focus
        for kv in self.o.env:
            k, _, v = kv.partition("=")
            env[k] = v
        self.stdio_path = os.path.join(self.dir, "stdio.txt")
        self.stdio_file = open(self.stdio_path, "w")
        self.proc = subprocess.Popen(args, cwd=BUILD, env=env, stdout=self.stdio_file, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            new = set(glob.glob(os.path.join(LOGS, "Fable2_*.log"))) - before
            if new:
                self.log_path = max(new, key=os.path.getmtime)
                break
            time.sleep(0.2)
        else:
            raise Abort("no new log file")
        self.log, self.stdio = Tail(self.log_path), Tail(self.stdio_path)

    def now(self):
        return time.monotonic() - self.t0

    def poll(self):
        t = self.now()
        for line in self.log.lines():
            if m := RE_PERF.search(line):
                self.perf.append((t, float(m[1]), int(m[2]), float(m[3])))
            elif m := RE_POPS.search(line):
                self.pops.append((t, float(m[1]), float(m[2]), int(m[3]), int(m[4])))
            elif m := RE_EXPO.search(line):
                self.expo.append((t, m[1], m[2], m[3], m[4], self.phase))
            elif m := RE_HERO.search(line):
                self.hero.append(tuple(float(m[i]) for i in range(1, 5)))
            elif m := RE_NOFETCH.search(line):
                self.nofetch.append(m[1][:4])
            elif "no input layout: vs" in line:
                self.nolayout += 1
            elif "shader container (flags" in line:
                self.stage_mismatch += 1
            elif "[shaders]" in line and " reused: was " in line:
                self.reused += 1
            if self.lost is None and any(s in line for s in LOST):
                self.lost = (round(t, 1), self.phase, line.strip()[-160:])
        for line in self.stdio.lines():
            if self.lost is None and any(s in line for s in LOST):
                self.lost = (round(t, 1), self.phase, line.strip()[-160:])
        if self.exit is None and self.proc.poll() is not None:
            self.exit = (round(t, 1), self.phase, self.proc.returncode)

    def check(self):
        self.poll()
        if self.lost:
            raise Abort("device lost")
        if self.exit:
            raise Abort("game exited (%s)" % self.exit[2])

    def sleep(self, secs):
        end = time.monotonic() + secs
        while time.monotonic() < end:
            self.check()
            time.sleep(min(0.25, max(0.0, end - time.monotonic())))

    def wait(self, what, pred, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.check()
            if pred():
                return True
            time.sleep(0.25)
        return False

    def draws(self, n):
        """The last n perf samples' draw counts (None if fewer samples)."""
        if len(self.perf) < n:
            return None
        return [p[2] for p in self.perf[-n:]]

    def keys(self, items):
        """Plays controller items in the game (input_now, read on its next pad poll)
        and waits until they are done. The window may be in the background."""
        total = 0
        for item in items:
            parts = item.split(":")
            total += int(parts[1]) + (int(parts[2]) if len(parts) > 2 else 150)
        tmp = os.path.join(BUILD, "input_now.tmp")
        with open(tmp, "w") as f:
            f.write(" ".join(items) + "\n")
        os.replace(tmp, os.path.join(BUILD, "input_now"))
        self.sleep(total / 1000.0 + 0.2)

    def diag(self, name):
        """A per-draw diag frame in the log ([diag] lines), marked with the name."""
        with open(os.path.join(BUILD, "diag_now"), "w"):
            pass
        self.diags.append((name, round(self.now(), 1)))
        self.sleep(3)

    def shot(self, name):
        r = subprocess.run([tool("winid"), str(self.proc.pid)], capture_output=True, text=True)
        if r.returncode != 0:
            self.notes.append("no window for screenshot " + name)
            return
        full = os.path.join(self.dir, name + ".png")
        subprocess.run(["screencapture", "-x", "-o", "-l" + r.stdout.strip(), full], check=False)
        if os.path.exists(full):
            subprocess.run(["sips", "-s", "format", "jpeg", "-Z", "960", full, "--out",
                            os.path.join(self.dir, name + ".jpg")], capture_output=True)
            if not self.o.full_shots:
                os.remove(full)

    def mark(self, name):
        self.phase = name
        self.marks[name] = round(self.now(), 1)

    def window(self, name, secs, keys=None):
        start = self.now()
        if keys:
            self.keys(keys)
            rest = secs - (self.now() - start)
            if rest > 0:
                self.sleep(rest)
        else:
            self.sleep(secs)
        self.sleep(2.5)  # the perf/pops lines covering the end of the window
        end = self.now()
        perf = [p for p in self.perf if start < p[0] <= end]
        pops = [p for p in self.pops if start < p[0] <= end]
        med = lambda v: round(statistics.median(v), 2) if v else None
        mean = lambda v: round(statistics.fmean(v), 1) if v else None
        self.windows[name] = {
            "frame_ms": med([p[1] for p in perf]), "gpu_ms": med([p[3] for p in perf]),
            "draws": med([p[2] for p in perf]), "pops_s": mean([p[1] for p in pops]),
            "flickers_s": mean([p[2] for p in pops]), "query_zero_pct": mean([p[3] for p in pops]),
        }

    # --- the route ---------------------------------------------------------------------------
    def route(self):
        self.windows = {}
        self.diags = []
        self.mark("boot")
        self.launch()
        if not self.wait("title", lambda: (d := self.draws(2)) and all(TITLE_DRAWS[0] <= x <= TITLE_DRAWS[1] for x in d),
                         self.o.boot_timeout):
            raise Abort("title screen not reached")
        self.mark("title")
        for attempt in range(3):
            self.keys(["A:150:300"])
            n = len(self.perf)
            if self.wait("menu", lambda: any(p[2] >= MENU_DRAWS for p in self.perf[n:]), 8):
                break
        else:
            raise Abort("main menu not reached")
        self.mark("menu")
        self.sleep(1.5)
        self.keys(menu_keys(self.o.card))
        self.mark("loading")
        if not self.wait("world", lambda: (d := self.draws(2)) and min(d) >= WORLD_DRAWS, self.o.load_timeout):
            raise Abort("world not reached")
        self.mark("world")
        self.sleep(self.o.settle)
        self.shot("world")
        if self.o.diag_world:
            self.diag("world")
        self.window("still", self.o.still)
        self.window("pan", 16, PAN_KEYS)
        self.shot("pan")
        if self.o.soak:
            # Minutes of camera pans: device losses came after minutes in the world.
            self.mark("soak")
            start = self.now()
            n = len(self.perf)
            while self.now() - start < self.o.soak:
                self.keys(WALK_KEYS if self.o.walk else PAN_KEYS)
                self.check()
            perf = self.perf[n:]
            self.windows["soak"] = {"secs": round(self.now() - start), "frame_ms": round(statistics.median(p[1] for p in perf), 2) if perf else None,
                                    "gpu_ms": None, "draws": None, "pops_s": None, "flickers_s": None, "query_zero_pct": None}
        if self.o.route:
            self.mark("route")
            n = len(self.perf)
            self.keys(self.o.route.split())
            left = lambda: any(p[2] < WORLD_DRAWS for p in self.perf[n:])
            if not self.wait("route load", left, 30):
                raise Abort("route: no load started")
            m = len(self.perf)
            if not self.wait("route world", lambda: len(self.perf) - m >= 2 and min(p[2] for p in self.perf[-2:]) >= WORLD_DRAWS,
                             self.o.load_timeout):
                raise Abort("route: destination not reached")
            self.mark("route_world")
            self.sleep(self.o.settle)
            self.shot("route")
            if self.o.route_cvar:
                # cvar_now: name=value lines the game applies at its next frame.
                with open(os.path.join(BUILD, "cvar_now"), "w") as f:
                    f.write("\n".join(self.o.route_cvar) + "\n")
                self.sleep(3)
            if self.o.diag:
                self.diag("route")
            self.window("route_still", self.o.still)
            self.window("route_pan", 16, PAN_KEYS)
            self.shot("route_pan")
        if self.o.disconnect:
            # The controller reported lost (as when the window lost focus with the
            # keyboard driver): the game pauses on a frozen frame.
            self.mark("disconnect")
            ms = int(self.o.disconnect * 1000)
            tmp = os.path.join(BUILD, "input_now.tmp")
            with open(tmp, "w") as f:
                f.write("DISCONNECT:%d:0\n" % ms)
            os.replace(tmp, os.path.join(BUILD, "input_now"))
            self.sleep(self.o.disconnect / 2)
            self.shot("disconnected")
            if self.o.diag:
                self.diag("disconnected")
            self.sleep(self.o.disconnect / 2 + 3)
            self.shot("reconnected")
        self.mark("done")

    def stop(self):
        if getattr(self, "proc", None) and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(8)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        if getattr(self, "stdio_file", None):
            self.stdio_file.close()

    def summary(self, error):
        # Exposure readback frozen: 3+ identical lines in a row for one target, in the world.
        frozen = {}
        by_va = {}
        for t, va, mn, mx, avg, phase in self.expo:
            if phase in ("boot", "title", "menu", "loading"):
                continue
            by_va.setdefault(va, []).append((mn, mx, avg))
        for va, v in by_va.items():
            run = best = 1
            for a, b in zip(v, v[1:]):
                run = run + 1 if a == b else 1
                best = max(best, run)
            frozen[va] = {"lines": len(v), "max_identical_run": best, "last_avg": v[-1][2]}
        # Brown castle: every exposure readback after the route load reads 0.
        route_expo = [e for e in self.expo if e[5] in ("route_world", "route_still", "route_pan")]
        castle_zero = bool(route_expo) and all(e[3] == "0" for e in route_expo)
        hero = self.hero[-1] if self.hero else None
        return {
            "error": error, "lost": self.lost, "exit": self.exit, "marks": self.marks, "windows": self.windows,
            "exposure": frozen, "castle_exposure_zero": castle_zero, "hero_rgba": hero,
            "dog": None if hero is None else ("ok" if hero[0] > 20 else "BLACK"),
            "log": os.path.basename(getattr(self, "log_path", "")), "notes": self.notes,
            "diags": getattr(self, "diags", []), "nofetch_vs": self.nofetch, "nolayout_vs": self.nolayout, "stage_mismatch": self.stage_mismatch, "stage_reused": self.reused,
            "world_view": orientation(os.path.join(self.dir, "world.jpg"), os.path.join(LOGS, "smoke", "ref", "world.jpg")),
            "route_view": orientation(os.path.join(self.dir, "route.jpg"), os.path.join(LOGS, "smoke", "ref", "castle_route.jpg"))
            if self.o.route else None,
            "disconnected_view": orientation(os.path.join(self.dir, "disconnected.jpg"), os.path.join(LOGS, "smoke", "ref", "world.jpg"))
            if self.o.disconnect and not self.o.route else None,
        }


def one_line(i, s):
    w = s["windows"]
    def fmt(name):
        v = w.get(name)
        if not v:
            return ""
        return " %s %s/%sms d%s pops %s flk %s" % (name, v["frame_ms"], v["gpu_ms"], v["draws"], v["pops_s"], v["flickers_s"])
    expo = " ".join("%s:%s(run%s)" % (va[-4:], e["last_avg"], e["max_identical_run"]) for va, e in s["exposure"].items())
    status = "LOST@%s" % s["lost"][1] if s["lost"] else ("ERR " + s["error"] if s["error"] else "ok")
    return "run%d %s |%s%s%s%s | expo %s%s | dog %s | view %s / %s | nofetch %s nolayout %s mismatch %s reused %s | %s%s" % (
        i, status, fmt("still"), fmt("pan"), fmt("route_still"), fmt("route_pan"), expo or "-",
        " CASTLE-EXPOSURE-0" if s.get("castle_exposure_zero") else "", s["dog"],
        s["world_view"], s["route_view"], ",".join(s["nofetch_vs"]) or "-", s["nolayout_vs"], s["stage_mismatch"], s["stage_reused"], s["log"], (" | " + "; ".join(s["notes"])) if s["notes"] else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--label", default="smoke")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--card", type=int, default=2, help="save card to load from Continue (1 = leftmost; user test saves: 2, 3)")
    ap.add_argument("--route", default="", help="controller items sent in the world to start a load (castle: 'A:5500:1500')")
    ap.add_argument("--settle", type=float, default=8)
    ap.add_argument("--soak", type=float, default=0, help="seconds of camera pans in the world before the route")
    ap.add_argument("--walk", action="store_true", help="the soak walks around while turning the camera")
    ap.add_argument("--still", type=float, default=10)
    ap.add_argument("--boot-timeout", type=float, default=180)
    ap.add_argument("--load-timeout", type=float, default=150)
    ap.add_argument("--env", action="append", default=[], help="K=V for the game's environment")
    ap.add_argument("--disconnect", type=float, default=0, help="seconds of a lost controller at the end (screenshots)")
    ap.add_argument("--route-cvar", action="append", default=[], help="name=value set live (cvar_now) at the route shot")
    ap.add_argument("--diag-world", action="store_true", help="a per-draw diag frame in the log at the world shot")
    ap.add_argument("--stop-on-frozen", action="store_true", help="end the batch after a run whose exposure readback froze")
    ap.add_argument("--stop-on-nofetch", action="store_true", help="end the batch after a run that drew a no-fetch vertex shader")
    ap.add_argument("--diag", action="store_true", help="a per-draw diag frame in the log at the route shot")
    ap.add_argument("--full-shots", action="store_true", help="keep full-size PNG screenshots")
    ap.add_argument("--args", nargs=argparse.REMAINDER, default=[], help="extra game arguments (last)")
    o = ap.parse_args()

    if subprocess.run(["pgrep", "-x", "Fable2"], capture_output=True).returncode == 0:
        sys.exit("Fable2 is already running: close it first")
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    top = os.path.join(LOGS, "smoke", "%s-%s" % (o.label, stamp))
    os.makedirs(top)
    lines = []
    for i in range(1, o.runs + 1):
        run_dir = os.path.join(top, "run%d" % i)
        backup = os.path.join(top, "save_before_run%d" % i)
        shutil.copytree(SAVES, backup)
        digest = tree_digest(SAVES)
        r = Run(o, run_dir)
        error = None
        try:
            r.route()
        except Abort as e:
            error = str(e)
            r.shot("abort")
        except KeyboardInterrupt:
            error = "interrupted"
        finally:
            r.stop()
            if tree_digest(SAVES) != digest:
                shutil.rmtree(SAVES)
                shutil.copytree(backup, SAVES)
                r.notes.append("save folder restored")
            shutil.rmtree(backup)
        s = r.summary(error)
        with open(os.path.join(run_dir, "summary.json"), "w") as f:
            json.dump(s, f, indent=1)
        lines.append(one_line(i, s))
        print(lines[-1], flush=True)
        frozen = any(e["max_identical_run"] >= 3 for e in s["exposure"].values()) or s["castle_exposure_zero"]
        if error == "interrupted" or (o.stop_on_nofetch and s["nofetch_vs"]) or (o.stop_on_frozen and frozen):
            break
    with open(os.path.join(top, "summary.txt"), "w") as f:
        f.write("args: %s\nenv: %s\nroute: %s\n" % (" ".join(o.args), " ".join(o.env), o.route))
        f.write("\n".join(lines) + "\n")
    print(top)


if __name__ == "__main__":
    main()
