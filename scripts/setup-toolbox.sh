#!/usr/bin/env bash
# Creates the Ubuntu 24.04 build box and installs JUCE dependencies. Safe to re-run.
set -euo pipefail
BOX=juce-build

if ! distrobox list 2>/dev/null | awk -F'|' '{gsub(/ /,"",$2); print $2}' | grep -qx "$BOX"; then
  distrobox create --yes --name "$BOX" --image docker.io/library/ubuntu:24.04
fi

distrobox enter "$BOX" -- sudo apt-get update
distrobox enter "$BOX" -- sudo apt-get install -y \
  build-essential cmake ninja-build git pkg-config curl unzip \
  libasound2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev \
  libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libgl1-mesa-dev

echo "Toolbox '$BOX' ready."
