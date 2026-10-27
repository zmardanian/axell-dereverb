# Changelog

## 4.0.0 - 2026-10-27
- **TRUE TONE (new):** in small rooms the room removal took a little more low end than the room had added - against
  the voice's direct sound the output was ~3.7 dB thin at 63 Hz and 1-1.5 dB bright above 400 Hz. TRUE TONE watches
  how much De-Reverb removes in each band (its own output vs its input, ~10 s average) and corrects the processed
  signal with a learned model (at most +-3 dB, per REDUCTION amount, no latency, STUDIO and LIVE). Rooms never used
  for training, the real plugin: tone error in artist-like rooms 1.76 -> 1.09 dB at 100 % and 1.23 -> 1.00 dB at
  85 %; in synthetic small rooms 1.92 -> 1.64 / 1.74 -> 1.58 dB (12-17 % of single rooms slightly worse, typically
  under 0.3 dB). On for new instances and every preset; projects saved before 4.0 load with it off and render
  byte-identically. research/truetone_fit.py renders, fits and writes Source/TrueToneModel.h.
- **Version in the header**, small and quiet after the title.

## 3.0.0 - 2026-10-26
- **LIVE mode (new): zero latency, for recording.** A STUDIO / LIVE switch. LIVE runs a causal version of the
  network (fine-tuned from 2.0's) as a minimum-phase filter that changes every 10.7 ms (Axell Velvet 3.0's method),
  with an onset guard that keeps word starts intact. On 48 held-out examples it removes 3.7 dB of room (STUDIO 5.7)
  with ViSQOL 4.00 (input 3.96, STUDIO 4.13) and no more damage to the voice than STUDIO - and no delay, so you hear
  yourself without the room while you record. The Axell Signature Dojo's tracking chain now includes it. (D17)
- **Axell's Dojo look**: the family emblem, a brush-written title with De-Reverb's seal (静, "stillness") and
  De-Reverb's own deep-blue ground.
- Verified: DSP 45 checks (STUDIO parity with the trained model -134 dB, LIVE: zero latency, exact at REDUCTION 0, no
  allocation), plugin 27 (STUDIO <-> LIVE through the latency handshake), ASan + UBSan clean, independent audit 21/21 (with --quality: real voices, rooms, dry voices),
  pluginval strictness 10, Steinberg's VST3 validator 47/47.

## 2.0.0 - 2026-10-17
Rap and spoken word first.

- **Trained for dense delivery:**
  - 70 % of the speech examples are now built like a rap take: utterances trimmed and joined with 0-120 ms gaps,
    so the next word starts while the last one's room still rings.
  - 1.0 trained mostly on read speech with long pauses.
- **Trained more on home-recording rooms:** booths, closets, bedrooms and bathrooms are drawn 1.5x as often.
- **Singing:** reduced to 15 % of training. It is still kept: dry held notes lose 2.0 dB (1.0: 3.2 dB).
- **Measured against 1.0** at the default 85 % (dense rap-like takes, 3 voices it never heard, 8 room types and 24
  measured rooms):
  - better on every score: cepstral distance 2.92 -> 2.89, FWSSNR 11.56 -> 11.68, SI-SDR -0.78 -> -0.74 dB,
    coloration 2.59 -> 2.53;
  - more room removed: -5.5 -> -5.7 dB.
  - The regular speech benchmark improves the same way.
- **On a real dry rap take from a bare room:** more room removed (-37.8 vs -37.3 dB after each phrase) with less
  change to the low-mids (150-350 Hz: -1.2 vs -1.6 dB).
- Parameters, presets and saved projects are unchanged: a 1.0 project opens as it was, with the new network.

## 1.0.0 - 2026-10-16
First release.

- **A trained de-reverb network for close vocals:**
  - ERB-band gains plus a complex deep filter below 4.5 kHz (two GRU layers, 1.41 M parameters, 32 ms
    look-ahead).
  - Trained on 80 speaking voices and 16 singers in simulated small rooms (booth, closet, car, bedroom, bathroom)
    and measured rooms.
  - Sung vocals are kept: dry held notes lose 3.2 dB, where a speech-only network took 11.6 dB.
- **Controls:**
  - REDUCTION (default 85 %), LISTEN: REMOVED, OUTPUT.
  - Meters and a room history.
  - Presets Natural / Clean / Gentle, user presets, A/B.
- **Any rate from 44.1 to 96 kHz:**
  - Whole-sample latency, reported and compensated.
  - REDUCTION 0 and the host bypass give exactly the input.
- **Robust to digital silence:** silence before a take, or a gate in front, does not disturb it.
- **Measured** on voices and rooms it never trained on, against WPE: better quality (ViSQOL 4.11 vs 3.99), a third
  of the coloration error in booths, 5.4 dB of room removed vs 1.0 dB. The figures are in the README.
- **Validated:**
  - 27 DSP and 21 plugin checks, plus a 21-check independent audit in a VST3 host.
  - ASan/UBSan clean, pluginval at strictness 10, VST3 validator 47/47.
  - A blind A/B by the artist on a real take.
