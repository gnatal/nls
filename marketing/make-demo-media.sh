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

# project clutter for the find demo: old logs, big files (one hidden inside
# node_modules), a .DS_Store and an empty folder. Old dates are fixed in the
# past so "older than 30 days" keeps working whenever the demo is recorded.
mkdir -p "$OUT/logs" "$OUT/assets" "$OUT/node_modules/left-pad" "$OUT/tmp"
printf 'archived\n' > "$OUT/logs/archive-2025-03.log"
printf 'archived\n' > "$OUT/logs/archive-2025-06.log"
printf 'current\n'  > "$OUT/logs/today.log"
touch -t 202503010900 "$OUT/logs/archive-2025-03.log"
touch -t 202506010900 "$OUT/logs/archive-2025-06.log"
dd if=/dev/zero of="$OUT/assets/backup.tar" bs=1048576 count=8 2>/dev/null
dd if=/dev/zero of="$OUT/node_modules/left-pad/cache.bin" bs=1048576 count=12 2>/dev/null
printf 'module.exports = s => s;\n' > "$OUT/node_modules/left-pad/index.js"
printf 'x' > "$OUT/.DS_Store"
printf 'x' > "$OUT/src/.DS_Store"

# tar demo: src/ gets the usual things you don't want in a backup (.git,
# node_modules), and a release tarball wraps everything in a top-level
# folder, the classic case for --strip-components. dist/ must exist for -C.
mkdir -p "$OUT/src/.git" "$OUT/src/node_modules/left-pad"
printf 'ref: refs/heads/main\n' > "$OUT/src/.git/HEAD"
printf 'module.exports = s => s;\n' > "$OUT/src/node_modules/left-pad/index.js"
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/nls-demo.XXXXXX")
mkdir -p "$STAGE/release-1.0/bin"
printf '#!/bin/sh\necho "demo app 1.0"\n' > "$STAGE/release-1.0/bin/app"
printf '# demo app 1.0\n' > "$STAGE/release-1.0/README.md"
chmod +x "$STAGE/release-1.0/bin/app"
# COPYFILE_DISABLE: keep macOS tar from adding ._ AppleDouble files
(cd "$STAGE" && COPYFILE_DISABLE=1 tar -czf - release-1.0) > "$OUT/release-1.0.tgz"
rm -rf "$STAGE"
# reset what a previous recording left behind
rm -f "$OUT/backup.tar.gz" "$OUT/backup.tgz" "$OUT/dist/README.md" "$OUT/dist/bin/app"
rmdir "$OUT/dist/bin" 2>/dev/null || true
mkdir -p "$OUT/dist"

echo "demo files in $OUT:"
ls -1 "$OUT"
