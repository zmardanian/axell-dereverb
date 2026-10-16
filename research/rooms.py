"""Room impulse responses at the vocal mic, at 48 kHz.

SYNTHETIC SMALL ROOMS (the rooms vocals are recorded in; real measured responses of wardrobe booths, closets and cars
are rare in open datasets):
  - image-source method (Allen & Berkley 1979) in a shoebox, with frequency-dependent wall absorption in 7 octave bands
    (125 Hz - 8 kHz), a cardioid mic aimed at the mouth and a forward-leaning mouth;
  - beyond ~60 ms (or the image order limit) a statistical tail: noise decaying at each band's Eyring RT60,
    energy-matched to the image-source part where they meet;
  - everything is split into DIRECT (the direct sound alone), EARLY (reflections up to 50 ms: the coloration a close
    mic picks up in a small room) and LATE (the tail).
MEASURED ROOMS (bandlimited: their direct sound is replaced by the full-band direct impulse; only their reflections
are used, which carry little above 8-16 kHz anyway):
  - BUT ReverbDB (CC BY 4.0 / Apache 2.0): training and evaluation;
  - MIT IR Survey (licence stated only on a mirror): evaluation only.
"""
import os, glob, wave, numpy as np
from scipy import signal as sg

FS = 48000
BANDS = np.array([125, 250, 500, 1000, 2000, 4000, 8000.0])
C = 343.0

# absorption coefficients per octave band (125 .. 8k), from standard tables (rounded)
MATERIALS = {
    "foam":      [0.10, 0.25, 0.55, 0.85, 0.92, 0.95, 0.95],   # 5 cm acoustic foam
    "foam_thin": [0.05, 0.10, 0.25, 0.55, 0.75, 0.85, 0.85],   # 2-3 cm foam
    "clothes":   [0.15, 0.30, 0.45, 0.55, 0.60, 0.65, 0.65],   # hanging clothes
    "wood":      [0.28, 0.22, 0.17, 0.09, 0.10, 0.11, 0.11],   # wardrobe panels, doors
    "drywall":   [0.29, 0.10, 0.05, 0.04, 0.07, 0.09, 0.09],
    "plaster":   [0.01, 0.02, 0.02, 0.03, 0.04, 0.05, 0.05],   # bare painted walls
    "concrete":  [0.01, 0.01, 0.02, 0.02, 0.02, 0.03, 0.03],
    "tile":      [0.01, 0.01, 0.01, 0.02, 0.02, 0.02, 0.02],
    "glass":     [0.35, 0.25, 0.18, 0.12, 0.07, 0.04, 0.04],
    "woodfloor": [0.15, 0.11, 0.10, 0.07, 0.06, 0.07, 0.07],
    "carpet":    [0.02, 0.06, 0.14, 0.37, 0.60, 0.65, 0.65],
    "upholstery":[0.20, 0.35, 0.50, 0.60, 0.65, 0.65, 0.65],   # car seats, sofa
    "car_roof":  [0.10, 0.20, 0.35, 0.45, 0.50, 0.50, 0.50],
}

# room types: dimensions (m) ranges and wall materials [x0, x1, y0, y1, floor, ceiling]
ROOM_TYPES = {
    "booth":   dict(L=(0.8, 1.3), W=(0.55, 0.8), H=(1.9, 2.2), walls=["foam", "foam", "foam_thin", "clothes", "wood", "wood"]),
    "closet":  dict(L=(1.0, 2.0), W=(0.8, 1.5), H=(2.2, 2.5), walls=["clothes", "wood", "drywall", "clothes", "carpet", "drywall"]),
    "bedroom": dict(L=(2.8, 4.0), W=(2.5, 3.5), H=(2.4, 2.7), walls=["drywall", "plaster", "glass", "drywall", "woodfloor", "plaster"]),
    "bedroom_furnished": dict(L=(3.0, 4.2), W=(2.8, 3.8), H=(2.4, 2.7), walls=["drywall", "upholstery", "glass", "clothes", "carpet", "plaster"]),
    "bathroom":dict(L=(1.8, 2.8), W=(1.5, 2.2), H=(2.3, 2.6), walls=["tile", "tile", "tile", "glass", "tile", "plaster"]),
    "car":     dict(L=(1.6, 2.0), W=(1.3, 1.5), H=(1.0, 1.2), walls=["glass", "upholstery", "glass", "glass", "carpet", "car_roof"]),
    "living":  dict(L=(4.0, 6.0), W=(3.5, 5.0), H=(2.5, 2.9), walls=["plaster", "glass", "drywall", "upholstery", "woodfloor", "plaster"]),
    "treated": dict(L=(2.5, 3.5), W=(2.0, 3.0), H=(2.4, 2.7), walls=["foam", "foam", "foam", "foam", "carpet", "foam"]),
}

def _frac_delay(n, d, taps=32):
    """windowed-sinc fractional delay kernel placed at sample d (float); returns (start index, kernel)"""
    i0 = int(np.floor(d)) - taps // 2 + 1
    k = np.arange(taps) + i0
    h = np.sinc(k - d) * np.hanning(taps + 2)[1:-1]
    return i0, h

def eyring_rt60(dims, alphas_b):
    L, W, H = dims
    S = np.array([W * H, W * H, L * H, L * H, L * W, L * W])
    V = L * W * H
    a = np.clip(np.sum(S[:, None] * alphas_b, 0) / S.sum(), 1e-3, 0.99)
    return 0.161 * V / (-S.sum() * np.log(1 - a))

def shoebox(dims, src, mic, walls, rng, max_ms=None, mic_dir=None):
    """image-source RIR (direct / early / late) for a shoebox, 7-band absorption, cardioid mic"""
    L, W, H = dims
    alphas = np.array([MATERIALS[w] for w in walls])               # 6 x 7
    refl = np.sqrt(1 - alphas)                                      # pressure reflection per band
    rt = eyring_rt60(dims, alphas)
    tmax = max_ms / 1000 if max_ms else min(2.5, 1.3 * rt.max() + 0.05)
    n = int(tmax * FS) + 64
    ism_ms = 60.0
    order = int(np.ceil(ism_ms / 1000 * C / min(L, W, H))) + 1
    order = min(order, 40)
    src = np.asarray(src); mic = np.asarray(mic)
    if mic_dir is None:
        mic_dir = (src - mic) / np.linalg.norm(src - mic)
    trains = np.zeros((len(BANDS), n))
    direct = np.zeros(n)
    d0 = np.linalg.norm(src - mic)
    rng_axes = range(-order, order + 1)
    for nx in rng_axes:
        for ny in rng_axes:
            for nz in rng_axes:
                if abs(nx) + abs(ny) + abs(nz) > order:
                    continue
                g = np.ones(len(BANDS)); pos = np.empty(3)
                for ax, (nn, Ld, (w0, w1)) in enumerate(zip((nx, ny, nz), (L, W, H), ((0, 1), (2, 3), (4, 5)))):
                    # image coordinate (mirror about the walls at 0 and Ld, alternately)
                    pos[ax] = nn * Ld + src[ax] if nn % 2 == 0 else (nn + 1) * Ld - src[ax]
                    # hits: n > 0 starts at the far wall (Ld), n < 0 at the near wall (0)
                    m = abs(nn)
                    far = (m + 1) // 2 if nn > 0 else m // 2
                    near = m - far
                    g = g * refl[w0] ** near * refl[w1] ** far
                v = pos - mic; r = np.linalg.norm(v)
                t = r / C
                if t > ism_ms / 1000 and (nx, ny, nz) != (0, 0, 0):
                    continue
                cos_m = np.dot(v / r, mic_dir)
                mic_g = 0.5 + 0.5 * cos_m                               # cardioid
                gain = mic_g / max(r, 0.05)
                i0, k = _frac_delay(n, t * FS)
                if i0 < 0 or i0 + len(k) > n:
                    continue
                if (nx, ny, nz) == (0, 0, 0):
                    direct[i0:i0 + len(k)] += gain * k
                else:
                    for b in range(len(BANDS)):
                        trains[b, i0:i0 + len(k)] += gain * g[b] * k
    # bands: octave bandpass (zero phase), summed; the lowest and highest bands are shelved open
    out = np.zeros(n)
    tail = np.zeros(n)
    t_axis = np.arange(n) / FS
    t_join = ism_ms / 1000
    j0 = int((t_join - 0.01) * FS); j1 = int(t_join * FS)
    for b, fc in enumerate(BANDS):
        lo, hi = fc / np.sqrt(2), fc * np.sqrt(2)
        if b == 0:
            sos = sg.butter(4, hi, "low", fs=FS, output="sos")
        elif b == len(BANDS) - 1:
            sos = sg.butter(4, lo, "high", fs=FS, output="sos")
        else:
            sos = sg.butter(3, [lo, hi], "band", fs=FS, output="sos")
        x = sg.sosfiltfilt(sos, trains[b])
        out += x
        # statistical tail for this band beyond the image-source part, energy-matched at the join
        e_join = np.mean(x[j0:j1] ** 2) if j1 < n else 0.0
        if e_join > 0 and n > j1:
            nz = sg.sosfiltfilt(sos, rng.standard_normal(n))
            env = np.exp(-6.91 * (t_axis - t_join) / rt[b])
            nz_part = nz * env
            nz_part *= np.sqrt(e_join / (np.mean(nz[j0:j1] ** 2) + 1e-30))
            fade = np.clip((t_axis - (t_join - 0.005)) / 0.005, 0, 1)
            tail += nz_part * fade
    # cross-fade the image-source part out where the tail takes over
    fade_out = np.clip(1 - (t_axis - (t_join - 0.005)) / 0.005, 0, 1)
    refl_part = out * fade_out + tail
    norm = np.max(np.abs(direct)) + 1e-12
    early = refl_part.copy(); early[int(0.05 * FS):] = 0
    late = refl_part.copy(); late[:int(0.05 * FS)] = 0
    return dict(direct=direct / norm, early=early / norm, late=late / norm, rt60=rt, dims=dims, d=d0)

def random_room(kind, rng):
    t = ROOM_TYPES[kind]
    dims = np.array([rng.uniform(*t["L"]), rng.uniform(*t["W"]), rng.uniform(*t["H"])])
    # the singer: mouth at 1.5-1.7 m (standing) or ~1.2 m in a car; mic 10-30 cm in front, slightly off-axis
    mouth_h = rng.uniform(1.0, 1.15) if kind == "car" else min(dims[2] - 0.3, rng.uniform(1.45, 1.7))
    margin = 0.25 if kind in ("booth", "car") else 0.6
    mx, my = min(margin, dims[0] / 2 - 0.1), min(margin, dims[1] / 2 - 0.1)
    src = np.array([rng.uniform(mx, dims[0] - mx), rng.uniform(my, dims[1] - my), mouth_h])
    ang = rng.uniform(0, 2 * np.pi); dist = rng.uniform(0.10, 0.30)
    mic = src + np.array([dist * np.cos(ang), dist * np.sin(ang), rng.uniform(-0.05, 0.05)])
    mic = np.clip(mic, 0.08, dims - 0.08)
    walls = list(t["walls"])
    if rng.random() < 0.5:                                         # rooms differ: shuffle the side walls
        sides = walls[:4]; rng.shuffle(sides); walls[:4] = sides
    r = shoebox(dims, src, mic, walls, rng)
    r["kind"] = kind
    return r

# -------------------------------------------------------------------------------------------------- measured rooms
def _read(path):
    w = wave.open(path); fs = w.getframerate(); ch = w.getnchannels(); sw = w.getsampwidth()
    raw = w.readframes(w.getnframes())
    if sw == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3)
        x = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16))
        x = np.where(x >= 1 << 23, x - (1 << 24), x) / float(1 << 23)
    else:
        x = np.frombuffer(raw, np.int16) / 32768.0
    return fs, x.reshape(-1, ch)[:, 0].astype(np.float64)

def measured(path):
    """a measured RIR, split like the synthetic ones: the direct sound becomes a full-band impulse (the measurement's
    own direct sound is bandlimited), the reflections are resampled to 48 kHz"""
    fs, h = _read(path)
    from math import gcd
    g = gcd(FS, fs); h = sg.resample_poly(h, FS // g, fs // g)
    k = int(np.argmax(np.abs(h)))
    pre = int(0.0015 * FS)                                          # the direct sound: +-1.5 ms around the peak
    peak = h[k]
    direct = np.zeros(len(h)); direct[k] = 1.0
    refl = h.copy(); refl[max(0, k - pre):k + pre] = 0.0
    refl[:max(0, k - pre)] = 0.0                                    # nothing before the direct sound
    refl /= peak
    early = refl.copy(); early[k + int(0.05 * FS):] = 0
    late = refl.copy(); late[:k + int(0.05 * FS)] = 0
    # remove the measurement's pre-delay so all rooms start at a few ms
    s = max(0, k - int(0.002 * FS))
    return dict(direct=direct[s:], early=early[s:], late=late[s:], path=path)

def mit_paths():
    return sorted(glob.glob(os.path.expanduser("~/axell-dereverb-data/rir/mit/Audio/*.wav")))
