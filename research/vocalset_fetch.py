"""Fetch a subset of VocalSet (Wilkins, Seetharaman, Wahl, Pardo 2018; Zenodo 10.5281/zenodo.1193957, CC BY 4.0;
20 professional singers, 44.1 kHz) without downloading the 2 GB archive: the zip's central directory is read with HTTP
range requests (vctk_fetch.RangeFile), then each chosen recording is fetched in one request (CRC-checked).
Chosen: song excerpts (straight, vibrato, spoken), straight and forte long tones, belted arpeggios, breathy scales.
Out: ~/axell-dereverb-data/vocalset/<singer>/<technique>_<file>.wav
usage: python3 vocalset_fetch.py"""
import os, sys, time, struct, zlib, zipfile, urllib.request, urllib.error
sys.argv = sys.argv[:1]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vctk_fetch import RangeFile

URL = "https://zenodo.org/api/records/1193957/files/VocalSet.zip/content"
OUT = os.path.expanduser(os.environ.get("VOCALSET_OUT", "~/axell-dereverb-data/vocalset"))
KEEP = ("/excerpts/", "/long_tones/straight/", "/long_tones/forte/", "/arpeggios/belt/", "/scales/breathy/")

def get(a, b):
    """bytes a..b (inclusive) in one request; waits and retries when the server asks to slow down (HTTP 429)"""
    for attempt in range(12):
        try:
            req = urllib.request.Request(URL, headers={"Range": f"bytes={a}-{b}"})
            with urllib.request.urlopen(req, timeout=300) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code != 429 or attempt == 11:
                raise
            time.sleep(float(e.headers.get("Retry-After") or 30) + 1)
        except OSError:
            if attempt == 11:
                raise
            time.sleep(10)

def fetch(info):
    """one member: its local header and data in a single range request, then inflated here"""
    p = info.filename.split("/")              # FULL/<singer>/<group>/<technique>/<file>.wav
    dst = f"{OUT}/{p[1]}/{p[2]}-{p[3]}-{p[4]}"
    if os.path.exists(dst) and os.path.getsize(dst) > 0:
        return
    raw = get(info.header_offset, info.header_offset + 30 + len(info.filename.encode()) + 1024 + info.compress_size)
    n, e = struct.unpack("<HH", raw[26:30])
    data = raw[30 + n + e:30 + n + e + info.compress_size]
    if info.compress_type == zipfile.ZIP_DEFLATED:
        data = zlib.decompress(data, -15)
    assert len(data) == info.file_size and zlib.crc32(data) == info.CRC, info.filename
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst + ".part", "wb") as f:
        f.write(data)
    os.replace(dst + ".part", dst)

if __name__ == "__main__":
    z = zipfile.ZipFile(RangeFile(URL))
    todo = [i for i in z.infolist() if i.filename.startswith("FULL/") and i.filename.endswith(".wav") and any(k in i.filename for k in KEEP)]
    print(len(todo), "files", flush=True)
    for k, info in enumerate(todo, 1):
        fetch(info)
        if k % 50 == 0:
            print(k, flush=True)
    print("done", OUT)
