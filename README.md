# Axell De-Reverb (VST3, Linux)

[![CI](https://github.com/zmardanian/axell-dereverb/actions/workflows/ci.yml/badge.svg)](https://github.com/zmardanian/axell-dereverb/actions/workflows/ci.yml)

The de-reverb of the Axell suite. It removes the room from a close vocal:
- **the tail** of a bare bedroom or a bathroom;
- **the early reflections and the 350-600 Hz box** of a wardrobe booth, a closet or a car, which most de-reverbs
  leave alone because they were built for long tails in big rooms.

> **Part of Axell's Dojo** - the Axell Signature suite for rap vocals, beats and masters on Linux:
> [Penta EQ](https://github.com/zmardanian/axell-penta-eq) · [Duo Compressor](https://github.com/zmardanian/axell-duo-compressor) ·
> [Gate](https://github.com/zmardanian/axell-gate) · [De-Reverb](https://github.com/zmardanian/axell-dereverb) ·
> [Velvet](https://github.com/zmardanian/axell-velvet) · [Master](https://github.com/zmardanian/axell-master), installed,
> updated and tuned to your voice by the [Axell Signature Dojo](https://github.com/zmardanian/axell-signature-dojo).
> Since 3.0 the plugins share one core library (`external/axell-core`): a breakthrough in one plugin becomes a
> feature of the next.

![Axell De-Reverb](docs/screenshots/dereverb.png)

## What's new in 3.0

- **LIVE mode: zero latency, for recording.** STUDIO looks 32 ms ahead (53 ms latency) - for mixing. LIVE runs a
  causal version of the network (fine-tuned from 2.0's) as a minimum-phase filter that changes every 10.7 ms (Axell
  Velvet 3.0's method, now in the shared core), with an onset guard that keeps word starts intact (a room only decays,
  so a sudden rise is the voice). On 48 held-out examples: 3.7 dB of room removed (STUDIO 5.7), ViSQOL 4.00 (input
  3.96, STUDIO 4.13), no more damage to the voice than STUDIO - with no delay. The Axell Signature Dojo's tracking
  chain uses it.
- **Axell's Dojo look**: the family emblem, a brush-written title with De-Reverb's seal (静, "stillness"), a deep-blue
  ground.

More screens: [LISTEN: REMOVED](docs/screenshots/dereverb-listen.png), [REDUCTION 75 %](docs/screenshots/dereverb-75pct.png).

## What it does

- **A trained network** runs on your CPU (no internet, nothing to install). It predicts, for every moment and
  frequency, how much of the sound is your voice and how much is the room. Below 4.5 kHz it also undoes the room with
  a short complex filter over several frames, so it can remove reflections that arrive within a few milliseconds of
  the voice.
  - **Training:** rap and spoken word first (2.0: dense takes with 0-120 ms between phrases). 80 speaking voices
    (women and men, 11 accents) and 16 singers, in 4,000 simulated small rooms (booths, closets, cars, bedrooms,
    bathrooms) and 7 measured rooms.
  - **10 % of the training had no room at all:** a dry take must come out as it went in.
- **REDUCTION** 0-100 %. The default 85 % was the best balance on the benchmark (DECISIONS D9). At 0 % the output is
  the input, delayed.
- **LISTEN: REMOVED** plays only what is taken away (processed + removed = the input).
- **OUTPUT** -24 to +24 dB. In/out meters, a REMOVED meter and a room history.
- **Presets:** Natural (85 %), Clean (100 %), Gentle (60 %). User presets and A/B.
- **STUDIO / LIVE** (3.0): STUDIO looks 32 ms ahead (latency 2560 samples at 48 kHz, 53 ms, reported and
  compensated) - for mixing. LIVE has no latency at all (48 kHz; the resamplers' 2-3 ms at other rates): a causal
  network applied as a minimum-phase filter, for hearing yourself without the room while you record. LIVE removes
  about two thirds of what STUDIO removes (3.7 vs 5.7 dB of room on 48 held-out examples, ViSQOL 4.00 vs 4.13) and
  harms the voice no more.
- **Any sample rate:** 44.1 to 96 kHz. The network runs at 48 kHz, with exact-latency resampling, and the band above
  it passes untouched.

## Measured quality

**Benchmark.** 3 voices it never heard (CMU ARCTIC), in 8 small-room types and 24 measured everyday rooms (MIT IR
Survey, never trained on). Averages over all 48 examples:

| | input | WPE (nara_wpe) | Axell 100 % | **Axell 85 % (default)** |
|---|---|---|---|---|
| ViSQOL (quality, MOS) | 3.96 | 3.99 | 4.12 | **4.13** |
| ESTOI (intelligibility) | 0.74 | 0.76 | 0.78 | **0.78** |
| SRMR (reverb, higher = drier) | 5.74 | 6.07 | 6.76 | **6.50** |
| Cepstral distance (lower = better) | 4.29 | 4.09 | 3.44 | **3.43** |
| SI-SDR vs the direct sound (dB) | -1.64 | -1.08 | -0.74 | **-0.72** |
| Coloration, 1/3-octave error (dB) | 3.33 | 3.25 | 2.63 | **2.55** |
| Room left (dB re the input) | 0 | -1.0 | -7.0 | **-5.7** |

**Dense, rap-like takes** (the same voices and rooms, 0-120 ms between phrases; `bench_dr.py --dense`), 85 %:
ViSQOL 3.94 -> 4.12, ESTOI 0.74 -> 0.78, cepstral distance 3.84 -> 2.89, SI-SDR -1.63 -> -0.74 dB, room left
-5.7 dB. 2.0 against 1.0 on these takes: better on every score, 0.2 dB more room removed.

**Per room type** (default 85 %):

| Room | ViSQOL, input -> output | Coloration error (dB) | Room left |
|---|---|---|---|
| Foam wardrobe booth | 4.10 -> 4.56 | 6.0 -> 2.0 | -10.2 dB |
| Closet | 4.44 -> 4.57 | 2.5 -> 1.4 | -7.6 dB |
| Bathroom | 3.46 -> 3.78 | 5.1 -> 1.8 | -10.0 dB |
| Car | 4.17 -> 4.26 | 3.2 -> 2.6 | -3.9 dB |
| Bare bedroom | 4.11 -> 4.23 | 0.7 -> 0.7 | -6.5 dB |
| Furnished bedroom | 4.61 -> 4.62 | 0.9 -> 0.8 | -8.3 dB |
| Living room | 4.39 -> 4.44 | 1.0 -> 0.5 | -7.2 dB |
| Treated room | 4.40 -> 4.62 | 2.8 -> 0.6 | -8.6 dB |
| Measured everyday rooms | 3.71 -> 3.87 | 3.9 -> 3.8 | -3.6 dB |

WPE, the standard method, removes 0.2-1.8 dB of room and almost nothing from booths, closets and cars.

**Singing** (4 professional singers it never heard, VocalSet: songs, vibrato, held notes, belting). A sung note looks
like a ringing room to a network that only knew speech, so it is also trained on 16 other singers (2.0: 15 % of the
training, rap and speech first):

| | input | Axell 85 % |
|---|---|---|
| Dry singing: ViSQOL | 4.65 | 4.38 |
| Dry singing: level change (songs / held notes / belting) | | -1.6 / -2.0 / -2.1 dB (about 1 dB of it is the recording room: the same singers *speaking* in the same room lose as much) |
| Singing in the small rooms: ViSQOL / room left | 4.42 / 0 | 4.25 / -5.6 dB |

Before the singing was added, dry held notes lost 11.6 dB and belting 9.6 dB.

**A real dry rap take from a bare room:**
- The room after each phrase drops by 9.9 dB at the default 85 % (-27.9 -> -37.8 dB re the phrase), by 12.8 dB at
  100 % (the Clean preset).
- Above 350 Hz the tone moves by 0.5 dB at most.
- 60-150 Hz drops by about 4 dB: that is the room's low modes, which ring on 11 dB louder than any other band after
  every phrase in this room. Dry voices with up to +10 dB of close-mic bass come through within 0.8 dB.
- In a blind A/B by the artist (1.0), the processed take was preferred in all 4 excerpts.

**Reliability:**

| | |
|---|---|
| A dry voice (no room) | left alone: SI-SDR 23-37 dB, at most 0.4 dB in any octave (3 real voices and a synthetic one) |
| Silence before a take, or a gate in front | handled: silence before a take changes nothing (-330 dB) |
| REDUCTION 0 | the input, delayed, at every rate (-292 to -335 dB) |
| LISTEN | processed + removed = the input (-139 to -154 dB) |
| Silence in | exactly silence out |
| Host bypass | exactly the input, delayed by the reported latency |
| LIVE: latency | 0 at 48 kHz; the resamplers' 2.6 ms at 44.1 kHz, 1.8 ms at 96 kHz |
| LIVE vs STUDIO (48 held-out examples, 85 %) | room left -3.7 / -5.7 dB; ViSQOL 4.00 / 4.13; SRMR 6.26 / 6.50; voice damage -11.5 / -11.3 dB |
| C++ = the trained model | -134 dB |
| CPU (one core) | STUDIO 3.5 % stereo at 48 kHz, 3.8 % at 44.1 kHz; LIVE 4.8 % at 48 kHz; a mono vocal is half |
| Real-time safety | no allocation, lock or I/O in the audio path (allocation counter in the test suite) |

All of these come from:
- the research benchmarks in `research/` (`bench_dr.py`, `bench_sing.py`);
- the automated suites in `tests/`: 45 DSP checks at four rates (STUDIO and LIVE) and 27 plugin checks;
- an independent black-box audit through a VST3 host (`tests/independent/dr_audit.py --quality`, 21 checks);
- AddressSanitizer + UBSan;
- pluginval at strictness 10;
- Steinberg's VST3 validator (47/47).

The reasoning behind every design choice is in [docs/DECISIONS.md](docs/DECISIONS.md).

**Known limits** (details in DECISIONS):
- The measured everyday rooms (bigger, more varied than the trained ones) gain least: 3.6 dB of room removed,
  quality 3.71 -> 3.87, and SI-SDR 0.1 dB below the input.
- Singing in a room: the room comes down by 5.6 dB but quality ends a little below the input (ViSQOL 4.25 vs
  4.42). Held notes lose about 2 dB more than speech, and a very high falsetto lost up to 7 dB around 1 kHz in one
  singer. For a sung part, Gentle (60 %) is the safe choice.
- Low room modes are only partly removed (in the bare room above, 80-150 Hz still rings after a phrase).

## Engineering progress

| Version | Date | Engineering | Measured (48 held-out examples, 85 %) |
|---|---|---|---|
| **1.0.0** | 2026-10-16 | A recurrent network (ERB gains + a complex deep filter below 4.5 kHz, 32 ms look-ahead) trained on 80 voices in 4,000 simulated small rooms (booths, closets, cars, bedrooms, bathrooms) + 7 measured rooms; 16 singers so held notes are not mistaken for a room; exact-latency resampling for any rate | ViSQOL 3.96 -> 4.11, room left -5.4 dB; WPE (the standard method) removes 0.2-1.8 dB; dry singing held notes: -11.6 dB before the singers were added, -2 dB after; the artist's blind A/B: preferred 4/4 |
| **2.0.0** | 2026-10-17 | Rap and spoken word first: fine-tuned on dense takes (0-120 ms between phrases), 15 % singing, home rooms weighted | Regular speech: ViSQOL 4.13, CD 3.43, room left -5.7 dB (1.0: 4.11 / 3.45 / -5.4); dense takes better on every score; the artist's take: the room after each phrase -27.9 -> -37.8 dB |
| **3.0.0** | 2026-10-26 | LIVE: a causal network (fine-tuned from 2.0) as a zero-latency minimum-phase filter per hop with an onset guard (Axell Velvet 3.0's method via the shared core); STUDIO / LIVE through the latency handshake; Axell's Dojo look | LIVE at 0 ms: room left -3.7 dB, ViSQOL 4.00, voice damage -11.5 dB (STUDIO -11.3); STUDIO unchanged (C++ = trained model to -134 dB); 45 DSP + 27 plugin checks, ASan clean, audit 21/21, pluginval L10, validator 47/47 |

Every design decision with its measurement: [docs/DECISIONS.md](docs/DECISIONS.md); all changes: [CHANGELOG.md](CHANGELOG.md).

## Install (Linux)

Download `AxellDeReverb-3.0.0-linux-x86_64.tar.gz` from the
[latest release](https://github.com/zmardanian/axell-dereverb/releases/latest) (optional: `sha256sum -c SHA256SUMS.txt`),
then:

    tar xzf AxellDeReverb-3.0.0-linux-x86_64.tar.gz
    cd AxellDeReverb-3.0.0-linux-x86_64
    ./install.sh

Or let the [Axell Signature Dojo](https://github.com/zmardanian/axell-signature-dojo) install and update it.

Then in REAPER: Options > Preferences > Plug-ins > VST > Re-scan.

## Build (Bazzite / any Linux, inside a distrobox)

    bash scripts/setup-toolbox.sh
    bash scripts/build.sh
    bash scripts/test.sh               # DSP + plugin suites (--quick, --asan)
    bash scripts/pluginval.sh
    bash scripts/install.sh

The DSP lives in JUCE-free headers (`Source/DRNet.h`, `Engine.h`, `Resampler.h`, `DeReverb.h` and the shared Axell
core in `external/axell-core` or `../axell-core`). The network is trained by `research/train_dr.py` and exported by
`research/export_dr.py` to `Resources/dereverb.bin`, which is embedded in the plugin.

## Contact

Axell Yobag - axellpetre@protonmail.com. Issues on GitHub are welcome too.

Licence: GNU AGPLv3 (`LICENSE`); third-party components and the data notices in `THIRD_PARTY_NOTICES.md` (VCTK,
BUT ReverbDB and VocalSet for training; CMU ARCTIC, the MIT IR Survey and 4 VocalSet singers for evaluation). No
code, data or graphics of any commercial product are used.
