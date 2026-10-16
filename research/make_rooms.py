"""Bank of simulated rooms for training (rooms.random_room), weighted to where vocals are recorded:
booth 20 %, closet 15 %, car 10 %, bedroom 15 %, furnished bedroom 10 %, bathroom 10 %, living room 10 %,
treated room 10 %. Each room: the full response h (direct + early + late, float32) and the index of the direct
sound's peak (the training target is the dry voice delayed to it).
usage: python make_rooms.py <count> <out.npz>"""
import os, sys, zlib, numpy as np
from concurrent.futures import ProcessPoolExecutor
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rooms as RM

WEIGHTS = dict(booth=0.20, closet=0.15, car=0.10, bedroom=0.15, bedroom_furnished=0.10, bathroom=0.10, living=0.10, treated=0.10)

def one(i):
    rng = np.random.default_rng(zlib.crc32(f"trainroom/{i}".encode()))
    kinds = list(WEIGHTS); kind = kinds[rng.choice(len(kinds), p=list(WEIGHTS.values()))]
    r = RM.random_room(kind, rng)
    h = (r["direct"] + r["early"] + r["late"]).astype(np.float32)
    # trim the tail at -70 dB re the direct sound
    e = np.abs(h); thr = 10 ** (-70 / 20) * e.max()
    last = int(np.nonzero(e > thr)[0][-1]) + 1
    peak = int(np.argmax(np.abs(r["direct"])))
    return kind, h[:last], peak, r["rt60"].astype(np.float32)

if __name__ == "__main__":
    n, out = int(sys.argv[1]), sys.argv[2]
    with ProcessPoolExecutor(14) as ex:
        res = list(ex.map(one, range(n), chunksize=8))
    lens = np.array([len(r[1]) for r in res])
    flat = np.concatenate([r[1] for r in res])
    np.savez(out, h=flat, offsets=np.concatenate([[0], np.cumsum(lens)]), peak=np.array([r[2] for r in res]),
             kind=np.array([r[0] for r in res]), rt60=np.stack([r[3] for r in res]))
    import collections
    print(len(res), "rooms,", flat.nbytes / 1e6, "MB,", collections.Counter(r[0] for r in res))
