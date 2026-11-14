#!/usr/bin/env bash
# Checks NorthRay Capture's streaming destinations (Winsock, its RTMP client, buffering, reconnection)
# under Wine: one encoded stream (made with ffmpeg) fed to several local ffmpeg RTMP servers at once,
# with one destination stopped part-way, one added part-way, and one whose server only appears later.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
S="$ROOT/apps/capture"
OUT="$(mktemp -d)"
trap 'pkill -f "listen 1" 2>/dev/null || true; rm -rf "$OUT"' EXIT
cd "$OUT"
ffmpeg -v error -y -f lavfi -i testsrc2=size=1280x720:rate=30 -t 20 -c:v libx264 -preset ultrafast -g 30 -bf 0 -bsf:v h264_metadata=aud=insert -f h264 v.h264
ffmpeg -v error -y -f lavfi -i sine=frequency=440:sample_rate=48000 -ac 2 -t 20 -c:a aac -b:a 128k -f adts a.aac
x86_64-w64-mingw32-g++ -std=c++20 -O1 -I"$S/src" -I"$S/core/include" "$S/tests/destinations_check.cpp" "$S/src/output/destination.cpp" \
  "$S/src/output/net.cpp" "$S"/core/src/media.cpp "$S"/core/src/rtmp.cpp -lws2_32 -lsecur32 -lcrypt32 -static -o dest_check.exe
for p in 19371 19372 19373; do (timeout 60 ffmpeg -v error -y -listen 1 -i rtmp://127.0.0.1:$p/live/k -c copy -f flv out$p.flv >/dev/null 2>&1 &); done
(sleep 4; timeout 50 ffmpeg -v error -y -listen 1 -i rtmp://127.0.0.1:19379/live/k -c copy -f flv out19379.flv >/dev/null 2>&1) &
sleep 1
wine dest_check.exe "$(winepath -w v.h264)" "$(winepath -w a.aac)" | grep -E "^end|stopped|started|fed"
sleep 3
echo "Expected: A 600 frames, B about 240 (stopped at 240), C 240 (started at 360), D from when its server appeared."
for f in out19371.flv out19372.flv out19373.flv out19379.flv; do
  echo "$f: $(ffprobe -v error -count_frames -select_streams v -show_entries stream=nb_read_frames -of csv=p=0 "$f") video frames," \
       "$(ffprobe -v error -count_packets -select_streams a -show_entries stream=nb_read_packets -of csv=p=0 "$f") audio frames," \
       "$(ffmpeg -v error -i "$f" -f null - 2>&1 | wc -l) decode errors"
done
