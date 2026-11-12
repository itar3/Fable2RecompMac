import re, subprocess, time, collections
def snap():
    out = subprocess.run(["ioreg", "-r", "-c", "AGXDeviceUserClient", "-w", "0", "-l"], capture_output=True, text=True).stdout
    tot = collections.Counter(); usage = None
    for line in out.splitlines():
        if '"AppUsage"' in line:
            usage = sum(int(v) for v in re.findall(r'"accumulatedGPUTime"=(\d+)', line))
        m = re.search(r'"IOUserClientCreator" = "pid (\d+), ([^"]*)"', line)
        if m and usage is not None:
            tot[f"{m.group(2)} ({m.group(1)})"] += usage; usage = None
    return tot
a = snap(); t0 = time.time(); time.sleep(3); b = snap(); dt = time.time() - t0
rows = sorted(((b[k] - a.get(k, 0)) / (dt * 1e9) * 100, k) for k in b)
for pct, k in reversed(rows[-8:]):
    if pct > 0.1: print(f"{pct:6.1f}%  {k}")
