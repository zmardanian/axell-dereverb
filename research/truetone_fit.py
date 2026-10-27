"""TRUE TONE (Axell De-Reverb 4.0): the tone-correction model, rendered and fitted (DECISIONS D-TT).

    python3 truetone_fit.py render OUT.npz yours ROOMS.npz      # artist-like rooms (make_your_rooms.py output)
    python3 truetone_fit.py render OUT.npz generic SEED0 SEED1  # the 8 synthetic small-room types, seeds [SEED0, SEED1)
    python3 truetone_fit.py fit --train A.npz B.npz --test C.npz D.npz [--header ../Source/TrueToneModel.h]

render: every example (the 3 held-out ARCTIC voices of dr_corpus in a room) goes through tools/DRRender --measure,
the shipping engine at REDUCTION 100 % with TRUE TONE measuring but not correcting. Saved per example: G (the
plugin's own band gain, exactly as Source/TrueTone.h measures it) and Welch spectra (8192 at 48 kHz) of the
direct-path target, the input and the output, plus the input-output cross-spectrum - enough to score any REDUCTION
mix and any correction offline.
fit: for each REDUCTION amount on a grid, ridge regression (lambda 10) of the correction that makes the MIXED output
match the direct sound (level-matched third octaves) from [1, G]; scored on the test sets; writes the C++ header."""
import argparse, os, subprocess, sys, tempfile, zlib
import numpy as np
from scipy import signal as sg

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
FS = 48000
FC = np.array([63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000,
               5000, 6300, 8000, 10000], float)
MIXES = [0.4, 0.55, 0.7, 0.85, 1.0]
RENDER = os.path.join(HERE, "..", "build", "DRRender")
WEIGHTS = os.path.join(HERE, "..", "Resources", "dereverb.bin")


def measure(x, tmp):
    """the engine's output and its TRUE TONE G for one mono example"""
    import soundfile as sf
    sf.write(tmp + "_in.wav", x.astype(np.float32), FS, subtype="FLOAT")
    out = subprocess.run([RENDER, WEIGHTS, tmp + "_in.wav", tmp + "_out.wav", "--measure"], check=True,
                         capture_output=True, text=True).stdout.strip().split("\n")
    y = sf.read(tmp + "_out.wav")[0]
    return (y if y.ndim == 1 else y[:, 0]), np.array([float(v) for v in out[1].split()])


def spectra(t, x, y):
    f, ptt = sg.welch(t, FS, nperseg=8192)
    _, pxx = sg.welch(x, FS, nperseg=8192)
    _, pyy = sg.welch(y, FS, nperseg=8192)
    _, pxy = sg.csd(x, y, FS, nperseg=8192)
    return f, np.stack([ptt, pxx, pyy, pxy.real]).astype(np.float32)


def render(args):
    import dr_corpus as DC, rooms as RM
    S, G, K = [], [], []
    with tempfile.TemporaryDirectory() as td:
        tmp = os.path.join(td, "ex")

        def one(t, x, kind):
            y, g = measure(x, tmp)
            f, s = spectra(t, x, y[:len(x)])
            S.append(s); G.append(g); K.append(kind)
            return f
        if args.kind == "yours":
            z = np.load(os.path.expanduser(args.a))
            for r in range(len(z["h"])):
                for spk in DC.ARCTIC:
                    dry, _ = DC.dry_take(spk, 4, 1000 + r)
                    n = len(dry)
                    rng = np.random.default_rng(zlib.crc32(f"yours/{spk}/{r}".encode()))
                    tone = sg.lfilter([1], [1, -0.9], rng.standard_normal(n))
                    tone *= 10 ** (-70 / 20) / np.std(tone)
                    f = one(sg.fftconvolve(dry, z["d"][r])[:n], sg.fftconvolve(dry, z["h"][r])[:n] + tone, str(z["kind"][r]))
        else:
            for kind in RM.ROOM_TYPES:
                for s in range(int(args.a), int(args.b)):
                    for spk in DC.ARCTIC:
                        d = DC.example(spk, kind, s)
                        f = one(d["target"], d["input"], kind)
    np.savez(args.out, f=f, S=np.array(S), G=np.array(G), kinds=np.array(K))
    print("saved", args.out, len(S))


def band_err(f, pt, pe):
    """level-matched third-octave error of a spectrum against the target's (dB)"""
    g = pt.sum() / (pe.sum() + 1e-30)
    edges = [(c / 2 ** (1 / 6), c * 2 ** (1 / 6)) for c in FC]
    return np.array([10 * np.log10((pe[(f >= lo) & (f < hi)].sum() * g + 1e-30) / (pt[(f >= lo) & (f < hi)].sum() + 1e-30))
                     for lo, hi in edges])


def to_bins(f, c):
    return 10 ** (np.interp(np.log(np.clip(f, FC[0], FC[-1])), np.log(FC), c) / 20)


def errors(z, m, W=None):
    """band errors of the output heard at REDUCTION m, with (W) or without the correction on the processed part"""
    f, out = z["f"], []
    for s, g in zip(z["S"], z["G"]):
        gain = 1.0 if W is None else to_bins(f, np.clip(np.hstack([1.0, g]) @ W, -3, 3))
        pe = (1 - m) ** 2 * s[1] + m ** 2 * gain ** 2 * s[2] + 2 * m * (1 - m) * gain * s[3]
        out.append(band_err(f, s[0], pe))
    return np.array(out)


def fit(args):
    train = [np.load(p) for p in args.train]
    test = {os.path.basename(p): np.load(p) for p in args.test}
    G = np.vstack([z["G"] for z in train])
    X = np.hstack([np.ones((len(G), 1)), G])
    I = np.eye(X.shape[1]); I[0, 0] = 0
    rms = lambda E: np.sqrt((E ** 2).mean(1))
    Ws = []
    for m in MIXES:
        E = np.vstack([errors(z, m) for z in train])
        W = np.linalg.solve(X.T @ X + 10.0 * I, X.T @ (-E))
        Ws.append(W)
        report = []
        for name, z in test.items():
            e0, e1 = rms(errors(z, m)), rms(errors(z, m, W))
            report.append(f"{name} {e0.mean():.2f} -> {e1.mean():.2f} dB (worse >0.1 dB: {np.mean(e1 - e0 > 0.1) * 100:.0f} %)")
        print(f"REDUCTION {m:.2f}: " + " | ".join(report), flush=True)
    if args.header:
        write_header(np.array(Ws, np.float32), args.header, len(G))


def write_header(Ws, path, n):
    lit = lambda v: (lambda t: (t if ("." in t or "e" in t) else t + ".0") + "f")(f"{v:.8g}")
    blocks = ",\n".join("    {\n" + ",\n".join("        { " + ", ".join(lit(v) for v in r) + " }" for r in W) + "\n    }" for W in Ws)
    open(path, "w").write(f"""#pragma once
// Axell De-Reverb 4.0 TRUE TONE - the learned correction. Generated by research/truetone_fit.py - do not edit.
// For each REDUCTION amount in kMixes: correction_dB[b] = kW[m][0][b] + sum_k kW[m][1 + k][b] * G[k], G = the plugin's
// own band gain (its processed output vs its input per band, relative to the full-band gain) measured by the plugin
// (tools/DRRender --measure). Ridge regression on {n} examples (held-out voices in artist-like and synthetic small rooms)
// against the direct-path sound of the output heard at that amount. Capped at +-3 dB in TrueTone.h.
namespace axdr::truetone
{{
constexpr int kBands = {len(FC)};
constexpr float kCentres[kBands] = {{ {", ".join(lit(c) for c in FC)} }};
constexpr int kMixCount = {len(MIXES)};
constexpr float kMixes[kMixCount] = {{ {", ".join(lit(m) for m in MIXES)} }};
constexpr float kW[kMixCount][{Ws.shape[1]}][{Ws.shape[2]}] = {{
{blocks}
}};
}} // namespace axdr::truetone
""")
    print("wrote", path)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("render"); r.add_argument("out"); r.add_argument("kind", choices=["yours", "generic"])
    r.add_argument("a"); r.add_argument("b", nargs="?")
    f = sub.add_parser("fit"); f.add_argument("--train", nargs="+", required=True); f.add_argument("--test", nargs="+", required=True)
    f.add_argument("--header")
    a = ap.parse_args()
    render(a) if a.cmd == "render" else fit(a)
