"""The artist's recording setup, measured blind from dry takes (no labels): untreated room -> LCT 440 PURE -> Volt 176.
- room: per octave band, the free decay after word endings (a sharp offset followed by >= 150 ms of pause): the
  slope of the band's level (dB/s) between -5 and -25 dB re the ending, robust median -> RT60 = 60 / slope;
- noise floor: the level and 1/3-octave spectrum of the quietest live 50 ms frames (digital silence excluded);
- compression: crest factor and the distribution of 10 ms peaks re the loud level.
usage: python3 measure_setup.py <take.wav>...  -> ~/axell-dereverb-data/your-setup/setup.json (the first take's noise
floor is the one used for training: pick a take with real pauses)"""
import sys, os, json, numpy as np, soundfile as sf
from scipy import signal as sg

BANDS = [125, 250, 500, 1000, 2000, 4000, 8000]

def measure(path):
    x, fs = sf.read(path); x = x.mean(1) if x.ndim > 1 else x
    return dict(file=os.path.basename(path), **measure_array(x, fs))

def measure_array(x, fs):
    hop = int(0.002 * fs)
    def env(y):
        n = len(y) // hop
        e = np.mean(y[:n * hop].reshape(n, hop) ** 2, 1)
        return 10 * np.log10(sg.lfilter([0.2], [1, -0.8], e) + 1e-14)   # ~10 ms smoothing
    full = env(x)
    live = full > -110
    floor = np.percentile(full[live], 2); loud = np.percentile(full[live], 95)
    # offsets: from >= floor+25 dB to near the floor, then a pause of >= 150 ms
    offs = []
    i = 0
    while i < len(full) - 100:
        if full[i] > floor + 25 and full[i + 1] <= full[i] and np.all(full[i + 15:i + 75] < full[i] - 10):
            offs.append(i); i += 100
        else:
            i += 1
    rt = {}
    for f in BANDS:
        sos = sg.butter(3, [f / np.sqrt(2), min(f * np.sqrt(2), 0.45 * fs)], "bandpass", fs=fs, output="sos")
        e = env(sg.sosfiltfilt(sos, x))
        bfloor = np.percentile(e[live], 2)
        slopes = []
        for o in offs:
            seg = e[o:o + 150]; top = seg[0]
            if top - bfloor < 25:
                continue
            a = np.argmax(seg < top - 5); b = np.argmax(seg < top - 25)
            if a == 0 or b <= a + 3:
                continue
            t = np.arange(a, b) * hop / fs
            k = np.polyfit(t, seg[a:b], 1)[0]
            if k < 0:
                slopes.append(-k)
        if len(slopes) >= 4:
            rt[f] = dict(rt60=round(60.0 / float(np.median(slopes)), 3), n=len(slopes))
    # noise floor spectrum from the quietest live 50 ms frames
    fr = int(0.05 * fs); n = len(x) // fr
    L = 10 * np.log10(np.mean(x[:n * fr].reshape(n, fr) ** 2, 1) + 1e-14)
    q = np.where((L > -110) & (L < np.percentile(L[L > -110], 8)))[0]
    spec = np.zeros(fr // 2 + 1)
    for j in q:
        spec += np.abs(np.fft.rfft(x[j * fr:(j + 1) * fr] * np.hanning(fr))) ** 2
    spec /= max(1, len(q)); fq = np.fft.rfftfreq(fr, 1 / fs)
    third = [63 * 2 ** (k / 3) for k in range(25)]
    nspec = {round(c): round(float(10 * np.log10(spec[(fq >= c / 2 ** (1 / 6)) & (fq < c * 2 ** (1 / 6))].mean() + 1e-20)), 1) for c in third if c < 0.45 * fs}
    # voice level: active 400 ms blocks
    blk = int(0.4 * fs); nb = len(x) // blk
    B = 10 * np.log10(np.mean(x[:nb * blk].reshape(nb, blk) ** 2, 1) + 1e-14)
    voice = float(np.mean(B[B > np.percentile(B, 30)]))
    pk10 = [20 * np.log10(np.abs(x[j:j + int(0.01 * fs)]).max() + 1e-9) for j in range(0, len(x) - int(0.01 * fs), int(0.01 * fs))]
    return dict(fs=fs, offsets=len(offs), rt60=rt,
                noise_dbfs=round(float(np.mean(L[q])), 1), voice_dbfs=round(voice, 1), snr_db=round(voice - float(np.mean(L[q])), 1),
                noise_third_octave=nspec,
                crest_db=round(float(20 * np.log10(np.abs(x).max()) - 10 * np.log10(np.mean(x[:len(live) * hop][live.repeat(hop)] ** 2) + 1e-14)), 1),
                peak10_p99_minus_p50=round(float(np.percentile(pk10, 99) - np.percentile(pk10, 50)), 1))

if __name__ == "__main__":
    res = [measure(p) for p in sys.argv[1:]]
    for r in res:
        print(r["file"], "| offsets", r["offsets"], "| RT60", {k: v["rt60"] for k, v in r["rt60"].items()},
              "| noise", r["noise_dbfs"], "dBFS, SNR", r["snr_db"], "dB | crest", r["crest_db"], "dB | 10 ms peaks p99-p50", r["peak10_p99_minus_p50"], "dB")
    # the room across takes (sessions differ: mic position, furniture): per band, the range over the takes that measured it
    rng_ = {}
    for b in BANDS:
        v = [r["rt60"][b]["rt60"] for r in res if b in r["rt60"]]
        if len(v) >= 2:
            rng_[b] = [round(min(v), 3), round(max(v), 3)]
    snr = [r["snr_db"] for r in res]
    summary = dict(room_range=rng_, snr_range=[min(snr), max(snr)], noise_from=res[0]["file"], takes=[r["file"] for r in res])
    print("room range per band:", rng_, "| SNR", summary["snr_range"])
    out = os.path.expanduser("~/axell-dereverb-data/your-setup/setup.json")
    json.dump(dict(summary=summary, takes=res), open(out, "w"), indent=1); print("wrote", out)
