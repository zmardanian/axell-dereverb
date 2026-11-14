#!/usr/bin/env python3
"""Linux/Windows sound match (standard library only, runs on both CI runners).
  xplat.py gen in.f32                     a fixed 6 s stereo 48 kHz test signal: sweep, bursts, noise, silence
  xplat.py compare a.f32 b.f32 [limit]    the largest sample difference in dBFS; fails above limit (default -90)"""
import math, struct, sys
from array import array

def gen(path):
    fs, n = 48000, 48000 * 6; x = array("f"); seed = 76
    for i in range(n):
        t = i / fs; seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF; nz = seed / 0x7FFFFFFF - 0.5
        sweep = math.sin(2 * math.pi * (40 * t + 0.5 * (8000 - 40) / 6 * t * t))
        burst = 1.0 if int(t * 3) % 2 == 0 else 0.08                  # loud/quiet every 1/3 s (attack + release)
        gate = 0.0 if 4.5 < t < 4.8 else 1.0                          # a silence (release tail)
        l = gate * (0.6 * burst * sweep + 0.05 * nz); r = gate * (0.5 * burst * math.sin(2 * math.pi * 220 * t) + 0.04 * nz)
        x.append(l); x.append(r)
    with open(path, "wb") as f: x.tofile(f)

def load(path):
    x = array("f")
    with open(path, "rb") as f: x.frombytes(f.read())
    return x

def compare(a, b, limit=-90.0):
    x, y = load(a), load(b)
    if len(x) != len(y): print(f"FAIL length {len(x)} vs {len(y)}"); return 1
    d = max(abs(p - q) for p, q in zip(x, y)); pk = max(abs(p) for p in x)
    db = 20 * math.log10(d) if d > 0 else -math.inf
    print(f"{a} vs {b}: largest difference {db:.1f} dBFS (signal peak {20 * math.log10(pk):.1f} dBFS), limit {limit} dBFS")
    return 0 if db <= limit else 1

if __name__ == "__main__":
    if sys.argv[1] == "gen": gen(sys.argv[2])
    else: sys.exit(compare(sys.argv[2], sys.argv[3], float(sys.argv[4]) if len(sys.argv) > 4 else -90.0))
