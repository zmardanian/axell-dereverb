#!/usr/bin/env bash
# Validates the built plugin with pluginval at strictness 10 (+ Steinberg's VST3 validator when available).
# Reuses the binaries of Axell Penta EQ's tools/ folder when present, else downloads pluginval once.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p tools
for t in pluginval vst3-validator; do
  [ -x "tools/$t" ] || { [ -x "../axell-penta-eq/tools/$t" ] && cp "../axell-penta-eq/tools/$t" tools/; } || true
done
if [ ! -x tools/pluginval ]; then
  curl -fL -o tools/pluginval.zip https://github.com/Tracktion/pluginval/releases/latest/download/pluginval_Linux.zip
  (cd tools && unzip -o pluginval.zip && chmod +x pluginval)
fi
BUNDLE="build/AxellDeReverb_artefacts/Release/VST3/Axell De-Reverb.vst3"
VAL=()
[ -x tools/vst3-validator ] && VAL=(--vst3validator "$PWD/scripts/vst3-validator-wrap.sh")
./tools/pluginval --strictness-level 10 --validate-in-process "${VAL[@]}" --output-dir tools/pluginval-logs --validate "$BUNDLE" "$@"
