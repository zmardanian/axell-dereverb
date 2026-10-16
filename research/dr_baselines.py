"""Baselines for de-reverberation (single channel, offline):
  identity        the input
  wpe             weighted prediction error (Nakatani et al. 2010; Yoshioka & Nakatani 2012), nara_wpe (MIT)
  latesub         late-reverberation spectral suppression (Lebart et al. 2001; Habets 2007), oracle RT60 (an
                  upper bound for this family: the method is given the room's true decay)"""
import numpy as np
from scipy import signal as sg

FS = 48000

def identity(x, room=None):
    return x.copy()

def wpe(x, room=None, taps=10, delay=3, iterations=3, n=1536, h=384):
    from nara_wpe.wpe import wpe as _wpe
    from nara_wpe.utils import stft, istft
    Y = stft(x[None, :], size=n, shift=h)                          # (D=1, T, F)
    Z = _wpe(Y.transpose(2, 0, 1), taps=taps, delay=delay, iterations=iterations, statistics_mode="full").transpose(1, 2, 0)
    z = istft(Z, size=n, shift=h)[0]
    out = np.zeros(len(x)); m = min(len(x), len(z)); out[:m] = z[:m]
    return out

def latesub(x, room, n=1024, h=256, td=0.05, gmin_db=-20.0):
    rt = float(np.mean(room["rt60"][2:5])) if "rt60" in room else 0.4
    delta = 3 * np.log(10) / rt
    f, t, Y = sg.stft(x, FS, nperseg=n, noverlap=n - h)
    P = np.abs(Y) ** 2
    nd = max(1, int(round(td * FS / h)))
    L = np.zeros_like(P)
    L[:, nd:] = np.exp(-2 * delta * td) * P[:, :-nd]
    G = np.maximum(1 - L / (P + 1e-20), 10 ** (gmin_db / 10)) ** 0.5
    G = sg.lfilter([0.5], [1, -0.5], G, axis=1)                    # smooth the gain over time (musical noise)
    _, z = sg.istft(Y * G, FS, nperseg=n, noverlap=n - h)
    out = np.zeros(len(x)); m = min(len(x), len(z)); out[:m] = z[:m]
    return out

METHODS = dict(identity=identity, wpe=wpe, latesub=latesub)

# ---------------------------------------------------------------------------------------------- the Axell network
_NETS = {}
def axell_net(x, room=None, ckpt=None):
    """offline inference of the trained network on a whole take; the look-ahead delay is removed"""
    import os, torch, dr_model as DM
    ckpt = ckpt or os.environ.get("AXDR_CKPT")
    if ckpt not in _NETS:
        net = DM.DRNet(); sd = torch.load(ckpt, map_location="cpu")
        net.load_state_dict(sd["net"] if "net" in sd else sd); net.eval(); _NETS[ckpt] = net
        torch.set_num_threads(1)
    net = _NETS[ckpt]
    pad = DM.HOP                                        # the streaming engine's first frame: half zeros, half signal
    xp = np.concatenate([np.zeros(pad), x, np.zeros(pad + DM.LA * DM.HOP)]).astype(np.float32)
    X = DM.stft_np(xp)
    # until the first frame that is not digital silence the network stays as at the start (as the C++ engine does):
    # the stream effectively starts there
    live = np.flatnonzero(~DM.silent_frames(X)); t0 = int(live[0]) if len(live) else len(X)
    Y = np.zeros_like(X)
    if t0 < len(X):
        F = DM.features_np(X[t0:])
        with torch.no_grad():
            Y[t0:] = net(torch.tensor(F)[None], torch.tensor(X[t0:])[None])[0][0].numpy()
    y = DM.istft_np(Y)
    s = pad + DM.LA * DM.HOP
    out = np.zeros(len(x)); seg = y[s:s + len(x)]; out[:len(seg)] = seg
    return out

METHODS["axell"] = axell_net

def _mix(a):
    def f(x, room=None):
        return (1 - a) * x + a * axell_net(x, room)
    return f
METHODS["axell85"] = _mix(0.85)
METHODS["axell70"] = _mix(0.70)
