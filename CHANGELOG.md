# Changelog

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
