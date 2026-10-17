"""De-reverb test and training material with ground truth, at 48 kHz.

A dry vocal (CMU ARCTIC / VCTK voices, through Axell Gate's corpus loaders) is placed in a room (rooms.py):
  input  = dry * (direct + early + late) + room tone
  target = dry * direct                     (the voice as the mic would hear it in free space)
The components are returned separately, so every method can be scored on what it removes (early reflections,
late tail) and on what it damages (the direct sound)."""
import os, sys, zlib, numpy as np
from scipy import signal as sg
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.expanduser("~/axell-gate/research"))
import rooms as RM
import corpus as GC                       # Axell Gate's corpus: CMU ARCTIC + VCTK loaders, 48 kHz

FS = RM.FS
ARCTIC = ("slt", "bdl", "jmk")            # held out for evaluation
def vctk():
    return GC.vctk_speakers()

def dry_take(spk, n_utt, seed, gap=None):
    """a dry take: utterances trimmed to their speech, joined with short (rap-like) pauses; -18 dBFS RMS on speech"""
    # gaps between utterances: 80-500 ms; AXDR_GAP="0,0.12" for dense rap-like takes (bench_dr.py --dense)
    gap = gap or tuple(float(v) for v in os.environ.get("AXDR_GAP", "0.08,0.5").split(","))
    rng = np.random.default_rng(zlib.crc32(f"drtake/{spk}/{seed}".encode()))
    utts = GC.utterances(spk); idx = rng.choice(len(utts), n_utt, replace=False)
    parts, labs = [], []
    for i in idx:
        name, w, l = utts[i]
        fs0, x, _, ph = GC.load(spk, w, l)
        if fs0 != FS:
            from math import gcd
            g = gcd(FS, fs0); x = sg.resample_poly(x, FS // g, fs0 // g)
        lab = np.zeros(len(x), bool)
        for a, b, p in ph:
            if p not in ("pau", "gap"):
                lab[int(a * FS):int(b * FS)] = True
        on = np.where(lab)[0]
        if len(on) == 0:
            continue
        a = max(0, on[0] - int(0.03 * FS)); b = min(len(x), on[-1] + int(0.03 * FS))
        parts.append(x[a:b] * 10 ** (rng.uniform(-3, 3) / 20)); labs.append(lab[a:b])
        n = int(rng.uniform(*gap) * FS)
        parts.append(np.zeros(n)); labs.append(np.zeros(n, bool))
    x = np.concatenate(parts); lab = np.concatenate(labs)
    x -= np.mean(x)
    x = sg.sosfiltfilt(sg.butter(2, 50, "high", fs=FS, output="sos"), x)   # recording DC / rumble out of the dry
    x *= 10 ** (-18 / 20) / np.sqrt(np.mean(x[lab] ** 2))
    return x, lab

def reverberate(dry, room, rng, tone_db=-70.0):
    n = len(dry)
    conv = lambda h: sg.fftconvolve(dry, h)[:n]
    direct, early, late = conv(room["direct"]), conv(room["early"]), conv(room["late"])
    tone = sg.lfilter([1], [1, -0.9], rng.standard_normal(n)); tone *= 10 ** (tone_db / 20) / np.std(tone)
    mix = direct + early + late + tone
    return dict(input=mix, target=direct, early=early, late=late, tone=tone)

def example(spk, kind, seed, n_utt=4, measured=None):
    """one scored example: a voice in a room of the given kind (synthetic) or a measured RIR file"""
    rng = np.random.default_rng(zlib.crc32(f"drex/{spk}/{kind}/{seed}/{measured}".encode()))
    dry, lab = dry_take(spk, n_utt, seed)
    room = RM.measured(measured) if measured else RM.random_room(kind, rng)
    d = reverberate(dry, room, rng)
    d.update(lab=lab, kind=kind, spk=spk, room=room)
    return d
