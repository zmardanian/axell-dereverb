# Axell core

Shared code of the Axell plugins (Axell Penta EQ, Axell Duo Compressor, Axell Master):

- `include/axell/dsp` (JUCE-free, tested with plain g++): fast dB math, filters (RBJ biquads, BS.1770 K-weighting,
  DC blocker, 90-degree allpass pair), running maximum, linear-phase oversampler (1x-16x, integer latency, optional
  band limit to 0.48 fs with 120 dB stopbands), true-peak limiter (8x detection, exact to 0.48 fs), mastering limiter
  (per-channel with stereo link, four program-dependent release styles, bass-aware hold), true-peak meter, loudness
  meter (BS.1770-4 / EBU R128: momentary, short-term, integrated, loudness range), dither (TPDF / 9th-order F-weighted
  noise shaping), latency handshake, display FIFO.
- `include/axell/gui`, `src/gui`: the Axell look (colours, Inter fonts, look and feel, knob, segmented switch,
  toggles, choice, menu button).
- `tools`: the independent black-box VST3 audit host and its Python helpers.
- `tests/CoreTests.cpp` (80 checks): EBU Tech 3341 / 3342 loudness conformance, true-peak accuracy, dither noise
  spectrum, limiter ceilings and release behaviour, latency handshake.

Licence: GNU AGPLv3 (LICENSE). Contact: axellpetre@protonmail.com
