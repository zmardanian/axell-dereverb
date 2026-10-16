"""Quick check of a checkpoint on dry singing (the 4 VocalSet singers held out from training): level change and SI-SDR
vs the dry take at the default REDUCTION 85 %, per style. A dry voice must come out essentially as it went in.
usage: python check_sing.py <weights.pt>"""
import os, sys, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ["AXDR_CKPT"] = sys.argv[1]
import bench_sing as S, dr_baselines as B

def sisdr(y, t):
    a = np.dot(y, t) / np.dot(t, t); return 10 * np.log10(np.sum((a * t) ** 2) / np.sum((y - a * t) ** 2))

if __name__ == "__main__":
    for st in S.STYLES:
        r = []
        for s in S.HELD_OUT:
            x = S.sung(s, st); x = x + 10 ** (-70 / 20) * 0.1 * np.random.default_rng(0).standard_normal(len(x))
            y = 0.15 * x + 0.85 * B.METHODS["axell"](x, None)
            r.append((10 * np.log10(np.mean(y ** 2) / np.mean(x ** 2)), sisdr(y, x)))
        r = np.array(r)
        print(f"dry {st:22s} level {r[:, 0].mean():+5.1f} dB (worst {r[:, 0].min():+5.1f})  SI-SDR {r[:, 1].mean():5.1f} dB", flush=True)
