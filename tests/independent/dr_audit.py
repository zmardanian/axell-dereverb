"""Independent black-box audit of the built Axell De-Reverb VST3 through a real VST3 host (DrAuditHost, JUCE's VST3
hosting code), with test signals and analysis written here from textbook definitions (numpy / scipy) - nothing uses
the plugin's own code. Exit code = number of failures. Environment: AXELL_AUDIT_HOST, AXELL_VST3, AXELL_AUDIT_WORK.
--quality adds the checks that need the trained network (reverberant decay shortened, a dry voice left alone)."""
import os, sys, time
import numpy as np
from scipy import signal as sg

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
os.environ.setdefault("AXELL_AUDIT_HOST", os.path.join(ROOT, "build/DrAuditHost_artefacts/Release/DrAuditHost"))
os.environ.setdefault("AXELL_VST3", os.path.join(ROOT, "build/AxellDeReverb_artefacts/Release/VST3/Axell De-Reverb.vst3"))
os.environ.setdefault("AXELL_AUDIT_WORK", os.path.expanduser("~/.cache/axell-dereverb-audit"))
_vendored = os.path.join(ROOT, "external", "axell-core")
CORE = os.environ.get("AXELL_CORE", _vendored if os.path.isdir(_vendored) else os.path.join(os.path.dirname(ROOT), "axell-core"))
sys.path.insert(0, os.path.join(CORE, "tools"))
import lib  # noqa: E402
from lib import Run  # noqa: E402

def run_all(runs, inputs, **kw):
    return lib.run_all(runs, {k: np.asarray(v, np.float32) for k, v in inputs.items()}, **kw)

results = []
def report(name, ok, detail):
    results.append((name, ok, detail))
    print(f"{'PASS' if ok else 'FAIL'}  {name}: {detail}", flush=True)

def voice(fs, secs, seed=21):
    """an independent speech-like signal: a glottal pulse train (f0 100-180 Hz) through three formant resonators, with
    syllables and fricative bursts, phrases of 1.4 s and 0.9 s pauses (stereo, identical channels)"""
    rng = np.random.default_rng(seed)
    n = int(secs * fs); t = np.arange(n) / fs
    f0 = 140 + 40 * np.sin(2 * np.pi * 0.31 * t)
    pulses = (np.diff(np.floor(np.cumsum(f0 / fs)), prepend=0) > 0).astype(float)
    src = sg.lfilter([1], [1, -0.96], pulses)
    vow = np.zeros(n)
    for fc, bw, g in ((600, 110, 1.0), (1100, 140, 0.5), (2500, 220, 0.2)):
        r = np.exp(-np.pi * bw / fs); w = 2 * np.pi * fc / fs
        vow += g * sg.lfilter([1 - r], [1, -2 * r * np.cos(w), r * r], src)
    fric = sg.lfilter(*sg.butter(2, [3000, min(9000, 0.45 * fs)], "bandpass", fs=fs), rng.standard_normal(n))
    ph = np.mod(t, 2.3); active = (ph > 0.3) & (ph < 1.7); syl = np.mod(ph, 0.35)
    venv = np.clip(np.minimum(syl, 0.25 - syl) / 0.02, 0, 1) * (syl < 0.25)
    fenv = np.sin(np.pi * np.clip((syl - 0.25) / 0.1, 0, 1)) * (syl >= 0.25)
    x = (vow / np.std(vow) * venv + 0.6 * fric / np.std(fric) * fenv) * active
    x *= 10 ** (-18 / 20) / np.sqrt(np.mean(x[active] ** 2))
    x = x.astype(np.float32).astype(np.float64)        # exactly what the host passes (float32)
    return np.stack([x, x], 1), active

def room(fs, rt60, seed=3, drr_db=6.0):
    """an independent exponential-decay room response: direct impulse + Gaussian noise decaying 60 dB in rt60"""
    rng = np.random.default_rng(seed); n = int(1.2 * rt60 * fs)
    h = rng.standard_normal(n) * np.exp(-6.91 * np.arange(n) / (rt60 * fs))
    h[:int(0.002 * fs)] = 0
    h *= 10 ** (-drr_db / 20) / np.sqrt(np.sum(h ** 2)); h[0] = 1.0
    return h

def aligned(y, L, n):
    return y[L:L + n]

START = time.time()
# ---------------------------------------------------------------- 1. latency (measured), silence, transparency, bypass
for fs in (44100, 48000, 96000):
    imp = np.zeros((fs * 2, 2)); imp[fs // 2, :] = 0.5
    x, _ = voice(fs, 6.0)
    runs = [Run(f"imp{fs}", fs, sets={"Reduction": "0"}), Run(f"sil{fs}", fs), Run(f"r0{fs}", fs, sets={"Reduction": "0"}),
            Run(f"byp{fs}", fs, sets={"Bypass": "On"})]
    out = run_all(runs, {f"imp{fs}": imp, f"sil{fs}": np.zeros((fs, 2)), f"r0{fs}": x, f"byp{fs}": x}, jobname=f"base{fs}")
    y = out[f"imp{fs}"][:, 0]; pk = int(np.argmax(np.abs(y)))
    report(f"@ {fs}: measured latency (impulse peak) = reported latency", pk - fs // 2 == runs[0].latency, f"measured {pk - fs // 2}, reported {runs[0].latency}")
    report(f"@ {fs}: silence in -> exactly silence out", np.abs(out[f"sil{fs}"]).max() == 0.0, f"max |y| {np.abs(out[f'sil{fs}']).max():.3g}")
    y0 = aligned(out[f"r0{fs}"], runs[2].latency, len(x)); m = len(y0) - fs // 4
    e = 10 * np.log10(np.sum((y0[fs // 2:m] - x[fs // 2:m]) ** 2) / np.sum(x[fs // 2:m] ** 2) + 1e-30)
    report(f"@ {fs}: REDUCTION 0 = the input delayed by the reported latency (full band)", e < -100, f"{e:.1f} dB")
    yb = aligned(out[f"byp{fs}"], runs[3].latency, len(x))
    report(f"@ {fs}: host bypass = exactly the input, delayed by the reported latency", np.abs(yb[fs // 2:] - x[fs // 2:len(yb)]).max() == 0.0,
           f"max error {np.abs(yb[fs // 2:] - x[fs // 2:len(yb)]).max():.2g}")

# ---------------------------------------------------------------- 2. block sizes; LISTEN identity; presets
fs = 48000
x, act = voice(fs, 8.0)
xr = np.stack([sg.fftconvolve(x[:, 0], room(fs, 0.6))[:len(x)]] * 2, 1).astype(np.float32).astype(np.float64)
runs = [Run("b512", fs), Run("brand", fs, blocks="random 1 4096 7"), Run("lis", fs, sets={"Listen: Removed": "On"})]
out = run_all(runs, {r.name: xr for r in runs}, jobname="blocks")
d = np.abs(aligned(out["brand"], runs[1].latency, len(xr)) - aligned(out["b512"], runs[0].latency, len(xr))).max()
report("random host block sizes 1-4096 = 512-sample blocks", d < 1e-5, f"max diff {d:.2g}")
s = aligned(out["b512"], runs[0].latency, len(xr)) + aligned(out["lis"], runs[2].latency, len(xr))
e = 10 * np.log10(np.sum((s[fs // 2:] - xr[fs // 2:len(s)]) ** 2) / np.sum(xr[fs // 2:len(s)] ** 2) + 1e-30)
report("LISTEN: processed + removed = the input", e < -100, f"{e:.1f} dB")
names = ["Natural", "Clean", "Gentle"]
runs = [Run(f"p{i}", fs, sets={"Program": nm}) for i, nm in enumerate(names)]
out = run_all(runs, {r.name: xr for r in runs}, jobname="presets")
bad = [nm for r, nm in zip(runs, names) if not np.isfinite(out[r.name]).all() or np.abs(out[r.name]).max() > 4 * np.abs(xr).max()]
report("all factory presets: finite and bounded", not bad, ", ".join(bad) if bad else "ok")

# ---------------------------------------------------------------- 3. quality (needs the trained network)
if "--quality" in sys.argv:
    def tail_db(y, ends, fs):
        """the room after each phrase: energy 80-400 ms after the phrase end re the last second of the phrase (dB)"""
        r = [np.mean(y[int((t + 0.08) * fs):int((t + 0.4) * fs)] ** 2) / np.mean(y[int((t - 1.0) * fs):int(t * fs)] ** 2) for t in ends]
        return float(np.median(10 * np.log10(np.array(r) + 1e-30)))
    def octave_change(y, x, fs):
        """the long-term spectrum change, output re input, in octave bands 125 Hz - 8 kHz (dB): the largest one"""
        f, py = sg.welch(y, fs, nperseg=8192); _, px = sg.welch(x, fs, nperseg=8192)
        ch = [10 * np.log10(py[(f >= fc / 2 ** 0.5) & (f < fc * 2 ** 0.5)].sum() / px[(f >= fc / 2 ** 0.5) & (f < fc * 2 ** 0.5)].sum()) for fc in 125 * 2.0 ** np.arange(7)]
        return max(ch, key=abs)
    def si_sdr(y, x):
        a = np.dot(y, x) / np.dot(x, x); return 10 * np.log10(np.sum((a * x) ** 2) / np.sum((y - a * x) ** 2))
    ARC = os.path.expanduser(os.environ.get("AXELL_ARCTIC", "~/axell-velvet-data/arctic"))
    def arctic(spk, files):
        """CMU ARCTIC recordings (free for any use; never trained on), 32 -> 48 kHz; channel 0 is the speech"""
        import wave
        d = f"{ARC}/cmu_us_{spk}_arctic/orig"
        out = []
        for f in sorted(os.listdir(d))[files]:
            w = wave.open(f"{d}/{f}")
            v = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, w.getnchannels())[:, 0] / 32768.0
            out.append(sg.sosfiltfilt(sg.butter(2, 60, "high", fs=fs, output="sos"), sg.resample_poly(v, 3, 2)))
        return out
    # room removal: phrases of real speech with 0.8 s pauses; the room after each phrase must drop by 6 dB. The
    # synthetic voice (a steady buzz with a gliding pitch: to the network it looks like a held sung note, which it
    # treats with care) is reported for information only.
    ends = [1.7 + 2.3 * k for k in range(3)]
    t_dry = tail_db(x[:, 0], ends, fs)
    for rt in (0.3, 0.6):
        h = room(fs, rt, seed=int(rt * 10))
        xr = np.stack([sg.fftconvolve(x[:, 0], h)[:len(x)]] * 2, 1).astype(np.float32).astype(np.float64)
        runs = [Run(f"q{int(rt * 10)}", fs)]
        out = run_all(runs, {runs[0].name: xr}, jobname=f"q{rt}")
        y = aligned(out[runs[0].name], runs[0].latency, len(xr))[:, 0]
        print(f"info  RT60 {rt} s room, synthetic voice: tail re phrase {tail_db(xr[:, 0], ends, fs):.1f} -> {tail_db(y, ends, fs):.1f} dB "
              f"(dry voice {t_dry:.1f} dB)", flush=True)
        if not os.path.isdir(ARC):
            print("note: CMU ARCTIC not found (AXELL_ARCTIC) - real-voice room checks skipped")
            continue
        ins, ends_by = {}, {}
        for spk in ("slt", "bdl", "jmk"):
            parts, e, pos = [np.zeros(int(0.5 * fs))], [], int(0.5 * fs)
            for v in arctic(spk, slice(200, 208)):
                act = np.flatnonzero(np.abs(v) > 0.02 * np.abs(v).max()); v = v[act[0]:act[-1]]
                v = v * np.r_[np.linspace(0, 1, 240), np.ones(len(v) - 480), np.linspace(1, 0, 240)]
                parts += [v, np.zeros(int(0.8 * fs))]; pos += len(v); e.append(pos / fs); pos += int(0.8 * fs)
            dv = np.concatenate(parts); dv *= 0.05 / np.std(dv)
            ins[spk] = np.stack([sg.fftconvolve(dv, h)[:len(dv)]] * 2, 1).astype(np.float32).astype(np.float64)
            ends_by[spk] = e[:-1]
        runs = [Run(f"r{int(rt * 10)}{s}", fs) for s in ins]
        out = run_all(runs, {r.name: v for r, v in zip(runs, ins.values())}, jobname=f"real{rt}")
        t_in = np.mean([tail_db(v[:, 0], ends_by[s], fs) for s, v in ins.items()])
        t_out = np.mean([tail_db(aligned(out[r.name], r.latency, len(v))[:, 0], ends_by[s], fs) for r, (s, v) in zip(runs, ins.items())])
        report(f"RT60 {rt} s room, 3 real voices: the room after each phrase is at least 6 dB lower (default REDUCTION)",
               t_out < t_in - 6, f"tail re phrase: input {t_in:.1f} dB -> output {t_out:.1f} dB")
    # dry voices (no room) must come out essentially as they went in: the synthetic voice, and real voices when the
    # CMU ARCTIC recordings are present (free for any use; never trained on). Limits: no octave band moves by more
    # than 3 dB, and SI-SDR > 10 dB (a broad 2 dB tilt alone already gives about 15 dB)
    dry = {"synthetic": x}
    for spk in ("slt", "bdl", "jmk"):
        d = f"{ARC}/cmu_us_{spk}_arctic/orig"
        if not os.path.isdir(d):
            continue
        import wave
        parts = []
        for f in sorted(os.listdir(d))[100:112]:
            w = wave.open(f"{d}/{f}")
            v = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, w.getnchannels())[:, 0] / 32768.0
            parts.append(sg.resample_poly(v, 3, 2))  # 32 -> 48 kHz; channel 0 is the speech (1 is the EGG)
        v = np.concatenate(parts); v = sg.sosfiltfilt(sg.butter(2, 60, "high", fs=fs, output="sos"), v)
        v *= 10 ** (-20 / 20) / np.sqrt(np.mean(v ** 2))
        v = v.astype(np.float32).astype(np.float64)
        dry[f"ARCTIC {spk}"] = np.stack([v, v], 1)
    runs = [Run(f"dry{i}", fs) for i in range(len(dry))]
    out = run_all(runs, {r.name: v for r, v in zip(runs, dry.values())}, jobname="dry")
    for r, (nm, v) in zip(runs, dry.items()):
        y = aligned(out[r.name], r.latency, len(v))[:, 0]; xv = v[:len(y), 0]
        sk = slice(fs, len(y))                     # after the first second (the network's running statistics settle)
        sd, oc = si_sdr(y[sk], xv[sk]), octave_change(y[sk], xv[sk], fs)
        report(f"dry voice ({nm}, no room): left essentially untouched", sd > 10 and abs(oc) <= 3,
               f"SI-SDR {sd:.1f} dB, largest octave-band change {oc:+.1f} dB")
    if len(dry) == 1:
        print("note: CMU ARCTIC not found (AXELL_ARCTIC) - real-voice dry checks skipped")

fails = sum(1 for _, ok, _ in results if not ok)
print(f"\n{'ALL PASS' if fails == 0 else 'FAILURES'}: {len(results) - fails} of {len(results)} independent checks passed ({time.time() - START:.0f} s)")
sys.exit(fails)
