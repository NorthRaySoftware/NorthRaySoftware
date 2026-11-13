#!/usr/bin/env bash
# Cross-compiles the Windows launcher and tests from Linux with MinGW-w64.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build/windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
# Packages copy shared/metadata after linking. Remove the executables so every
# app relinks and repackages, and a metadata-only change is never shipped stale.
find build/windows -maxdepth 3 -name '*.exe' -path '*/apps/*' -delete 2>/dev/null || true
rm -f build/windows/launcher/NorthRayLauncher.exe
cmake --build build/windows -j"$(nproc)"
echo "Built: build/windows/launcher/NorthRayLauncher.exe and build/windows/launcher/packages/"
