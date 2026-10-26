"""Axell De-Reverb network (torch, training side). Built only from layers with a direct C++ port: Linear, GRU,
sigmoid, tanh. Signal path at 48 kHz:
  STFT  1024-sample sqrt-Hann window, 512 hop (21.3 ms / 10.7 ms), 513 bins
  in    per frame: log power in 32 ERB bands (running-mean normalised) and the complex spectrum of bins 0..95
        (running unit-norm), for frames t-2 .. t+LA (LA frames of look-ahead)
  core  Linear -> 2 x GRU (the recurrent state learns the room over the take)
  out   32 ERB gains (sigmoid), applied to every bin by interpolation;
        a deep filter for bins 0..95: an order-5 complex filter over frames t-4 .. t of the (gain-weighted) spectrum,
        replacing those bins - it can undo early reflections and coloration, which a gain cannot."""
import os, numpy as np, torch, torch.nn as nn

FS = 48000
NFFT = int(os.environ.get("AXDR_NFFT", 1024)); HOP = NFFT // 2      # (960 only for the first pilot run)
NB = NFFT // 2 + 1
NERB = 32
NDF = 96            # deep-filtered bins (0 .. 4.8 kHz)
DF_ORDER = 5
LA = int(os.environ.get("AXDR_LA", 3))   # look-ahead frames (32 ms; 0 = causal, 3.0 LIVE)
CTX = 2             # past frames in the input
H = 256

def erb_matrix(nb=NB, nerb=NERB, fs=FS):
    """triangular-ish rectangular ERB bands: each bin to exactly one band (bins grouped on the ERB scale)"""
    f = np.linspace(0, fs / 2, nb)
    erb = lambda x: 21.4 * np.log10(1 + 0.00437 * x)
    e = erb(f); edges = np.linspace(0, e[-1], nerb + 1)
    idx = np.clip(np.searchsorted(edges, e, side="right") - 1, 0, nerb - 1)
    # guarantee at least 2 bins per band at the bottom: merge empty bands upwards
    M = np.zeros((nb, nerb), np.float32)
    M[np.arange(nb), idx] = 1.0
    for b in range(nerb):
        if M[:, b].sum() == 0:
            k = int(np.argmin(np.abs(edges[b] - e))); M[k, :] = 0; M[k, b] = 1.0
    return M

class DRNet(nn.Module):
    def __init__(self):
        super().__init__()
        M = torch.tensor(erb_matrix())
        self.register_buffer("erb_fb", M / M.sum(0, keepdim=True).clamp(min=1))   # bins -> bands (mean)
        self.register_buffer("erb_inv", M.t().contiguous())                         # bands -> bins
        nin = (CTX + 1 + LA) * (NERB + 2 * NDF)
        self.inp = nn.Linear(nin, H)
        self.gru1 = nn.GRU(H, H, batch_first=True)
        self.gru2 = nn.GRU(H, H, batch_first=True)
        self.out_g = nn.Linear(H, NERB)
        self.out_df = nn.Linear(H, NDF * DF_ORDER * 2)
        self.out_a = nn.Linear(H, NDF)            # per-bin mix of the deep filter vs the gain path

    def forward(self, F, X, h=None):
        """F: features (B, T, NERB + 2 NDF) from features_np; X: complex STFT (B, T, NB). Returns the enhanced complex
        STFT, delayed by LA frames (output frame t is input frame t - LA), and the GRU states"""
        B, T, Fd = F.shape
        pad = torch.zeros(B, CTX + LA, Fd, dtype=F.dtype, device=F.device)
        Fp = torch.cat([pad, F], 1)                              # Fp[j] = F[j - CTX - LA]
        # input for output frame t (= input frame t - LA): feature frames t - LA - CTX .. t = Fp[t .. t + CTX + LA]
        win = torch.stack([Fp[:, k:k + T] for k in range(CTX + 1 + LA)], 2)    # (B, T, CTX+1+LA, Fd), newest last
        z = torch.relu(self.inp(win.reshape(B, T, -1)))
        h1 = h[0] if h is not None else None; h2 = h[1] if h is not None else None
        z, h1 = self.gru1(z, h1)
        z2, h2 = self.gru2(z, h2)
        z = z + z2
        g = torch.sigmoid(self.out_g(z))                         # (B, T, NERB)
        gb = g @ self.erb_inv                                    # (B, T, NB)
        # the frame being produced: input frame t - LA
        Xd = torch.cat([torch.zeros(B, LA, NB, dtype=X.dtype, device=X.device), X[:, :T - LA]], 1)
        Y = Xd * gb
        coef = torch.tanh(self.out_df(z)).reshape(B, T, NDF, DF_ORDER, 2)
        coef = torch.complex(coef[..., 0], coef[..., 1])
        # deep filter over frames (t - LA) - 4 .. (t - LA) of the input spectrum (low bins)
        Xl = torch.cat([torch.zeros(B, DF_ORDER - 1, NDF, dtype=X.dtype, device=X.device), Xd[..., :NDF]], 1)
        stack = torch.stack([Xl[:, i:i + T] for i in range(DF_ORDER)], -1)   # (B, T, NDF, ORDER), oldest first
        ydf = torch.sum(stack * coef, -1)
        a = torch.sigmoid(self.out_a(z))
        low = a * ydf + (1 - a) * Y[..., :NDF]
        Y = torch.cat([low, Y[..., NDF:]], -1)
        return Y, (h1, h2)

ERB_M = erb_matrix()
ERB_FB = ERB_M / np.maximum(ERB_M.sum(0, keepdims=True), 1)

def stft_np(x):
    """numpy STFT identical to torch.stft(center=False) with the sqrt-Hann window: (T, NB) complex64"""
    w = np.sqrt(0.5 - 0.5 * np.cos(2 * np.pi * np.arange(NFFT) / NFFT)).astype(np.float32)
    n = 1 + (len(x) - NFFT) // HOP
    fr = np.lib.stride_tricks.sliding_window_view(x.astype(np.float32), NFFT)[::HOP][:n] * w
    return np.fft.rfft(fr, axis=1).astype(np.complex64)

SILENT_DB = -130.0   # a frame below this (mean bin power of white noise at -130 dBFS) is digital silence

def silent_frames(X):
    """frames of digital silence (a DAW before the first take, a gate in front): mean bin power below SILENT_DB"""
    P = (X.real ** 2 + X.imag ** 2).astype(np.float64)
    return P.mean(1) < 512.0 * 10 ** (SILENT_DB / 10)

def features_np(X, alpha=0.99):
    """X (T, NB) complex -> (T, NERB + 2 NDF): ERB log power minus its running mean (/4), and the low bins' complex
    spectrum over their running mean magnitude. Running means start at the first frame's values and do not move
    during digital silence (training never had any: it would drag the means far outside it). Same in C++."""
    from scipy.signal import lfilter
    P = (X.real ** 2 + X.imag ** 2).astype(np.float64)
    E = np.log10(P @ ERB_FB + 1e-10)
    mag = np.sqrt(P[:, :NDF] + 1e-10)
    sil = silent_frames(X)
    if not sil.any():
        m = lfilter([1 - alpha], [1, -alpha], E, axis=0, zi=(alpha * E[:1]))[0]
        u = lfilter([1 - alpha], [1, -alpha], mag, axis=0, zi=(alpha * mag[:1]))[0]
    else:
        m = np.empty_like(E); u = np.empty_like(mag); mi, ui = E[0].copy(), mag[0].copy()
        for t in range(len(P)):
            if t > 0 and not sil[t]:
                mi = alpha * mi + (1 - alpha) * E[t]; ui = alpha * ui + (1 - alpha) * mag[t]
            m[t] = mi; u[t] = ui
    c = X[:, :NDF] / np.maximum(u, 1e-5)
    return np.concatenate([(E - m) / 4.0, c.real, c.imag], 1).astype(np.float32)

def istft_np(Y):
    """overlap-add inverse of stft_np (sqrt-Hann synthesis; Hann at 50 % overlap sums to 1) - the C++ engine's way"""
    w = np.sqrt(0.5 - 0.5 * np.cos(2 * np.pi * np.arange(NFFT) / NFFT)).astype(np.float32)
    fr = np.fft.irfft(Y, NFFT, axis=1) * w
    out = np.zeros((len(Y) - 1) * HOP + NFFT, np.float32)
    for t in range(len(Y)):
        out[t * HOP:t * HOP + NFFT] += fr[t]
    return out

def stft(x):
    w = torch.sqrt(torch.hann_window(NFFT, periodic=True, device=x.device))
    return torch.stft(x, NFFT, HOP, window=w, return_complex=True, center=False).transpose(1, 2)   # (B, T, NB)

def istft(Y, length):
    w = torch.sqrt(torch.hann_window(NFFT, periodic=True, device=Y.device))
    return torch.istft(Y.transpose(1, 2), NFFT, HOP, window=w, center=False, length=length)

def loss_fn(Y, T_):
    """compressed spectral loss (magnitude and complex, |.|^0.3) between the output and the target STFT"""
    c = 0.3
    my, mt = (Y.abs() + 1e-8), (T_.abs() + 1e-8)
    yc = Y / my * my ** c; tc = T_ / mt * mt ** c
    mag = torch.mean((my ** c - mt ** c) ** 2)
    cpx = torch.mean((yc - tc).abs() ** 2)
    return mag + 0.3 * cpx
