#!/usr/bin/env bash
# Checks NorthRay Capture's Twitch and YouTube sign-in code (WinHTTP, the loopback listener, Credential
# Manager) under Wine, against a local stand-in for both services. Nothing reaches the real services.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
S="$ROOT/apps/capture"
OUT="$(mktemp -d)"
x86_64-w64-mingw32-g++ -std=c++20 -O1 -I"$S/src" -I"$S/core/include" -I"$ROOT/libs/third_party" "$S/tests/accounts_check.cpp" \
  "$S/src/output/accounts.cpp" "$S/src/output/http.cpp" "$S/src/output/credentials.cpp" "$S/src/output/net.cpp" "$S"/core/src/*.cpp \
  -lws2_32 -lsecur32 -lcrypt32 -lwinhttp -lbcrypt -ladvapi32 -lshell32 -static -o "$OUT/accounts_check.exe"
python3 "$ROOT/tools/accounts-mock.py" & MOCK=$!
trap 'kill $MOCK 2>/dev/null || true; rm -rf "$OUT"' EXIT
sleep 1
wine "$OUT/accounts_check.exe"
