"""Scores for de-reverberation, against the true direct sound. Standard, openly licensed measures:
  ESTOI     extended short-time objective intelligibility (Jensen & Taal 2016; pystoi, MIT)       higher = better
  SRMR      speech-to-reverberation modulation energy ratio (Falk et al. 2010; SRMRpy, MIT)      higher = less reverb
  CD        cepstral distance to the target, dB (REVERB challenge definition)                    lower = better
  FWSSNR    frequency-weighted segmental SNR, dB (REVERB challenge)                              higher = better
  SISDR     scale-invariant signal-to-distortion ratio to the target, dB                         higher = better
  LTAS      third-octave long-term spectrum error to the target, dB RMS 100 Hz - 8 kHz; BOX = the same 350-600 Hz
  DAMAGE    energy lost or distorted where the direct sound dominates (time-frequency), dB re those bins
  LEFT      room energy left where the room dominates, dB re the input there                     lower = more removed
  VISQOL    ViSQOL v3 audio mode, MOS-LQO 1-5 against the target (Hines et al. 2015; Chinen et al. 2020), via
            visqol-python (Apache-2.0; 12/12 conformance tests against Google's C++ implementation)  higher = better
No PESQ / POLQA (licence restrictions)."""
import numpy as np
from scipy import signal as sg

FS = 48000

def _stft(x, n=1024, h=256):
    _, _, Z = sg.stft(x, FS, nperseg=n, noverlap=n - h, boundary=None, padded=False)
    return Z

def estoi(target, est):
    from pystoi import stoi
    return float(stoi(target, est, FS, extended=True))

def srmr(x):
    from srmrpy import srmr as _s
    y = sg.resample_poly(x, 1, 3)
    return float(_s(y, 16000, fast=True)[0])

def cepstral_distance(target, est, order=24, n=400, h=100):
    """REVERB challenge definition: at 16 kHz, 25 ms frames / 6.25 ms hop, cepstra of the log magnitude spectrum up
    to order 24, mean over frames of (10 / ln 10) sqrt(2 sum_k>0 (dc_k)^2 + dc_0^2), each frame clipped to 10 dB"""
    t16, e16 = sg.resample_poly(target, 1, 3), sg.resample_poly(est, 1, 3)
    def ceps(x):
        f = np.lib.stride_tricks.sliding_window_view(x, n)[::h] * np.hamming(n)
        S = np.abs(np.fft.rfft(f, axis=1)) + 1e-9
        return np.fft.irfft(np.log(S), axis=1)[:, :order + 1]
    ct, ce = ceps(t16), ceps(e16)
    m = min(len(ct), len(ce))
    d = (10 / np.log(10)) * np.sqrt(2 * np.sum((ct[:m, 1:] - ce[:m, 1:]) ** 2, 1) + (ct[:m, 0] - ce[:m, 0]) ** 2)
    return float(np.mean(np.clip(d, 0, 10)))

def fwssnr(target, est):
    T, E = np.abs(_stft(target)) ** 2, np.abs(_stft(est)) ** 2
    f = np.fft.rfftfreq(1024, 1 / FS)
    edges = np.geomspace(100, 8000, 26)
    snr, w = [], []
    for a, b in zip(edges[:-1], edges[1:]):
        m = (f >= a) & (f < b)
        t = np.sum(T[m], 0); e = np.sum(E[m], 0)
        s = 10 * np.log10(t / (np.abs(np.sqrt(t) - np.sqrt(e)) ** 2 + 1e-12) + 1e-12)
        snr.append(np.clip(s, -10, 35)); w.append(t ** 0.2)
    snr = np.array(snr); w = np.array(w)
    frame = np.sum(snr * w, 0) / (np.sum(w, 0) + 1e-12)
    act = np.sum(T, 0) > np.percentile(np.sum(T, 0), 30)
    return float(np.mean(frame[act]))

def sisdr(target, est):
    a = np.dot(est, target) / (np.dot(target, target) + 1e-20)
    s = a * target; e = est - s
    return float(10 * np.log10(np.sum(s ** 2) / (np.sum(e ** 2) + 1e-20)))

def ltas_error(target, est):
    f, Pt = sg.welch(target, FS, nperseg=8192); _, Pe = sg.welch(est, FS, nperseg=8192)
    gain = np.sum(Pt) / (np.sum(Pe) + 1e-30)                     # level-matched: coloration only
    fc = 1000 * 2 ** (np.arange(-10, 10) / 3)
    err, box = [], []
    for c in fc:
        m = (f >= c / 2 ** (1 / 6)) & (f < c * 2 ** (1 / 6))
        d = 10 * np.log10((np.sum(Pe[m]) * gain + 1e-30) / (np.sum(Pt[m]) + 1e-30))
        err.append(d)
        if 350 <= c <= 600:
            box.append(d)
    return float(np.sqrt(np.mean(np.square(err)))), float(np.mean(box))

def damage_left(target, est, inp):
    """oracle diagnostics in the STFT domain: where the direct sound dominates the input, how much of it is lost or
    distorted; where the room dominates, how much of the input's energy is left"""
    T, E, Y = _stft(target), _stft(est), _stft(inp)
    m = min(T.shape[1], E.shape[1], Y.shape[1]); T, E, Y = T[:, :m], E[:, :m], Y[:, :m]
    R = Y - T                                                     # everything that is not the direct sound
    dom = np.abs(T) ** 2 > 4 * np.abs(R) ** 2                     # direct sound 6 dB above the room
    room = np.abs(R) ** 2 > 4 * np.abs(T) ** 2
    dmg = 10 * np.log10(np.sum((np.abs(E[dom]) - np.abs(T[dom])) ** 2) / (np.sum(np.abs(T[dom]) ** 2) + 1e-20) + 1e-20)
    left = 10 * np.log10(np.sum(np.abs(E[room]) ** 2) / (np.sum(np.abs(Y[room]) ** 2) + 1e-20) + 1e-20)
    return float(dmg), float(left)

_VQ = None
def visqol(target, est):
    global _VQ
    if _VQ is None:
        import logging
        logging.getLogger("visqol").setLevel(logging.ERROR)
        from visqol.api import VisqolApi
        _VQ = VisqolApi(); _VQ.create(mode="audio")
    return float(_VQ.measure_from_arrays(np.asarray(target, np.float64), np.asarray(est, np.float64), FS).moslqo)

def score(d, est, quick=False):
    t, x = d["target"], d["input"]
    n = min(len(t), len(est)); t, est, x = t[:n], est[:n], x[:n]
    lt, box = ltas_error(t, est)
    dmg, left = damage_left(t, est, x)
    out = dict(cd=cepstral_distance(t, est), fwssnr=fwssnr(t, est), sisdr=sisdr(t, est), ltas=lt, box=box,
               damage=dmg, left=left)
    if not quick:
        out.update(estoi=estoi(t, est), srmr=srmr(est), visqol=visqol(t, est))
    return out
