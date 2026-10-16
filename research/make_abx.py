"""Blind listening set from the user's own take (the final sign-off): excerpts of the dry vocal, each as two files in
random order - the original and Axell De-Reverb (through the C++ engine, tools/DRRender) - plus an X file that is one
of the two (ABX). The answers go to a separate key file. The source file is only read.
usage: python make_abx.py <take.wav> <DRRender> <weights.bin> <out dir> [reduction 0..1 = 1.0]"""
import os, sys, json, wave, subprocess, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

def read(path):
    w = wave.open(path); fs = w.getframerate(); ch = w.getnchannels(); sw = w.getsampwidth()
    raw = w.readframes(w.getnframes())
    if sw == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3)
        x = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16))
        x = np.where(x >= 1 << 23, x - (1 << 24), x) / float(1 << 23)
    else:
        x = np.frombuffer(raw, np.int16) / 32768.0
    return fs, x.reshape(-1, ch).mean(1)

def write(path, x, fs):
    y = np.clip(x, -1, 1)
    q = np.round(y * (2 ** 23 - 1)).astype(np.int32)
    b = np.stack([(q & 0xFF), (q >> 8) & 0xFF, (q >> 16) & 0xFF], 1).astype(np.uint8).tobytes()
    w = wave.open(path, "wb"); w.setnchannels(1); w.setsampwidth(3); w.setframerate(fs); w.writeframes(b); w.close()

if __name__ == "__main__":
    take, tool, weights, out = sys.argv[1:5]
    red = float(sys.argv[5]) if len(sys.argv) > 5 else 1.0
    os.makedirs(out, exist_ok=True)
    fs, x = read(take)
    assert fs == 48000, "the listening set is made at 48 kHz"
    # process the whole take once (the network learns the room over time), then cut excerpts from both
    tmp_in, tmp_out = f"{out}/.in.f32", f"{out}/.out.f32"
    x.astype(np.float32).tofile(tmp_in)
    subprocess.run([tool, weights, tmp_in, tmp_out, "--raw"], check=True)
    y = np.fromfile(tmp_out, np.float32).astype(np.float64)[:len(x)]
    y = (1 - red) * x + red * y
    os.remove(tmp_in); os.remove(tmp_out)
    # excerpts: 8 s each, evenly through the take, skipping the first 4 s (the network settles on the room)
    n = int(8 * fs); starts = np.linspace(4 * fs, len(x) - n - fs, 4).astype(int)
    rng = np.random.default_rng()
    key = {}
    fade = np.minimum(1, np.minimum(np.arange(n), n - 1 - np.arange(n)) / (0.02 * fs))
    for k, s in enumerate(starts, 1):
        a, b = x[s:s + n] * fade, y[s:s + n] * fade
        # loudness-matched (RMS), so louder never sounds "better"
        b *= np.sqrt(np.mean(a ** 2) / (np.mean(b ** 2) + 1e-20))
        pair = [("original", a), ("de-reverb", b)]
        rng.shuffle(pair)
        xi = int(rng.integers(2))
        write(f"{out}/{k}a.wav", pair[0][1], fs); write(f"{out}/{k}b.wav", pair[1][1], fs); write(f"{out}/{k}x.wav", pair[xi][1], fs)
        key[str(k)] = {"a": pair[0][0], "b": pair[1][0], "x": "a" if xi == 0 else "b", "start_s": round(s / fs, 2)}
    json.dump(dict(reduction=red, excerpts=key), open(f"{out}/.answer-key.json", "w"), indent=1)
    open(f"{out}/HOW-TO-LISTEN.txt", "w").write(
        "For each number 1-4: 'a' and 'b' are the same 8 s of your vocal - one untouched, one through Axell De-Reverb, in\n"
        "random order, at the same loudness. 'x' is one of the two.\n"
        "1. Which of a / b sounds better (more like you in a dead room, nothing lost)?\n"
        "2. Is x the same as a or as b? (if you cannot tell, say so - that is an answer too)\n"
        "Write down your answers before opening .answer-key.json (a hidden file in this folder).\n")
    print("wrote", out)
