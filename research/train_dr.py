"""Train the Axell De-Reverb network on generated examples (dr_model.DRNet).
Each example: 3 s of a training voice (VCTK, minus held-out voices; the CMU ARCTIC voices are never used), in a random
room from the bank (90 %) or none (10 %: the network must leave a dry take untouched), with room tone, a random mic EQ
and level. Target: the voice through the room's direct sound only.
Fine-tune with singing (1.0 final): --sing P uses VocalSet singers (minus the 4 held out by bench_sing.py) for a
fraction P of the examples, --init <weights> starts from trained weights, --lr <peak> sets the peak learning rate.
2.0 (rap / spoken word first): --dense P builds a fraction P of the speech examples as dense takes (trimmed
utterances, 0-120 ms gaps), and home-recording rooms are drawn 1.5x as often.
2.1 (the artist's setup: untreated room -> LCT 440 PURE -> Volt 176): --setup P builds a fraction P of the examples
in rooms measured to read like the artist's (make_your_rooms.py), with room noise shaped like the artist's floor
(measure_setup.py) 22-40 dB under the voice, and half of them through a 1176-style compressor (the Volt's 76) after
the room; its gain is applied to the target too (the network removes the room and the noise, not the compression).
usage: python train_dr.py <run dir> [steps] [--resume] [--sing 0.4] [--dense 0.6] [--setup 0.5] [--init net.pt] [--lr 3e-4]"""
import os, sys, glob, wave, zlib, time, numpy as np, torch
from scipy import signal as sg
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dr_model as DM

FS = DM.FS
DATA = os.path.expanduser("~/axell-dereverb-data")
HELD_OUT = {"p225", "p226", "p294", "p311"}
SEG = 3.0

def voices():
    sp = [l.split()[0] for l in open(f"{DATA}/vctk/speakers.txt") if l.strip()]
    out = {}
    for s in sp:
        if s in HELD_OUT:
            continue
        fs = sorted(glob.glob(f"{DATA}/vctk/{s}/*.wav"))
        fs = [f for f in fs if not f.endswith(".tmp.wav")]
        if len(fs) >= 10:
            out[s] = fs
    return out

VOCALSET = f"{DATA}/vocalset"
SING_HELD_OUT = {"female2", "female8", "male3", "male9"}       # bench_sing.py's test singers: never trained on

def singers():
    out = {}
    for d in sorted(glob.glob(f"{VOCALSET}/*")):
        if os.path.basename(d) in SING_HELD_OUT:
            continue
        fs = sorted(glob.glob(f"{d}/*.wav"))
        if fs:
            out["sing:" + os.path.basename(d)] = fs
    return out

def read_any(p):
    """a VCTK file (48 kHz) or a VocalSet file (44.1 kHz, resampled to 48 kHz)"""
    w = wave.open(p); fs = w.getframerate()
    x = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(np.float32) / 32768.0
    return sg.resample_poly(x, 160, 147).astype(np.float32) if fs == 44100 else x

def read(p):
    w = wave.open(p); x = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(np.float32) / 32768.0
    return x

HOME_KINDS = {"booth", "closet", "bedroom", "bedroom_furnished", "bathroom"}
BUT_HELD_OUT = {"Hotel_SkalskyDvur_Room112", "VUT_FIT_E112"}

class Rooms:
    """70 % simulated small rooms, 20 % measured BUT rooms (reflections rescaled to a close vocal mic: direct-to-
    reverberant ratio 2-20 dB), 10 % no room (handled by the caller)"""
    def __init__(self, path=f"{DATA}/rooms_train", but=f"{DATA}/rooms_but"):
        # memory-mapped .npy: every data worker shares one copy through the page cache
        ld = lambda d, k: np.load(f"{d}/{k}.npy", mmap_mode="r")
        self.h, self.off, self.peak = ld(path, "h"), ld(path, "offsets"), ld(path, "peak")
        # 2.0: home-recording rooms (booth, closet, bedrooms, bathroom) drawn 1.5x as often as car / living / treated
        kind = np.load(f"{path}/kind.npy", allow_pickle=True)
        w = np.array([1.5 if k in HOME_KINDS else 1.0 for k in kind]); self.cw = np.cumsum(w / w.sum())
        self.but = None
        if os.path.exists(but):
            room = ld(but, "room")
            keep = np.array([r not in BUT_HELD_OUT for r in room])
            self.but = (ld(but, "refl"), ld(but, "offsets"), ld(but, "peak"), ld(but, "drr"), np.nonzero(keep)[0])
    def __len__(self):
        return len(self.peak)
    def get(self, i):
        h = self.h[self.off[i]:self.off[i + 1]].astype(np.float64)
        p = int(self.peak[i])
        d = np.zeros_like(h); a, b = max(0, p - 16), min(len(h), p + 17); d[a:b] = h[a:b]
        return h, d
    def get_any(self, rng):
        if self.but is not None and rng.random() < 0.22:
            refl, off, peak, drr, idx = self.but
            j = idx[rng.integers(len(idx))]
            r = refl[off[j]:off[j + 1]].astype(np.float64)
            g = 10 ** ((drr[j] - rng.uniform(2, 20)) / 20)
            h = r * g; p = int(peak[j]); h[p] += 1.0
            d = np.zeros_like(h); d[p] = 1.0
            return h, d
        return self.get(min(len(self) - 1, int(np.searchsorted(self.cw, rng.random()))))

SETUP = f"{DATA}/your-setup"

class YourSetup:
    """the artist's recording setup: matched rooms, the measured noise floor's spectrum, the Volt's 76"""
    def __init__(self, split="train"):
        import json
        z = np.load(f"{SETUP}/rooms_{split}.npz")
        self.h, self.d = z["h"], z["d"]
        nz = json.load(open(f"{SETUP}/setup.json"))["takes"][0]["noise_third_octave"]
        self.nf = np.array([float(k) for k in nz]); self.nd = np.array(list(nz.values()))
    def room(self, rng):
        i = rng.integers(len(self.h))
        return self.h[i].astype(np.float64), self.d[i].astype(np.float64)
    def noise(self, n, rng):
        f = np.fft.rfftfreq(n, 1 / FS)
        mag = 10 ** (np.interp(np.log(np.maximum(f, 20.0)), np.log(self.nf), self.nd) / 20) * 10 ** (rng.normal(0, 1.5, len(self.nd)).mean() / 20)
        w = np.fft.irfft(np.fft.rfft(rng.standard_normal(n)) * mag, n)
        drift = 10 ** (rng.uniform(0.5, 2.0) * np.sin(2 * np.pi * np.arange(n) / FS / rng.uniform(3, 12) + rng.uniform(0, 6)) / 20)
        return w * drift / (np.std(w) + 1e-12)

def fet_gain(x, rng):
    """a 1176-style compressor's gain on x (the Volt 176's 76): attack 0.2-0.8 ms, release 50-400 ms, ratio 4-8,
    threshold for 2-10 dB on the peaks; run on 0.5 ms blocks (peak), interpolated to samples"""
    B = 24; n = len(x) // B * B
    pk = 20 * np.log10(np.abs(x[:n]).reshape(-1, B).max(1) + 1e-9)
    thr = np.percentile(pk, 99) - rng.uniform(2, 10) * 1.3; ratio = rng.uniform(4, 8)
    a = np.exp(-B / (rng.uniform(0.2, 0.8) * 1e-3 * FS)); r = np.exp(-B / (rng.uniform(50, 400) * 1e-3 * FS))
    over = np.maximum(pk - thr, 0.0); env = np.empty_like(over); e = 0.0
    for i, v in enumerate(over):
        e = a * e + (1 - a) * v if v > e else r * e + (1 - r) * v
        env[i] = e
    g = 10 ** (-env * (1 - 1 / ratio) / 20)
    gs = np.interp(np.arange(len(x)), np.arange(len(g)) * B + B / 2, g)
    return gs

class Gen(torch.utils.data.IterableDataset):
    def __init__(self, seed=0, sing=0.0, dense=0.0, setup=0.0):
        self.seed, self.sing, self.dense, self.setup = seed, sing, dense, setup
    def __iter__(self):
        info = torch.utils.data.get_worker_info()
        rng = np.random.default_rng(self.seed * 1000 + (info.id if info else 0) + int(time.time() * 1000) % 100000)
        V = voices(); names = list(V); rooms = Rooms()
        S = singers() if self.sing > 0 else {}; snames = list(S); V.update(S)
        ys = YourSetup() if self.setup > 0 else None
        while True:
            if ys is not None and rng.random() < self.setup:
                yield example(rng, V, names, rooms, 0.8, setup=ys)
                continue
            pick = snames if snames and rng.random() < self.sing else names
            yield example(rng, V, pick, rooms, self.dense)

def trim(x, rng):
    """the recording without its leading and trailing silence (a 20-40 ms margin)"""
    e = np.flatnonzero(np.abs(x) > 10 ** (-40 / 20) * (np.abs(x).max() + 1e-9))
    if len(e) == 0:
        return x
    m = int(rng.uniform(0.02, 0.04) * FS)
    return x[max(0, e[0] - m):e[-1] + m]

def example(rng, V, names, rooms, dense=0.0, setup=None):
    spk = names[rng.integers(len(names))]
    n = int((SEG + 1.0) * FS)
    parts, tot = [], 0
    # 2.0: rap / spoken-word delivery - utterances trimmed and joined with 0-120 ms gaps (now and then a breath-length
    # 150-400 ms), so words follow each other while the room of the last one still rings
    rap = (not spk.startswith("sing:")) and rng.random() < dense
    while tot < n:
        x = read_any(V[spk][rng.integers(len(V[spk]))]).astype(np.float64)
        if rap:
            x = trim(x, rng)
        x *= 10 ** (rng.uniform(-6, 6) / 20)
        g = (rng.uniform(0.15, 0.4) if rng.random() < 0.15 else rng.uniform(0.0, 0.12)) if rap else rng.uniform(0.05, 0.6)
        gap = np.zeros(int(g * FS))
        parts += [x, gap]; tot += len(x) + len(gap)
    dry = np.concatenate(parts)[:n]
    dry = sg.sosfilt(sg.butter(2, 40, "high", fs=FS, output="sos"), dry)
    if setup is not None:
        h, d = setup.room(rng)
    elif rng.random() < 0.9:
        h, d = rooms.get_any(rng)
    else:
        h = np.zeros(32); h[16] = 1.0; d = h.copy()
    x = sg.fftconvolve(dry, h)[:n]
    t = sg.fftconvolve(dry, d)[:n]
    # the mic and preamp: a mild random tilt / bell, applied to both (it is not the room)
    if rng.random() < 0.4:
        from scipy.signal import lfilter
        f0 = rng.uniform(150, 8000); gdb = rng.uniform(-4, 4); q = rng.uniform(0.5, 2)
        A = 10 ** (gdb / 40); w0 = 2 * np.pi * f0 / FS; al = np.sin(w0) / (2 * q)
        b = [1 + al * A, -2 * np.cos(w0), 1 - al * A]; a = [1 + al / A, -2 * np.cos(w0), 1 - al / A]
        x = lfilter(b, a, x); t = lfilter(b, a, t)
    # room tone
    rms = np.sqrt(np.mean(t ** 2) + 1e-12)
    if setup is not None:                       # the artist's floor: 22-40 dB under the voice, then maybe the 76
        x = x + setup.noise(n, rng) * rms * 10 ** (-rng.uniform(22, 40) / 20)
        if rng.random() < 0.5:
            gc = fet_gain(x, rng); x, t = x * gc, t * gc
    else:
        tone = sg.lfilter([1], [1, -rng.uniform(0.0, 0.98)], rng.standard_normal(n))
        tone *= rms * 10 ** (rng.uniform(-85, -50) / 20) / (np.std(tone) + 1e-12)
        x = x + tone
    g = 10 ** (rng.uniform(-35, -12) / 20) / (np.sqrt(np.mean(x ** 2)) + 1e-12)
    x, t = x * g, t * g
    s0 = n - int(SEG * FS)
    x, t = x[s0:].astype(np.float32), t[s0:].astype(np.float32)
    X = DM.stft_np(x); Tt = DM.stft_np(t)
    F = DM.features_np(X)
    return F, X, Tt

def collate(b):
    F = torch.tensor(np.stack([e[0] for e in b])); X = torch.tensor(np.stack([e[1] for e in b])); T = torch.tensor(np.stack([e[2] for e in b]))
    return F, X, T

def aligned_loss(net, F, X, T):
    Y, _ = net(F, X)
    LA = DM.LA
    Ta = torch.cat([torch.zeros_like(T[:, :LA]), T[:, :-LA]], 1)
    return DM.loss_fn(Y[:, LA:], Ta[:, LA:])

if __name__ == "__main__":
    run = sys.argv[1]; steps = int(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else 100000
    os.makedirs(run, exist_ok=True)
    torch.set_num_threads(8)
    net = DM.DRNet()
    opt = torch.optim.AdamW(net.parameters(), lr=1e-3, weight_decay=1e-5)
    arg = lambda k, d: type(d)(sys.argv[sys.argv.index(k) + 1]) if k in sys.argv else d
    sing, peak, dense, setup = arg("--sing", 0.0), arg("--lr", 1e-3), arg("--dense", 0.0), arg("--setup", 0.0)
    if "--init" in sys.argv:
        sd = torch.load(arg("--init", ""), map_location="cpu"); net.load_state_dict(sd["net"] if "net" in sd else sd)
    step = 0
    ck = f"{run}/last.pt"
    if "--resume" in sys.argv and os.path.exists(ck):
        s = torch.load(ck); net.load_state_dict(s["net"]); opt.load_state_dict(s["opt"]); step = s["step"]
    print(f"{sum(p.numel() for p in net.parameters()) / 1e6:.2f} M parameters, {len(voices())} training voices, "
          f"{len(singers()) if sing > 0 else 0} singers ({sing:.0%} of examples), dense rap-like takes {dense:.0%} of speech, "
          f"the artist's setup {setup:.0%} of examples", flush=True)
    dl = torch.utils.data.DataLoader(Gen(sing=sing, dense=dense, setup=setup), batch_size=16, num_workers=6, collate_fn=collate, prefetch_factor=4)
    sched = lambda s: peak * min(1.0, s / 1000) * (0.5 ** (s / 40000)) if "--init" not in sys.argv else \
        peak * min(1.0, s / 300) * (0.5 ** (s / 3000))
    t0 = time.time(); acc = 0.0; k = 0
    for F, X, T in dl:
        for g_ in opt.param_groups:
            g_["lr"] = sched(step)
        loss = aligned_loss(net, F, X, T)
        opt.zero_grad(); loss.backward()
        torch.nn.utils.clip_grad_norm_(net.parameters(), 1.0)
        opt.step(); step += 1; acc += loss.item(); k += 1
        if step % 100 == 0:
            print(f"step {step} loss {acc / k:.4f} lr {sched(step):.2e} {(time.time() - t0) / k:.2f} s/step", flush=True)
            acc = 0.0; k = 0; t0 = time.time()
        if step % 1000 == 0:
            torch.save(dict(net=net.state_dict(), opt=opt.state_dict(), step=step), ck)
            torch.save(net.state_dict(), f"{run}/net_{step}.pt")
        if step >= steps:
            break
