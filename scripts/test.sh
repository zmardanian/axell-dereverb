#!/usr/bin/env bash
# Builds and runs the test suites inside the distrobox.
#   scripts/test.sh          Release: DSP suite (3 rates + CPU) and plugin suite -> tests/report.txt
#   scripts/test.sh --quick  48 kHz only, no CPU benchmark
#   scripts/test.sh --asan   RelWithDebInfo + AddressSanitizer + UBSan -> tests/report-asan.txt
set -euo pipefail
cd "$(dirname "$0")/.."
DIR="$PWD"
JUCE_SRC=""
for d in "$DIR/../axell-penta-eq/build/_deps/juce-src" "$DIR/../axell-duo-compressor/build/_deps/juce-src"; do
  [ -f "$d/CMakeLists.txt" ] && { JUCE_SRC="-DFETCHCONTENT_SOURCE_DIR_JUCE=$(realpath "$d")"; break; }
done
if [ "${1:-}" = "--asan" ]; then
  distrobox enter juce-build -- bash -lc "cd '$DIR' && cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDR_SANITIZE=ON $JUCE_SRC >/dev/null \
    && cmake --build build-asan --target DrDspTests DrPluginTests -j\$(nproc) \
    && export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 LSAN_OPTIONS=suppressions='$DIR'/tests/lsan.supp UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    && { ./build-asan/DrDspTests --quick && ./build-asan/DrPluginTests_artefacts/RelWithDebInfo/DrPluginTests; } 2>&1 | tee tests/report-asan.txt"
  exit $?
fi
ARGS="${1:-}"
distrobox enter juce-build -- bash -lc "cd '$DIR' && { [ -f build/CMakeCache.txt ] || cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release $JUCE_SRC; } \
  && cmake --build build --target DrDspTests DrPluginTests -j\$(nproc) \
  && { ./build/DrDspTests $ARGS && ./build/DrPluginTests_artefacts/Release/DrPluginTests; } 2>&1 | tee tests/report.txt"
