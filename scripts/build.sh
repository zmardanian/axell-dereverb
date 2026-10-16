#!/usr/bin/env bash
# Configures and builds inside the distrobox. Usage: scripts/build.sh [Release|Debug]
set -euo pipefail
cd "$(dirname "$0")/.."
BOX=juce-build
CFG="${1:-Release}"
DIR="$PWD"
# reuse an already downloaded JUCE 8.0.15 when one is next door (no network needed)
JUCE_SRC=""
for d in "$DIR/../axell-penta-eq/build/_deps/juce-src" "$DIR/../axell-duo-compressor/build/_deps/juce-src"; do
  [ -f "$d/CMakeLists.txt" ] && { JUCE_SRC="-DFETCHCONTENT_SOURCE_DIR_JUCE=$(realpath "$d")"; break; }
done
distrobox enter "$BOX" -- bash -lc "cd '$DIR' && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=$CFG $JUCE_SRC && cmake --build build -j\$(nproc)"
BUNDLE="build/AxellDeReverb_artefacts/$CFG/VST3/Axell De-Reverb.vst3"
[ -d "$BUNDLE" ] && echo "Built: $DIR/$BUNDLE" || { echo "Bundle not found: $BUNDLE"; exit 1; }
