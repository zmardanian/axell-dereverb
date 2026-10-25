"""Rooms like the artist's untreated recording room, for training and testing Axell De-Reverb on that setup.
Candidates: the simulator's untreated small rooms (bedroom, furnished bedroom, closet, living room, and bare rooms of
drywall / plaster / glass / wood floor), every material's absorption perturbed (x0.5-1.6) so rooms differ.
A candidate is kept when every octave band's RT60 (125 Hz-8 kHz) lies within the range measured over the artist's
takes (measure_setup.py: sessions differ, the bass most), widened by MARGIN 1.4. Matching through the measurement itself (a take simulated in the room, measured
the same way) was tried first: in rooms that read right, synthetic voices left the full-band level too high after word
endings for the detector (1-3 endings found), so it rejected most of them. FACTOR 1.8 is wider than the measurement's
own spread; the artist's real takes are the final check.
Train and test rooms come from different seeds. usage: python3 make_your_rooms.py [n_train] [n_test]
-> ~/axell-dereverb-data/your-setup/rooms_{train,test}.npz"""
import os, sys, json, zlib, numpy as np
from concurrent.futures import ProcessPoolExecutor
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rooms as RM, dr_corpus as DC, measure_setup as MS

OUT = os.path.expanduser("~/axell-dereverb-data/your-setup")
MARGIN = 1.4
BANDS = [125, 250, 500, 1000, 2000, 4000, 8000]
KINDS = {"bedroom_furnished": 4, "closet": 2, "bedroom": 2, "bare": 1}   # measured: soft furnishings, ringing bass
BARE = dict(L=(2.5, 4.5), W=(2.2, 3.8), H=(2.3, 2.8))

def target():
    """per band (low, high): the range over the artist's takes, widened by MARGIN (the measurement's own spread)"""
    r = json.load(open(f"{OUT}/setup.json"))["summary"]["room_range"]
    return {int(k): (v[0] / MARGIN, v[1] * MARGIN) for k, v in r.items()}

def candidate(seed):
    rng = np.random.default_rng(zlib.crc32(f"yourroom/{seed}".encode()))
    kinds = [k for k, w in KINDS.items() for _ in range(w)]; kind = kinds[rng.integers(len(kinds))]
    saved = {k: list(v) for k, v in RM.MATERIALS.items()}
    try:
        for k in RM.MATERIALS:
            # leaning toward the measurement: furnishings soak up the mids and highs, little soaks up the bass
            bias = np.concatenate([rng.uniform(0.3, 1.0, 2), rng.uniform(0.9, 1.8, 5)])
            RM.MATERIALS[k] = list(np.clip(np.array(saved[k]) * bias, 0.005, 0.95))
        if kind == "bare":
            walls = list(rng.choice(["drywall", "plaster", "glass", "drywall", "plaster", "wood"], 4)) + ["woodfloor", "plaster"]
            RM.ROOM_TYPES["bare"] = dict(**BARE, walls=walls)
        room = RM.random_room(kind, rng)
    finally:
        for k in saved:
            RM.MATERIALS[k] = saved[k]
    return kind, room

def evaluate(seed):
    kind, room = candidate(seed)
    t = target()
    r = dict(zip(BANDS, (float(v) for v in room["rt60"])))
    ok = all(t[b][0] <= r[b] <= t[b][1] for b in t)
    dist = float(np.sqrt(np.mean([np.log(r[b] / np.sqrt(t[b][0] * t[b][1])) ** 2 for b in t])))
    return seed, kind, dist if ok else None, r

if __name__ == "__main__":
    n_train = int(sys.argv[1]) if len(sys.argv) > 1 else 240
    n_test = int(sys.argv[2]) if len(sys.argv) > 2 else 24
    t = target(); print("the artist's room (decay after word endings, RT60 s):", t)
    found = {"train": [], "test": []}
    seed = 0
    with ProcessPoolExecutor(8) as ex:
        while len(found["train"]) < n_train or len(found["test"]) < n_test:
            batch = list(range(seed, seed + 64)); seed += 64
            for s, kind, dist, m in ex.map(evaluate, batch):
                if dist is not None:
                    split = "test" if s % 10 == 0 else "train"            # every 10th seed is a test room
                    if len(found[split]) < (n_test if split == "test" else n_train):
                        found[split].append((s, kind, dist))
            acc = len(found["train"]) + len(found["test"])
            print(f"{seed} candidates, {acc} kept ({100 * acc / seed:.1f} %)", flush=True)
            if seed > 40000:
                break
    for split, rows in found.items():
        H, D, kinds, dists = [], [], [], []
        for s, kind, dist in rows:
            _, room = candidate(s)
            H.append(np.asarray(room["direct"] + room["early"] + room["late"], np.float32)); D.append(np.asarray(room["direct"], np.float32))
            kinds.append(kind); dists.append(dist)
        L = max(len(h) for h in H)
        pad = lambda v: np.pad(v, (0, L - len(v)))
        np.savez(f"{OUT}/rooms_{split}.npz", h=np.stack([pad(h) for h in H]), d=np.stack([pad(v) for v in D]),
                 kind=np.array(kinds), dist=np.array(dists), seed=np.array([r[0] for r in rows]))
        print(split, len(rows), "rooms; kinds", {k: kinds.count(k) for k in set(kinds)}, "median distance", round(float(np.median(dists)), 3))
