"""Fetch a balanced subset of the VCTK corpus (CSTR VCTK Corpus 0.92, University of Edinburgh, CC BY 4.0) without
downloading the 11.7 GB archive: the zip's central directory is read with HTTP range requests, then only the chosen
recordings (mic1, 48 kHz FLAC) are fetched and decoded (ffmpeg) to 16-bit WAV.
Out: ~/axell-gate-data/vctk/<speaker>/<utterance>.wav, speakers.txt (id, gender, accent)
usage: python3 vctk_fetch.py [n_female=12] [n_male=12] [utterances=40]"""
import os, sys, io, zipfile, subprocess, urllib.request, collections
URL = "https://datashare.ed.ac.uk/server/api/core/bitstreams/535f4286-e54c-4038-838c-a02285e32cb2/content"
OUT = os.path.expanduser(os.environ.get("VCTK_OUT", "~/axell-gate-data/vctk"))

class RangeFile(io.RawIOBase):
    """a read-only, seekable view of a remote file through HTTP range requests (blocks cached)"""
    BLOCK = 1 << 20
    def __init__(self, url):
        self.url, self.pos, self.cache = url, 0, collections.OrderedDict()
        req = urllib.request.Request(url, headers={"Range": "bytes=0-0"})
        with urllib.request.urlopen(req, timeout=120) as r:
            self.size = int(r.headers["Content-Range"].split("/")[1])
    def readable(self): return True
    def seekable(self): return True
    def tell(self): return self.pos
    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else self.pos + off if whence == 1 else self.size + off
        return self.pos
    def _block(self, k):
        if k not in self.cache:
            a = k * self.BLOCK; b = min(self.size, a + self.BLOCK) - 1
            for attempt in range(5):
                try:
                    req = urllib.request.Request(self.url, headers={"Range": f"bytes={a}-{b}"})
                    with urllib.request.urlopen(req, timeout=120) as r:
                        self.cache[k] = r.read(); break
                except Exception:
                    if attempt == 4: raise
            while len(self.cache) > 64:
                self.cache.popitem(last=False)
        return self.cache[k]
    def read(self, n=-1):
        if n < 0: n = self.size - self.pos
        out = bytearray()
        while n > 0 and self.pos < self.size:
            k, o = divmod(self.pos, self.BLOCK)
            chunk = self._block(k)[o:o + n]
            out += chunk; self.pos += len(chunk); n -= len(chunk)
        return bytes(out)
    def readinto(self, b):
        d = self.read(len(b)); b[:len(d)] = d; return len(d)

if __name__ == "__main__":
    nf, nm, nu = (int(a) for a in (sys.argv[1:] + ["12", "12", "40"])[:3])
    z = zipfile.ZipFile(RangeFile(URL))
    info = z.read("speaker-info.txt").decode("utf-8", "replace").splitlines()
    spk = []
    for l in info[1:]:
        p = l.split()
        if len(p) >= 4 and p[0].startswith("p"):
            spk.append((p[0], p[2], p[3]))   # id, gender, accent
    files = collections.defaultdict(list)
    for n in z.namelist():
        if n.startswith("wav48_silence_trimmed/") and n.endswith("_mic1.flac"):
            files[n.split("/")[1]].append(n)
    # balanced: alternate accents within each gender, speakers with enough recordings
    chosen = []
    for g, k in (("F", nf), ("M", nm)):
        by_acc = collections.defaultdict(list)
        for s, gg, acc in spk:
            if gg == g and len(files.get(s, [])) >= nu:
                by_acc[acc].append(s)
        order = []
        while len(order) < k and any(by_acc.values()):
            for acc in sorted(by_acc):
                if by_acc[acc] and len(order) < k:
                    order.append((by_acc[acc].pop(0), g, acc))
        chosen += order
    os.makedirs(OUT, exist_ok=True)
    with open(f"{OUT}/speakers.txt", "w") as fo:
        for s, g, acc in chosen:
            fo.write(f"{s} {g} {acc}\n")
    # every recording in one exact range request (local header + data), 6 at a time; the directory is read once
    import zlib, struct
    from concurrent.futures import ThreadPoolExecutor
    infos = {i.filename: i for i in z.infolist()}
    def fetch(a, b):
        for attempt in range(6):
            try:
                req = urllib.request.Request(URL, headers={"Range": f"bytes={a}-{b}"})
                with urllib.request.urlopen(req, timeout=180) as r:
                    return r.read()
            except Exception:
                if attempt == 5:
                    raise
    def job(item):
        s, n = item
        wav = f"{OUT}/{s}/{os.path.basename(n)[:-10]}.wav"
        if os.path.exists(wav):
            return
        zi = infos[n]
        raw = fetch(zi.header_offset, zi.header_offset + 30 + 512 + zi.compress_size)
        assert raw[:4] == b"PK\x03\x04"
        nl, el = struct.unpack("<HH", raw[26:30])
        body = raw[30 + nl + el:30 + nl + el + zi.compress_size]
        data = body if zi.compress_type == zipfile.ZIP_STORED else zlib.decompress(body, -15)
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "flac", "-i", "pipe:0", "-ac", "1", "-c:a", "pcm_s16le", wav + ".tmp.wav"], input=data, check=True)
        os.replace(wav + ".tmp.wav", wav)
    items = []
    for s, g, acc in chosen:
        os.makedirs(f"{OUT}/{s}", exist_ok=True)
        items += [(s, n) for n in sorted(files[s])[:nu]]
    with ThreadPoolExecutor(6) as ex:
        list(ex.map(job, items))
    for s, g, acc in chosen:
        print(f"{s} ({g}, {acc}): {len([f for f in os.listdir(f'{OUT}/{s}') if f.endswith('.wav') and not f.endswith('.tmp.wav')])} files", flush=True)
