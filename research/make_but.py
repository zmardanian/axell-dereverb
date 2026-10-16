"""Bank of measured rooms from BUT ReverbDB (CC BY 4.0 / Apache 2.0; Szoke et al. 2019): every RIR under */RIR/,
resampled 16 -> 48 kHz, its direct sound replaced by a full-band impulse (rooms.measured), reflections stored with
the measurement's own direct-to-reverberant ratio. Training rescales the reflections to a close vocal mic's ratio.
usage: python make_but.py <out.npz>"""
import os, sys, glob, numpy as np
from concurrent.futures import ProcessPoolExecutor
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rooms as RM

ROOT = os.path.expanduser("~/axell-dereverb-data/rir/but")

def one(p):
    try:
        r = RM.measured(p)
    except Exception:
        return None
    refl = (r["early"] + r["late"]).astype(np.float32)
    e = np.abs(refl); thr = 10 ** (-70 / 20)
    nz = np.nonzero(e > thr)[0]
    last = int(nz[-1]) + 1 if len(nz) else 64
    peak = int(np.argmax(r["direct"]))
    drr = 10 * np.log10(1.0 / (np.sum(refl ** 2) + 1e-20))
    room = p[len(ROOT) + 1:].split("/")[0]
    return room, refl[:max(last, peak + 64)], peak, drr

if __name__ == "__main__":
    paths = sorted(glob.glob(f"{ROOT}/**/RIR/*.wav", recursive=True))
    with ProcessPoolExecutor(14) as ex:
        res = [r for r in ex.map(one, paths, chunksize=16) if r is not None]
    lens = np.array([len(r[1]) for r in res])
    np.savez(sys.argv[1], refl=np.concatenate([r[1] for r in res]), offsets=np.concatenate([[0], np.cumsum(lens)]),
             peak=np.array([r[2] for r in res]), drr=np.array([r[3] for r in res]), room=np.array([r[0] for r in res]))
    import collections
    print(len(res), "RIRs", collections.Counter(r[0] for r in res), "DRR median %.1f dB" % np.median([r[3] for r in res]))
