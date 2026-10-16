"""Export a trained DRNet checkpoint to the flat weight file the C++ side loads (Source/DRNet.h, Weights::load), and
(with --ref <dir>) a parity reference for the C++ test suite: a fixed 3 s test signal and the PyTorch output on it.
usage: python export_dr.py <checkpoint.pt> <out.bin> [--ref tests/reference]"""
import sys, struct, numpy as np, torch
import dr_model as DM

def main(ck, out):
    sd = torch.load(ck, map_location="cpu"); sd = sd["net"] if "net" in sd else sd
    order = ["inp.weight", "inp.bias",
             "gru1.weight_ih_l0", "gru1.weight_hh_l0", "gru1.bias_ih_l0", "gru1.bias_hh_l0",
             "gru2.weight_ih_l0", "gru2.weight_hh_l0", "gru2.bias_ih_l0", "gru2.bias_hh_l0",
             "out_g.weight", "out_g.bias", "out_df.weight", "out_df.bias", "out_a.weight", "out_a.bias"]
    with open(out, "wb") as f:
        f.write(struct.pack("<ii", 0x52445841, 1))
        for k in order:
            t = sd[k].float().numpy()
            if t.ndim == 1:
                t = t[None, :]
            f.write(struct.pack("<ii", t.shape[0], t.shape[1]))
            f.write(np.ascontiguousarray(t, "<f4").tobytes())
    print("wrote", out)

def reference(ck, d):
    import os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import dr_baselines as B
    os.makedirs(d, exist_ok=True)
    rng = np.random.default_rng(7)
    n = 3 * DM.FS
    t = np.arange(n) / DM.FS
    # a voice-like test: harmonic bursts over noise, ringing in a simple exponential room
    x = np.zeros(n)
    for k, f0 in enumerate((140, 180, 120, 160)):
        a = int((0.1 + 0.7 * k) * DM.FS); b = a + int(0.5 * DM.FS)
        ph = 2 * np.pi * np.cumsum(np.full(b - a, f0)) / DM.FS
        x[a:b] = sum(np.sin(h * ph) / h for h in range(1, 20)) * np.hanning(b - a)
    h = rng.standard_normal(int(0.3 * DM.FS)) * np.exp(-np.arange(int(0.3 * DM.FS)) / (0.06 * DM.FS)) * 0.05; h[0] = 1.0
    x = np.convolve(x, h)[:n] * 0.1 + 1e-4 * rng.standard_normal(n)
    # digital silence: before the first sound and a gated gap (the engine's silence rules are part of the parity)
    x[:int(0.35 * DM.FS)] = 0.0; x[int(1.95 * DM.FS):int(2.25 * DM.FS)] = 0.0
    x = x.astype(np.float32).astype(np.float64)
    y = B.axell_net(x, ckpt=ck)
    x.astype("<f4").tofile(f"{d}/parity_in.f32"); y.astype("<f4").tofile(f"{d}/parity_out.f32")
    print("wrote", d)

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
    if "--ref" in sys.argv:
        reference(sys.argv[1], sys.argv[sys.argv.index("--ref") + 1])
