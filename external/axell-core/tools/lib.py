"""Independent audit helpers (from Axell Penta EQ's audit): drive a built .vst3 through the Axell audit host (JUCE's VST3
hosting code) - nothing here uses the plugin's own code. Environment: AXELL_AUDIT_HOST (host binary), AXELL_VST3
(plugin bundle), AXELL_AUDIT_WORK (scratch dir); the DUO_* names of Axell Duo Compressor are accepted too."""
import os, subprocess, re, tempfile, numpy as np
from scipy import signal

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
def _env(name, default):
    return os.environ.get("AXELL_" + name, os.environ.get("DUO_" + name, default))
HOST = _env("AUDIT_HOST", os.path.join(ROOT, "build/DuoAuditHost_artefacts/Release/DuoAuditHost"))
PLUGIN = _env("VST3", os.path.join(ROOT, "build/AxellDuoCompressor_artefacts/Release/VST3/Axell Duo Compressor.vst3"))
WORK = _env("AUDIT_WORK", os.path.join(tempfile.gettempdir(), "axell-audit"))
os.makedirs(WORK, exist_ok=True)

def raw_write(path, x, fs):
    x = np.asarray(x, dtype=np.float32)
    if x.ndim == 1:
        x = x[:, None]
    x.tofile(path)

def raw_read(path, ch):
    return np.fromfile(path, dtype=np.float32).astype(np.float64).reshape(-1, ch)

class Run:
    def __init__(self, name, fs=48000, ch=2, sets=None, ats=None, blocks="512", nonrt=True,
                 preroll=None, sleep=0, pdc=0):
        self.name, self.fs, self.ch = name, fs, ch
        self.sets = dict(sets or {})
        self.ats = list(ats or [])
        self.blocks, self.nonrt, self.sleep, self.pdc = blocks, nonrt, sleep, pdc
        self.preroll = preroll if preroll is not None else int(0.5 * fs)
        self.inp = os.path.join(WORK, name + "_in.f32")
        self.out = os.path.join(WORK, name + "_out.f32")
        self.sc = None
        self.latency = None
        self.log = ""

    def job(self):
        L = [f"rate {self.fs}", f"channels {self.ch}", f"blocks {self.blocks}",
             f"nonrealtime {1 if self.nonrt else 0}", f"preroll {self.preroll}", f"sleep {self.sleep}", f"pdc {self.pdc}"]
        L += [f"set {k}={v}" for k, v in self.sets.items()]
        L += [f"setn {k}={v}" for k, v in getattr(self, "setns", {}).items()]
        L += [f"at {a} set {k}={v}" if not str(k).startswith("#n#") else f"at {a} setn {k[3:]}={v}" for a, k, v in self.ats]
        L += [f"input {self.inp}", f"output {self.out}"]
        if self.sc:
            L.append(f"sidechain {self.sc}")
        L.append("run")
        return "\n".join(L)

def run_all(runs, inputs, sidechains=None, jobname="job", plugin=None, subplugin=None, timeout=None):
    """inputs: dict name -> array (n,ch) ; writes, runs host once, returns dict name -> output array"""
    for r in runs:
        raw_write(r.inp, inputs[r.name], r.fs)
        if sidechains and r.name in sidechains:
            r.sc = os.path.join(WORK, r.name + "_sc.f32")
            raw_write(r.sc, sidechains[r.name], r.fs)
    jp = os.path.join(WORK, jobname + ".txt")
    with open(jp, "w") as f:
        f.write("\n".join(r.job() for r in runs) + "\n")
    p = subprocess.run([HOST, plugin or PLUGIN, jp] + (["--plugin", subplugin] if subplugin else []), capture_output=True, text=True, timeout=timeout)
    if p.returncode != 0:
        raise RuntimeError(p.stdout[-3000:] + p.stderr[-3000:])
    blocks = p.stdout.split("\nrun ")
    lat = re.findall(r"  latency (-?\d+)", p.stdout)
    cpus = re.findall(r"  cpu ([0-9.]+)", p.stdout)
    assert len(lat) == len(runs), p.stdout[-2000:]
    out = {}
    for r, l, b in zip(runs, lat, blocks[1:]):
        r.latency = int(l)
        r.cpu = float(cpus[len(out)]) if len(cpus) > len(out) else None
        r.log = b
        out[r.name] = raw_read(r.out, r.ch)
        for path in (r.inp, r.out, r.sc):  # long impulse responses add up to gigabytes: keep nothing
            if path and os.path.exists(path):
                os.remove(path)
    os.remove(jp)
    return out

