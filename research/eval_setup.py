"""Axell De-Reverb on the artist's setup (untreated room -> LCT 440 PURE -> Volt 176), checkpoint against checkpoint.
1. Held-out matched setup: the 3 CMU ARCTIC voices (never trained on) in the 24 TEST rooms of make_your_rooms.py
   (never trained on), dense rap-like takes, the artist's noise floor 22-40 dB under the voice, the 76 on half of them;
   target = the direct sound (through the same compressor gain). Scored with dr_metrics (quick).
2. The artist's real takes: the decay after word endings per band (measure_setup.py) before and after, and the noise.
usage: python3 eval_setup.py <ckpt.pt> [<ckpt.pt> ...] [--real]"""
import os, sys, zlib, numpy as np, soundfile as sf
from concurrent.futures import ProcessPoolExecutor
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dr_corpus as DC, dr_metrics as M, dr_baselines as B, train_dr as T, measure_setup as MS

FS = 48000
KEYS = ["cd", "fwssnr", "sisdr", "ltas", "box", "damage", "left"]
# the artist's real takes: a personal list, kept outside the repository (one path per line)
TAKES_FILE = os.path.expanduser("~/axell-dereverb-data/your-setup/takes.txt")
TAKES = [l.strip() for l in open(TAKES_FILE) if l.strip()] if os.path.exists(TAKES_FILE) else []

def case(i):
    rng = np.random.default_rng(zlib.crc32(f"setup-eval/{i}".encode()))
    ys = T.YourSetup("test")
    spk = DC.ARCTIC[i % 3]
    dry, lab = DC.dry_take(spk, 4, 900 + i, gap=(0.0, 0.12))
    j = i % len(ys.h); h, d = ys.h[j].astype(np.float64), ys.d[j].astype(np.float64)
    n = len(dry)
    from scipy import signal as sg
    full = sg.fftconvolve(dry, h)[:n]; direct = sg.fftconvolve(dry, d)[:n]
    rms = np.sqrt(np.mean(direct[lab] ** 2))
    x = full + ys.noise(n, rng) * rms * 10 ** (-rng.uniform(22, 40) / 20)
    t = direct
    if i % 2:
        g = T.fet_gain(x, rng); x, t = x * g, t * g
    early = x - t   # everything that is not the direct sound counts as removable (room + noise)
    return dict(input=x, target=t, early=early * 0, late=early, lab=lab, kind="your-setup", spk=spk, room=None)

def score(args):
    i, ckpts = args
    d = case(i)
    out = {}
    for c in ckpts:
        os.environ["AXDR_CKPT"] = c
        est = B.axell_net(d["input"], None, ckpt=c)
        out[c] = M.score(d, est, quick=True)
    out["identity"] = M.score(d, d["input"], quick=True)
    return out

if __name__ == "__main__":
    ck = [a for a in sys.argv[1:] if not a.startswith("--")]
    with ProcessPoolExecutor(int(os.environ.get("BENCH_PROCS", 6))) as ex:
        res = list(ex.map(score, [(i, ck) for i in range(48)]))
    print(f"{'held-out setup (48 takes)':34s} " + " ".join(f"{k:>7s}" for k in KEYS))
    for c in ["identity"] + ck:
        print(f"{os.path.basename(os.path.dirname(c)) + '/' + os.path.basename(c) if c != 'identity' else 'unprocessed':34s} "
              + " ".join(f"{np.mean([r[c][k] for r in res]):7.2f}" for k in KEYS))
    if "--real" in sys.argv:
        for p in TAKES:
            x, fs = sf.read(p); x = x.mean(1) if x.ndim > 1 else x
            rows = [("as recorded", x)] + [(os.path.basename(c), B.axell_net(x, None, ckpt=c)) for c in ck]
            print(os.path.basename(p))
            for name, y in rows:
                m = MS.measure_array(y, fs)
                print(f"  {name:28s} endings {m['offsets']:3d} | decay RT60 " + " ".join(f"{b}:{m['rt60'][b]['rt60']:.2f}" if b in m['rt60'] else f"{b}:  -  " for b in MS.BANDS)
                      + f" | floor {m['noise_dbfs']:6.1f} dBFS (SNR {m['snr_db']:.1f})")
