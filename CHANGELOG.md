# Changelog

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
