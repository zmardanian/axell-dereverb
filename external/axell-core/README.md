# Axell core

Shared code of the Axell plugins and the Axell Signature Dojo (Axell's Dojo):

- `include/axell/dsp` (JUCE-free, tested with plain g++): fast dB math, filters (RBJ biquads, BS.1770 K-weighting,
  DC blocker, 90-degree allpass pair), running maximum, linear-phase oversampler (1x-16x, integer latency, optional
  band limit to 0.48 fs with 120 dB stopbands), true-peak limiter (8x detection, exact to 0.48 fs), mastering limiter
  (per-channel with stereo link, four program-dependent release styles, bass-aware hold), true-peak meter, loudness
  meter (BS.1770-4 / EBU R128: momentary, short-term, integrated, loudness range), dither (TPDF / 9th-order F-weighted
  noise shaping), latency handshake, display FIFO; the sibilance detector (Velvet 1.0 / 3.0 networks) and the voice
  decision of Axell Gate 2.0 (VoiceActivity, room model, fixed-lag HMM, VoicePresence: causal, 10 ms).
- `include/axell/gui`, `src/gui`: the Axell look (colours, Inter fonts, look and feel, knob, segmented switch,
  toggles, choice, menu button); each plugin's own ground (AXELL_THEME_HUE); the Axell's Dojo emblem, header rule
  and brush title with the plugin's hanko seal (BrandTitle.h).
- `tools`: the independent black-box VST3 audit host and its Python helpers.
- `tests/CoreTests.cpp` (124 checks): EBU Tech 3341 / 3342 loudness conformance, true-peak accuracy, dither noise
  spectrum, limiter ceilings and release behaviour, latency handshake, sibilance and voice decisions.

Licence: GNU AGPLv3 (LICENSE). Fonts: Inter and Yuji Syuku (a subset, `BrushFontData.h`; Copyright 2021 The Yuji
Project Authors), both SIL Open Font License 1.1 (resources/fonts). Contact: axellpetre@protonmail.com
