#!/usr/bin/env bash
# The Release build that goes into the Linux archive: Ubuntu 22.04 (glibc 2.35) with gcc 12 and the system C++
# runtime, so it runs on Ubuntu 22.04+, Debian 12+, Linux Mint 21+ and Fedora 36+. Same sources and flags as
# scripts/build.sh; renders are bit-identical to the Ubuntu 24.04 build (checked 2026-11-20). A static libstdc++ is
# deliberately NOT used: inside a host it leaves juce::String (float, places) empty (parameter values lose numbers).
# Box: distrobox `axell-jammy` (created here if missing).
set -euo pipefail
cd "$(dirname "$0")/.."
BOX=axell-jammy
if ! distrobox list 2>/dev/null | awk -F'|' '{gsub(/ /,"",$2); print $2}' | grep -qx "$BOX"; then
  distrobox create --yes --name "$BOX" --image docker.io/library/ubuntu:22.04
  distrobox enter "$BOX" -- sudo apt-get update
  distrobox enter "$BOX" -- sudo apt-get install -y gcc-12 g++-12 cmake ninja-build git pkg-config curl unzip \
    libasound2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev libxcursor-dev libxext-dev \
    libxinerama-dev libxrandr-dev libxrender-dev libgl1-mesa-dev libcurl4-openssl-dev
fi
distrobox enter "$BOX" -- bash -lc "cd '$PWD' && cmake -B build-portable -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/bin/gcc-12 -DCMAKE_CXX_COMPILER=/usr/bin/g++-12 && cmake --build build-portable -j\${JOBS:-14} --target AxellDeReverb_VST3"
echo "Built: build-portable/ (now scripts/make-release.sh)"
