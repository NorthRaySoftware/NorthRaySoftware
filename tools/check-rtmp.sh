#!/usr/bin/env bash
# Checks NorthRay Capture's RTMP client against a real RTMP server (ffmpeg listening): streams a
# test picture and tone through Capture's client, then checks what the server received.
#   tools/check-rtmp.sh [build/native]
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=$(find "${1:-build/native}" -name capture_rtmp_check -type f | head -1)
T=$(mktemp -d)
trap 'kill $SERVER 2>/dev/null || true; rm -rf "$T"' EXIT
ffmpeg -v error -f lavfi -i "testsrc2=size=1280x720:rate=30:duration=8" -c:v libx264 -preset veryfast -g 60 -bf 0 \
  -x264-params aud=1 -bsf:v h264_mp4toannexb -f h264 "$T/v.h264"
ffmpeg -v error -f lavfi -i "sine=frequency=440:duration=8:sample_rate=48000" -ac 2 -c:a aac -b:a 160k -f adts "$T/a.aac"
# The server: takes one publish and saves it.
ffmpeg -v error -listen 1 -timeout 20 -i rtmp://127.0.0.1:19350/live/test-key -c copy -f flv "$T/got.flv" &
SERVER=$!
sleep 1
"$BIN" rtmp://127.0.0.1:19350/live test-key "$T/v.h264" 30 "$T/a.aac"
wait $SERVER || true
ffprobe -v error -show_entries stream=codec_name,width,height,sample_rate,channels -of compact "$T/got.flv"
echo "Decoding what the server received (no output means no errors):"
ffmpeg -v error -i "$T/got.flv" -f null -
FRAMES=$(ffprobe -v error -count_frames -select_streams v -show_entries stream=nb_read_frames -of csv=p=0 "$T/got.flv")
echo "Video frames the server received: $FRAMES (240 sent)"
