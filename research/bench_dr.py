"""De-reverb benchmark: every method on the same examples, scored per room type.
Voices: the 3 CMU ARCTIC voices (held out from any training). Rooms: the 8 synthetic small-room types, and measured
everyday rooms from the MIT IR Survey (evaluation only). usage: python bench_dr.py [method ...] [--quick] [--dense]"""
import os, sys, time, glob, numpy as np
from concurrent.futures import ProcessPoolExecutor
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dr_corpus as DC, dr_metrics as M, dr_baselines as B, rooms as RM

MIT_KINDS = ("Bedroom", "Bathroom", "Car", "Shower", "Kitchen", "Office", "Livingroom", "LivingRoom", "Diningroom", "Stairwell")

def cases(seeds=1):
    out = []
    for kind in RM.ROOM_TYPES:
        for spk in DC.ARCTIC:
            for s in range(seeds):
                out.append((spk, kind, s, None))
    mit = [p for p in RM.mit_paths() if any(f"_{k}" in os.path.basename(p) for k in MIT_KINDS)]
    for i, p in enumerate(mit[:24]):
        out.append((DC.ARCTIC[i % 3], "mit_" + os.path.basename(p).split("_")[1].lower(), 0, p))
    return out

def run(args):
    (spk, kind, s, path), methods, quick = args
    d = DC.example(spk, kind if path is None else "measured", s, measured=path)
    res = {}
    for m in methods:
        est = B.METHODS[m](d["input"], d["room"])
        res[m] = M.score(d, est, quick=quick)
    return kind, res

if __name__ == "__main__":
    quick = "--quick" in sys.argv
    if "--dense" in sys.argv:          # rap / spoken-word delivery: 0-120 ms between utterances
        os.environ["AXDR_GAP"] = "0.0,0.12"
    methods = [a for a in sys.argv[1:] if not a.startswith("--")] or list(B.METHODS)
    cs = cases()
    t0 = time.time()
    with ProcessPoolExecutor(int(os.environ.get("BENCH_PROCS", 6))) as ex:
        out = list(ex.map(run, [(c, methods, quick) for c in cs]))
    keys = ["visqol", "estoi", "srmr", "cd", "fwssnr", "sisdr", "ltas", "box", "damage", "left"] if not quick else ["cd", "fwssnr", "sisdr", "ltas", "box", "damage", "left"]
    groups = {}
    for kind, res in out:
        g = "mit (measured)" if kind.startswith("mit_") else kind
        groups.setdefault(g, []).append(res)
    print(f"{len(cs)} examples, {time.time() - t0:.0f} s")
    print(f"{'room':16s} {'method':10s} " + " ".join(f"{k:>7s}" for k in keys))
    for g, rs in groups.items():
        for m in methods:
            print(f"{g:16s} {m:10s} " + " ".join(f"{np.mean([r[m][k] for r in rs]):7.2f}" for k in keys))
    print("ALL")
    for m in methods:
        print(f"{'':16s} {m:10s} " + " ".join(f"{np.mean([r[m][k] for _, r in out]):7.2f}" for k in keys))
