"""Singing benchmark: is a sung vocal (held notes, vibrato, belting) treated as well as speech? VocalSet singers held
out from any training (female2, female8, male3, male9), song excerpts (straight, vibrato), long tones and belted
arpeggios, in the small-room types and dry (no room: it must be left alone). CMU ARCTIC speech in the same rooms is the
reference. usage: python bench_sing.py [method ...] [--quick]"""
import os, sys, glob, zlib, wave, numpy as np
from scipy import signal as sg
from concurrent.futures import ProcessPoolExecutor
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dr_corpus as DC, dr_metrics as M, dr_baselines as B, rooms as RM

VS = os.path.expanduser(os.environ.get("VOCALSET", "~/axell-dereverb-data/vocalset"))
HELD_OUT = ("female2", "female8", "male3", "male9")
STYLES = ("excerpts-straight", "excerpts-vibrato", "long_tones-straight", "arpeggios-belt")
KINDS = ("none", "booth", "closet", "bedroom", "bathroom", "car")

def sung(singer, style, secs=12.0):
    """the singer's recordings of one style strung together (44.1 -> 48 kHz), ~secs long, at -20 dBFS RMS"""
    parts = []
    for f in sorted(glob.glob(f"{VS}/{singer}/{style}-*.wav")):
        w = wave.open(f); fs = w.getframerate(); ch = w.getnchannels()
        x = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, ch).mean(1) / 32768.0
        parts.append(sg.resample_poly(x, 160, 147) if fs == 44100 else x)
        if sum(map(len, parts)) > secs * 48000:
            break
    x = np.concatenate(parts)[:int(secs * 48000)]
    x = sg.sosfiltfilt(sg.butter(2, 50, "high", fs=48000, output="sos"), x)
    return x * 10 ** (-20 / 20) / np.sqrt(np.mean(x ** 2))

def run(args):
    (voice, style, kind), methods, quick = args
    rng = np.random.default_rng(zlib.crc32(f"sing/{voice}/{style}/{kind}".encode()))
    dry = sung(voice, style) if style != "speech" else DC.dry_take(voice, 4, 0)[0]
    if kind == "none":
        room = dict(direct=np.array([1.0]), early=np.zeros(1), late=np.zeros(1))
    else:
        room = RM.random_room(kind, rng)
    d = DC.reverberate(dry, room, rng)
    res = {}
    for m in methods:
        est = B.METHODS[m](d["input"], room)
        res[m] = M.score(d, est, quick=quick)
    return ("speech" if style == "speech" else "singing"), kind, res

if __name__ == "__main__":
    quick = "--quick" in sys.argv
    methods = [a for a in sys.argv[1:] if not a.startswith("--")] or ["identity", "axell"]
    cs = [(s, st, k) for s in HELD_OUT for st in STYLES for k in KINDS] + [(s, "speech", k) for s in DC.ARCTIC for k in KINDS]
    with ProcessPoolExecutor(int(os.environ.get("BENCH_PROCS", 6))) as ex:
        out = list(ex.map(run, [(c, methods, quick) for c in cs]))
    keys = ["cd", "fwssnr", "sisdr", "ltas", "box", "damage", "left"] + ([] if quick else ["visqol", "estoi", "srmr"])
    print(f"{'voice':8s} {'room':9s} {'method':9s} " + " ".join(f"{k:>7s}" for k in keys))
    for v in ("singing", "speech"):
        for kind in KINDS + ("ALL rooms",):
            rs = [r for vv, k, r in out if vv == v and (k == kind or (kind == "ALL rooms" and k != "none"))]
            for m in methods:
                print(f"{v:8s} {kind:9s} {m:9s} " + " ".join(f"{np.mean([r[m][k] for r in rs]):7.2f}" for k in keys))
