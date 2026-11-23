#!/bin/sh
# Generate royalty-free demo files for recordings: no real songs or videos,
# so the demo can be published anywhere without copyright issues.
#   sh marketing/make-demo-media.sh [outdir]      (default: marketing/demo)
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

# a tiny code base and a log for the grep demo
mkdir -p "$OUT/src"
cat > "$OUT/src/server.py" <<'EOF'
import os

# TODO: read the port from the environment
PORT = 8080

def handle(request):
    # todo: validate the request body
    return {"status": "ok"}
EOF
cat > "$OUT/src/worker.py" <<'EOF'
import time

def run_forever():
    while True:
        time.sleep(1)  # TODO: back off on errors
EOF
cat > "$OUT/src/app.js" <<'EOF'
// TODO: move this to a config file
const API_URL = "http://localhost:8080";
export default API_URL;
EOF
cat > "$OUT/src/README.md" <<'EOF'
# demo service
Nothing to do here.
EOF
cat > "$OUT/app.log" <<'EOF'
2026-11-23 09:00:00 INFO  server started on :8080
2026-11-23 09:00:04 INFO  connected to database
2026-11-23 09:01:12 WARN  slow query (1840ms)
2026-11-23 09:01:13 ERROR connection reset by peer
2026-11-23 09:01:14 INFO  retrying in 2s
2026-11-23 09:01:16 INFO  reconnected
2026-11-23 09:05:42 ERROR disk quota exceeded
2026-11-23 09:05:43 INFO  rotating logs
EOF

echo "demo files in $OUT:"
ls -1 "$OUT"
