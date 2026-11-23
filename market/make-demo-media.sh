#!/bin/sh
# Generate royalty-free demo files for recordings: no real songs or videos,
# so the demo can be published anywhere without copyright issues.
#   sh market/make-demo-media.sh [outdir]      (default: market/demo)
set -e
OUT=${1:-"$(dirname "$0")/demo"}
mkdir -p "$OUT"

command -v ffmpeg >/dev/null 2>&1 || { echo "needs ffmpeg (brew install ffmpeg)"; exit 1; }

# 12 s, 1280x720 test pattern + 440 Hz tone, named like a real recording
ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i testsrc2=size=1280x720:rate=30:duration=12 \
    -f lavfi -i sine=frequency=440:duration=12 \
    -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$OUT/talk.mp4"
cp "$OUT/talk.mp4" "$OUT/demo.mp4"

cat > "$OUT/users.json" <<'EOF'
[
  {"name": "Ana",   "email": "ana@example.com",   "age": 34, "status": "active"},
  {"name": "Bob",   "email": "bob@example.com",   "age": 17, "status": "inactive"},
  {"name": "Carla", "email": "carla@example.com", "age": 42, "status": "active"},
  {"name": "Dan",                                 "age": 30, "status": "active"}
]
EOF

echo "demo files in $OUT:"
ls -1 "$OUT"
