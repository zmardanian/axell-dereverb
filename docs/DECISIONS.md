# Axell De-Reverb - design decisions

Every decision was taken on a measurement. The scripts are in `research/` and the checks in `tests/`.

## D1 - The goal, and the rooms that matter

- **Goal:** the most advanced and reliable vocal de-reverb that can be built, for mixing (latency is compensated, so
  quality comes first; no low-latency monitoring mode in 1.0).
- **What it removes:** the room a close vocal mic picks up. Two kinds:
  - **The tail:** a bare bedroom, a bathroom.
  - **Early reflections and box coloration:** a wardrobe booth, a closet, a car. In a foam-lined wardrobe, most of
    what is heard is the 350-600 Hz "box" from walls 30-50 cm away. Most de-reverb research targets long tails in
    big rooms.
- **Benchmark weighting:** small rooms. Results are reported per room type, and early-reflection / coloration
  reduction is scored on its own.

## D2 - Rooms with ground truth (`research/rooms.py`)

- **Simulated** (measured responses of wardrobes, closets and cars are rare in open data):
  - an image-source shoebox (Allen & Berkley 1979) at 48 kHz;
  - 7-band wall absorption from standard material tables (foam, clothes, wood, drywall, plaster, tile, glass,
    carpet, upholstery);
  - a cardioid mic 10-30 cm from the mouth;
  - a statistical tail (Eyring RT60 per band) from 60 ms, energy-matched.
  - Room types: booth, closet, car, bedroom, furnished bedroom, bathroom, living room, treated room.
  - Checks: decays match Eyring in the closet, car, bedroom and bathroom. The wardrobe booth has -1.4 dB early
    reflections re the direct sound and a long low-frequency decay (foam barely absorbs below 300 Hz).
- **Measured:**
  - BUT ReverbDB: 2,325 responses, 9 rooms, CC BY 4.0. Training uses 7 rooms; 2 are held out.
  - MIT IR Survey: 271 everyday rooms. Its licence is stated only on a mirror, so it is used for evaluation only.
  - Their direct sound (bandlimited by the measurement) is replaced by a full-band impulse, and only their
    reflections are used. BUT's reflections are rescaled to a close vocal mic (direct-to-reverberant ratio 2-20 dB;
    measured at a distance it was -19 dB).
- **Target:** the voice through the direct sound alone, i.e. as the mic would hear it in free space.

## D3 - Voices

- **Evaluation:** CMU ARCTIC (3 voices) and 4 VCTK voices, never trained on.
- **Training:** 80 VCTK voices (CC BY 4.0, 48 kHz, women and men, 11 accents), fetched with HTTP range requests.
  Labels are not needed: the target is the whole direct-sound signal.

## D4 - Scores (`research/dr_metrics.py`)

| Score | Measures | Source / licence |
|---|---|---|
| ViSQOL v3 audio | overall quality, MOS-LQO | visqol-python, Apache-2.0, conformance-tested against Google's C++ implementation |
| ESTOI | intelligibility | pystoi, MIT |
| SRMR | reverb amount, non-intrusive | SRMRpy, MIT |
| Cepstral distance, FWSSNR | REVERB challenge definitions | |
| SI-SDR | vs the true direct sound | |
| Coloration | third-octave long-term spectrum error, with the 350-600 Hz box band separately | |
| DAMAGE / LEFT | oracle diagnostics: harm to the direct sound where it dominates; room left where the room dominates | |

No PESQ or POLQA (licence restrictions). A blind ABX set from the user's own takes is the final listening sign-off.

## D5 - Baselines (`research/dr_baselines.py`, `research/bench_dr.py`)

- **WPE** (nara_wpe, MIT): the standard linear-prediction de-reverb.
  - It helps with tails: about 1 dB SI-SDR in the bedroom and living room, almost without damage.
  - It does nearly nothing for early reflections (booth / closet / car: 0.2-0.6 dB of room removed).
- **Late-reverb suppression** (Lebart / Habets, given the true RT60): removes more in live rooms but damages the
  voice, and does nothing in small rooms.
- **The wardrobe booth is the hardest case:** input SI-SDR -6 dB and 6 dB of coloration error, untouched by both.
  So the core must handle early reflections. That points to a trained network with a deep filter (a short complex
  filter per frequency over several frames), which can undo within-frame reflections, unlike a gain.

## D6 - The network (`research/dr_model.py`, `research/train_dr.py`)

- **STFT:** 1024 / 512 at 48 kHz (power-of-two FFT, as in the shared core).
- **Inputs per frame:** 32 ERB band log powers (running-mean normalised) and the complex spectrum of bins 0-95 (to
  4.5 kHz, running-magnitude normalised). The frame sees 2 past frames and 3 future ones (32 ms of look-ahead).
- **Network:** Linear + ReLU, then 2 GRUs (256, with a residual). Outputs: 32 ERB gains, an order-5 deep filter for
  bins 0-95, and a per-bin mix. 1.41 M parameters.
- **Loss:** compressed magnitude (|.|^0.3) plus 0.3 x compressed complex, against the direct sound.
- **Training data:** 70 % simulated rooms, 20 % measured, 10 % no room. In the no-room 10 % the network must leave a
  dry take untouched: reliability.

## D7 - The C++ engine (`Source/`)

- **Parity:** `DRNet.h` mirrors the model. With the shipped weights the output matches PyTorch to -135 dB, tested in
  the suite from a reference written at export time.
- **Inference:**
  - dot products with 8 accumulators (they vectorise without fast-math, with a fixed summation order);
  - runtime dispatch to AVX2/FMA where the CPU has it;
  - CPU: ~7-9 % of one core for stereo; a mono vocal is half.
- **Any host rate** (`Resampler.h`, `DeReverb.h`): the network runs at 48 kHz. At 44.1 / 88.2 / 96 kHz there are
  polyphase Kaiser resamplers (-104 to -140 dB residual), and the output resampler's start phase makes the total
  latency a whole number of host samples.
- **Exact transparency:** the input also takes the round trip unprocessed. Its difference from the delayed input (the
  band above the network, about 20.7 kHz at 44.1 kHz) is added back, so REDUCTION 0 is the input to -292 to -335 dB at
  every rate, and the top octave passes untouched.
- **REDUCTION and LISTEN** mix at 48 kHz, so dry and processed stay sample-aligned. LISTEN plays exactly what is
  removed (processed + removed = input, tested).
- **Two bugs the tests found:**
  - the dry resampler's fine delay was set before it was prepared;
  - a NaN poisoned the resampler history before the engine's own input check. The input is now cleaned first.

## D8 - Training, and when to stop

- 80 VCTK voices, 3 s examples, batch 16, Adam (lr 1e-3, warm-up, halved every 40,000 steps), on the CPU
  (0.43 s per step with 6 data workers, one thread each).
- The benchmark was run on checkpoints along the way. It improved quickly to about 20,000 steps and then stayed
  flat: the quick scores at 20,000, 30,000 and 41,000 steps were within noise. Training was stopped at 43,200 steps;
  1.0 ships the 41,000-step weights.
- More steps of the same data would not have helped; more *kinds* of data did (D13: singing, 6,000 more steps).

## D9 - REDUCTION 85 % by default

Mixing the input back in (REDUCTION < 100 %) was measured on the whole benchmark:

| REDUCTION | CD | FWSSNR | SI-SDR | Coloration | Room left |
|---|---|---|---|---|---|
| 100 % | 3.55 | 11.28 | -1.65 | 2.76 | -8.1 dB |
| 85 % | 3.48 | 11.39 | -1.27 | 2.61 | -6.4 dB |
| 70 % | 3.59 | 11.32 | -1.19 | 2.64 | -5.0 dB |

85 % is the best balance on every score but SI-SDR: a little of the input hides what the network gets wrong, and
the room is still 6.4 dB down. The presets: Natural 85 % (default), Clean 100 %, Gentle 60 %.

This was measured with the speech-only weights. With the 1.0 weights (D13) 85 % is again best on CD, FWSSNR, SI-SDR
and coloration (3.45 / 11.44 / -0.80 / 2.62 vs 3.50 / 11.31 / -0.83 / 2.66 at 100 %).

## D10 - Digital silence (found by the independent audit)

- **The problem:** the network's inputs are normalised by running means. Training examples always had room tone in
  their pauses; a DAW gives *exact* zeros before the first take, and a gate in front of the plugin gives exact zeros
  between phrases. Those dragged the running means far outside anything trained on, and the next phrase was handled
  badly until they recovered (about a second).
- **Measured** (SI-SDR gain over the input, 12 takes in booth, bedroom, closet and bathroom; whole take / first 2 s):

| | before | after |
|---|---|---|
| A normal take | +2.31 / +0.75 dB | +2.31 / +0.75 dB (unchanged) |
| A gate in front (exact zeros between phrases) | +1.00 / -0.79 dB | +2.00 / +1.80 dB |
| 2 s of silence before the take | +1.29 / -2.58 dB | +2.36 / +1.03 dB |

- **The fix (no retraining):** a frame quieter than -130 dBFS (below any training example) is digital silence.
  - Until the first frame that is not silent, the network stays as at the start: the stream effectively begins
    there.
  - Later silent frames do not move the running means.
- **Tests:** the parity reference now contains leading silence and a gated gap (C++ = PyTorch to -130 dB). A DSP test
  checks that silence before a take changes nothing (-325 dB).

## D11 - Smoothing that never arrived (found by the independent audit)

- **The problem:** REDUCTION and LISTEN glide (about 40 ms) with a one-pole smoother in float. Near its target, the
  step falls below half a float's resolution and the smoother stops about 6e-5 short. It never reached the 1e-6 snap,
  so LISTEN sat at 0.99996 and processed + removed missed the input by -88 dB.
- **The fix:** snap from 1e-4 (an -80 dB step, inaudible). The same fix went into the OUTPUT gain.
- **The test:** the DSP suite's LISTEN check now moves both controls after the start, as a host does. It fails on
  the old code (-90 dB) and passes on the new (-139 to -160 dB).

## D12 - The independent audit's quality checks

- **Room removal:** the energy 80-400 ms after each phrase re the phrase, input vs output, for 3 real voices (CMU
  ARCTIC phrases with 0.8 s pauses) in exponential rooms of RT60 0.3 s and 0.6 s, at the default REDUCTION. It must
  drop by at least 6 dB.
  - Measured: -45.4 -> -56.0 dB and -31.9 -> -44.9 dB.
  - It first used a synthetic voice: a steady buzz with a gliding pitch. After D13 the network treats that like a
    held sung note and leaves more of its tail (-38.8 -> -40.6 dB at RT60 0.3 s), while real voices lose the same
    room as before (within 1 dB, 3 voices x 5 rooms). The synthetic result is still printed, for information.
  - The first version (a decay *rate*) could not measure the input at all, and so passed without measuring.
- **Dry voices are left alone:** a synthetic voice and three real voices (CMU ARCTIC, never trained on).
  - Limits: SI-SDR > 10 dB, and no octave band 125 Hz - 8 kHz moves by more than 3 dB.
  - Measured: SI-SDR 28-36 dB; at most 0.5 dB in any octave.

## D13 - Singing (found by the singing benchmark)

- **The problem:** all 80 training voices were speaking. To such a network a held sung note looks like a ringing
  room. `research/bench_sing.py` measures 4 VocalSet singers never trained on (song excerpts straight and with
  vibrato, held notes, belted arpeggios), dry and in the small rooms. With the speech-only weights at 85 %:
  - dry held notes lost 11.6 dB, belting 9.6 dB, songs 3.3-3.8 dB;
  - singing in rooms fell from ViSQOL 4.42 (input) to 3.20 (at 100 %).
- **The fix:** 6,000 more training steps from the speech-only weights (peak lr 3e-4, halved every 3,000 steps), with
  40 % of the examples sung by the 16 other VocalSet singers (CC BY 4.0), in the same rooms and conditions.
- **Singing after** (85 %):

| | input | speech-only weights | 1.0 |
|---|---|---|---|
| Dry: level, songs / held notes / belting | | -3.3 / -11.6 / -9.6 dB | -1.5 / -3.2 / -1.7 dB |
| Dry: ViSQOL | 4.65 | 3.29 (100 %) | 4.36 |
| In rooms: ViSQOL / ESTOI | 4.42 / 0.64 | 3.20 / 0.47 (100 %) | 4.29 / 0.62 |
| In rooms: room left | 0 | | -5.7 dB |

  The same singers *speaking* in the same recording room lose 0.8-1.3 dB, so about 1 dB of the sung level change is
  VocalSet's own room. SI-SDR and FWSSNR were undefined for some sung examples in the bedroom and bathroom types
  (a silent stretch of the target) and are left out.
- **Speech did not get worse** (the full benchmark, 85 %): ViSQOL 4.14 -> 4.11, CD 3.48 -> 3.45, SI-SDR -1.27 ->
  -0.80, coloration 2.61 -> 2.62, damage -10.0 -> -11.3 dB. The price is about 1 dB less room removed (-6.4 -> -5.4
  dB). Real voices keep the same phrase-end tail removal (D12). On the artist's dry take: -27.9 -> -37.3 dB at 85 %,
  -41.1 dB at 100 % (speech-only weights at 85 %: -39.9 dB).
- **85 % stays the default:** the best balance for both speech and singing. Clean (100 %) for a spoken or rapped
  take in a bad room; Gentle (60 %) for sung parts.
- **Left:**
  - one singer's very high falsetto (energy 400-1,600 Hz) still loses up to 7 dB around 1 kHz;
  - held notes lose about 2 dB more than speech.
  A larger singing set would be the next step.

## D14 - Listening sign-off

A blind set from the artist's own dry take (`research/make_abx.py`): 4 excerpts of 8 s, original and processed in
random order, loudness-matched, plus an X. Judged with the speech-only weights at 85 %:
- the processed take was preferred in all 4 excerpts ("without the room reverb and the artifacts of an open room");
- X was identified in 3 of 4.
The 1.0 weights leave 2.6 dB more room on that take at 85 % (D13).

## D15 - 2.0: rap and spoken word first

- **Why:** the artist raps. 1.0's speech training was mostly read sentences framed by long pauses (each VCTK
  recording keeps its silence at both ends). A rap take is dense: the next word starts while the last one's room
  still rings.
- **Training:** 6,000 more steps from the 1.0 weights (peak lr 3e-4, halved every 3,000 steps). The best checkpoint
  is step 2,000.
  - 70 % of the speech examples are dense takes: utterances trimmed to their sound, joined with 0-120 ms gaps (15 % of
    gaps breath-length, 150-400 ms).
  - Home-recording rooms (booth, closet, bedrooms, bathroom) drawn 1.5x as often.
  - Singing reduced from 40 % to 15 % of the examples.
- **Choosing the checkpoint:** the bar was set before any results were seen. 2.0 had to match or beat 1.0 on every
  score of the dense and the regular benchmark, remove at least as much room, and keep dry held notes within about
  6 dB.
  - Steps 3,000-6,000 traded a little accuracy for room removal.
  - Step 2,000 was the only checkpoint better than 1.0 on every score of both.
- **Results** (85 %, 1.0 -> 2.0):

| | dense rap-like takes | regular benchmark |
|---|---|---|
| ViSQOL | | 4.11 -> 4.13 |
| Cepstral distance | 2.92 -> 2.89 | 3.45 -> 3.43 |
| FWSSNR | 11.56 -> 11.68 | 11.44 -> 11.56 |
| SI-SDR (dB) | -0.78 -> -0.74 | -0.80 -> -0.72 |
| Coloration (dB) | 2.59 -> 2.53 | 2.62 -> 2.55 |
| Room left (dB) | -5.5 -> -5.7 | -5.4 -> -5.7 |

- **The artist's take** (85 %): the room after each phrase -37.3 -> -37.8 dB, with less change to the voice
  (60-150 Hz -4.7 -> -4.2 dB, 150-350 Hz -1.6 -> -1.2 dB).
- **Audit:** the phrase-end room with real voices -56.0 -> -58.3 dB (RT60 0.3 s), -44.9 -> -45.0 dB (0.6 s).
- **Singing,** which gave a little back as expected:
  - dry held notes -3.2 -> -2.0 dB, dry ViSQOL 4.36 -> 4.38;
  - singing in rooms: ViSQOL 4.29 -> 4.25, coloration 2.60 -> 2.73.
- **Unchanged:** parameters, presets and saved projects (a 1.0 project opens with the new network).
- **Not re-run:** the blind A/B (D14) was on the speech-only weights.

## D16 - The artist's setup (untreated room -> LCT 440 PURE -> Volt 176): studied, 2.0 kept

- **Measured from 10 of the artist's dry takes** (`research/measure_setup.py`, the decay after word endings per octave
  band; no labels): mids and highs 0.13-0.21 s, the bass 0.26-0.73 s (it varies by session), the floor 21-40 dB
  under the voice and falling ~4.6 dB per octave.
- **Matched rooms** (`research/make_your_rooms.py`): the room simulator's untreated rooms whose band RT60s fall in the
  measured range (x1.4). Furnished bedrooms match; bare bedrooms ring far too long above 500 Hz. 240 training rooms, 24
  test rooms. Matching through the measurement itself was tried first and dropped (the detector found too few word
  endings in rooms that did match).
- **Fine-tune** (`train_dr.py --setup 0.5`: half the examples in those rooms, the measured floor 22-40 dB under the
  voice, a 1176-style compressor after the room on half of them; 6,000 steps from 2.0). On held-out voices in held-out
  matched rooms the best checkpoint (2,000) left 0.6 dB less room (-11.3 -> -11.9 dB) and a lower cepstral distance
  (2.51 -> 2.30), but a lower FWSSNR (17.3 -> 16.8) and more colour (box 0.41 -> 0.65). On the 10 real takes it did
  exactly what 2.0 does (decay after word endings and floor identical). **Not shipped: 2.0 already handles this room.**
- **What does matter there: REDUCTION** (`research/eval_setup.py`, held-out matched rooms):

  | REDUCTION | room + noise left | damage | cepstral distance |
  |---|---|---|---|
  | 85 % | -9.0 dB | -13.1 dB | 2.53 |
  | 95 % | -10.6 dB | -12.5 dB | 2.49 |
  | 100 % | -11.3 dB | -12.2 dB | 2.51 |

  On the 10 real takes, 85 -> 100 % halves what is left of the bass decay (0.15 -> 0.13 s, from 0.29 recorded) and
  lowers the floor 6 dB more. The Dojo's Voice Profile now picks 95 % for a small room with a background less than
  35 dB under the voice.

## D17 - 3.0: LIVE - no latency, for recording (`Source/Engine.h`, `research/train_dr.py` with AXDR_LA=0)
- **Why.** STUDIO looks 32 ms ahead (latency 53 ms): fine for mixing, unusable for hearing yourself while recording,
  so the Dojo's tracking chain had to leave De-Reverb out.
- **A causal network.** The same network without look-ahead (input frames t-2 .. t), fine-tuned from 2.0's weights
  (its input layer trimmed to the past and present frames) on 2.0's recipe: 15 % singing, 70 % dense rap-like
  takes, lr 3e-4, 10 000 steps; step 8000 shipped (`Resources/dereverb_live.bin`).
- **Zero latency: Axell Velvet 3.0's method.** Every hop the network's frame becomes a magnitude response
  (|enhanced| / |input| per bin, clamped to -30 .. 0 dB) and that a minimum-phase filter (real cepstrum, 512 taps;
  axell-core `MinPhase.h`, the method of Velvet's LIVE de-esser), which filters the current samples and fades into
  the next one over the hop.
- **Onset guard.** The filter comes from the last analysed frame, which at a word's start is mostly the room's tail:
  the start of every word was cut (voice damage -10.0 dB vs STUDIO's -11.3). A room only decays, so a level 8 dB
  above that frame's is direct sound: the filter steps aside (1 ms) and returns over 25 ms. Swept on the quick
  benchmark: off -10.4 / +6 dB -11.8 / +8 dB -11.5 (room left -3.7 dB) / +10 dB -10.9.
- **Result** (`docs/research/bench-3.0-live.txt`; 48 held-out examples, REDUCTION 85 %):

  | | ViSQOL | ESTOI | SRMR | CD | damage | room left |
  |---|---|---|---|---|---|---|
  | input | 3.96 | 0.74 | 5.74 | 4.29 | -14.5 | 0.0 dB |
  | STUDIO (53 ms) | 4.13 | 0.78 | 6.50 | 3.43 | -11.3 | -5.7 dB |
  | **LIVE (0 ms)** | **4.00** | **0.74** | **6.26** | **3.84** | **-11.5** | **-3.7 dB** |

  LIVE takes about two thirds of the room out with no delay and does the voice no more harm than STUDIO; STUDIO
  remains the mixing mode. At 44.1 / 96 kHz LIVE adds only the resamplers' 2.6 / 1.8 ms.
- **Switching** STUDIO <-> LIVE changes the latency: the axell-core latency handshake (Penta EQ 2.0.1) fades out,
  reports the new latency to the host, switches once compensated and fades in.

