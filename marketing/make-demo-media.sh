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

# git demo: a small repo with two authors, a line that changed hands, a
# string to search for, and a feature branch ahead of main. Authors are
# fake and set per commit; signing and hooks are off so your own git config
# doesn't leak into (or break) the recording.
if command -v git >/dev/null 2>&1; then
    P="$OUT/project"
    rm -rf "$P" && mkdir -p "$P/src"
    g() { git -C "$P" -c commit.gpgsign=false -c core.hooksPath=/dev/null "$@"; }
    commit() {  # commit AUTHOR MESSAGE
        GIT_AUTHOR_NAME="$1" GIT_AUTHOR_EMAIL="$1@example.com" \
        GIT_COMMITTER_NAME="$1" GIT_COMMITTER_EMAIL="$1@example.com" \
            g commit -q -m "$2"
    }
    g init -q -b main
    printf 'import os\n\nPORT = 8080\nHOST = "0.0.0.0"\n' > "$P/src/server.py"
    g add -A && commit ana "initial server"
    printf 'import time\n\ndef retry(fn, attempts=3):\n    for _ in range(attempts):\n        try:\n            return fn()\n        except OSError:\n            time.sleep(1)\n' > "$P/src/retry.py"
    g add -A && commit bruno "add retry helper"
    printf 'SLOW_MS = 500\n' > "$P/src/db.py"
    g add -A && commit ana "log slow queries"
    printf 'import os\n\nPORT = int(os.environ.get("PORT", 8080))\nHOST = os.environ.get("HOST", "0.0.0.0")\n' > "$P/src/server.py"
    g add -A && commit bruno "read port and host from env"
    g switch -q -c feature-login
    printf '<form id="login"></form>\n' > "$P/src/login.html"
    g add -A && commit ana "add login form"
    printf 'MIN_PASSWORD = 12\n' > "$P/src/auth.py"
    g add -A && commit ana "validate passwords"
    g switch -q main
fi

# curl demo: a JSON body to POST (the demo only prints that command)
printf '{"name": "Ada", "email": "ada@example.com"}\n' > "$OUT/data.json"

# magick demo: a hero banner, a sprite on a white background and animation
# frames, all drawn by ImageMagick itself (skipped if it isn't installed)
if command -v magick >/dev/null 2>&1; then
    magick -size 1600x900 gradient:'#1e3c72'-'#2a5298' "$OUT/hero.png"
    magick -size 256x256 xc:white -fill '#e74c3c' -draw 'circle 128,128 128,40' "$OUT/sprite.png"
    rm -rf "$OUT/frames" && mkdir -p "$OUT/frames"
    i=0
    for c in '#e74c3c' '#e67e22' '#f1c40f' '#2ecc71' '#3498db' '#9b59b6'; do
        i=$((i + 1))
        magick -size 160x160 xc:'#1e1e2e' -fill "$c" -draw "circle 80,80 80,$((20 + i * 8))" "$OUT/frames/f$i.png"
    done
    # reset what a previous recording left behind
    rm -f "$OUT/hero.webp" "$OUT/sprite_transparent.png" "$OUT/animation.gif"
else
    echo "note: ImageMagick not installed; skipping the magick demo files (brew install imagemagick)"
fi

echo "demo files in $OUT:"
ls -1 "$OUT"
