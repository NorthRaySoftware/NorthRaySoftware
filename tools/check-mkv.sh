#!/usr/bin/env bash
# Checks NorthRay Capture's Matroska writer against ffmpeg: encodes a test picture (with a frame
# counter) and two test tones with ffmpeg, puts them in an MKV with Capture's own writer, then has
# ffprobe read it and ffmpeg decode every frame. Needs ffmpeg with libx264, and the native build.
#   tools/check-mkv.sh [build/native]
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=$(find "${1:-build/native}" -name capture_mkv_check -type f | head -1)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
ffmpeg -v error -f lavfi -i "testsrc2=size=1280x720:rate=30:duration=12" -c:v libx264 -preset veryfast -g 60 -bf 0 \
  -x264-params aud=1 -bsf:v h264_mp4toannexb -f h264 "$T/v.h264"
ffmpeg -v error -f lavfi -i "sine=frequency=440:duration=12:sample_rate=48000" -ac 2 -c:a aac -b:a 160k -f adts "$T/a1.aac"
ffmpeg -v error -f lavfi -i "sine=frequency=880:duration=12:sample_rate=48000" -ac 2 -c:a aac -b:a 160k -f adts "$T/a2.aac"
"$BIN" "$T/out.mkv" "$T/v.h264" 30 "$T/a1.aac" "$T/a2.aac"
ffprobe -v error -show_entries format=duration:stream=index,codec_name,width,height,sample_rate,channels:stream_tags=title -of compact "$T/out.mkv"
echo "Decoding every frame (no output means no errors):"
ffmpeg -v error -i "$T/out.mkv" -map 0 -f null -
echo "Seeking to 9 s:"
ffmpeg -v error -ss 9 -i "$T/out.mkv" -frames:v 1 -f null - && echo "ok"
FRAMES=$(ffprobe -v error -count_frames -select_streams v -show_entries stream=nb_read_frames -of csv=p=0 "$T/out.mkv")
echo "Video frames read back: $FRAMES (360 written)"
# A crash before the end: cut the file short and check what's left still plays.
head -c $(( $(stat -c %s "$T/out.mkv") / 2 )) "$T/out.mkv" > "$T/cut.mkv"
CUT=$(ffprobe -v error -count_frames -select_streams v -show_entries stream=nb_read_frames -of csv=p=0 "$T/cut.mkv" 2>/dev/null || echo 0)
echo "Frames readable from a file cut off halfway: $CUT"
