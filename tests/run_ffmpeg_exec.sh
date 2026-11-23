#!/bin/sh
# Execution tests for ffmpeg: compile each prompt with nls, run the command
# on small generated media, and verify the result with ffprobe.
#
# No media is stored in the repo: fixtures are synthesized with ffmpeg's
# test sources (testsrc2 video + sine audio) into a temp dir on each run.
#
# tests/ffmpeg_exec_cases.tsv lines:  prompt<TAB>checks
# Checks (space-separated, all must hold) apply to the command's output file
# (its last argument):
#   exists          output file exists and is non-empty
#   dur=N~T         duration is N seconds, +/- T
#   w=N  h=N        video width / height
#   fps=N           video frame rate (rounded)
#   v=CODEC a=CODEC first video / audio stream codec (ffprobe codec_name)
#   noaudio novideo stream is absent
#   files=GLOB:MIN-MAX  number of files matching GLOB (image sequences)
#   stdout=TEXT     the command's combined output contains TEXT
NLS=$(cd "$(dirname "${1:-./nls}")" && pwd)/$(basename "${1:-./nls}")
DIR=$(cd "$(dirname "$0")" && pwd)

if ! command -v ffmpeg >/dev/null 2>&1 || ! command -v ffprobe >/dev/null 2>&1; then
    echo "ffmpeg/ffprobe not installed; skipping ffmpeg execution tests (brew install ffmpeg)"
    exit 0
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/nls-ffmpeg.XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT INT TERM
SRC="$WORK/src"; RUN="$WORK/run"
mkdir -p "$SRC" "$RUN"

# ---- fixtures: 5 s, 320x180, 30 fps, keyframe every 0.5 s, stereo 440 Hz tone
gen() { ffmpeg -hide_banner -loglevel error -y "$@" || { echo "fixture generation failed: $*"; exit 1; }; }
LAVFI="-f lavfi -i testsrc2=size=320x180:rate=30:duration=5 -f lavfi -i sine=frequency=440:duration=5:sample_rate=44100"
# shellcheck disable=SC2086
gen $LAVFI -c:v libx264 -pix_fmt yuv420p -g 15 -c:a aac -ac 2 -shortest "$SRC/in.mp4"
# shellcheck disable=SC2086
gen $LAVFI -c:v libx264 -pix_fmt yuv420p -g 15 -c:a aac -ac 2 -shortest "$SRC/input.mkv"
cp "$SRC/in.mp4" "$SRC/my holiday video.mp4"
cp "$SRC/in.mp4" "$SRC/demo.mov"

reset_run() {
    rm -rf "$RUN"; mkdir -p "$RUN"
    for f in "$SRC"/*; do ln -s "$f" "$RUN/$(basename "$f")"; done
}

probe_v() { ffprobe -v error -select_streams v:0 -show_entries "stream=$1" -of default=nw=1:nk=1 "$2" 2>/dev/null | head -1; }
probe_a() { ffprobe -v error -select_streams a:0 -show_entries "stream=$1" -of default=nw=1:nk=1 "$2" 2>/dev/null | head -1; }
probe_dur() { ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "$1" 2>/dev/null | head -1; }

# check <spec> <outfile> <logfile>  -> prints a reason and returns 1 on failure
check() {
    spec=$1; out=$2; log=$3
    case "$spec" in
    exists)   [ -s "$out" ] || { echo "output '$out' missing or empty"; return 1; } ;;
    dur=*)    want=${spec#dur=}; n=${want%~*}; tol=${want#*~}; got=$(probe_dur "$out")
              awk -v g="${got:-x}" -v n="$n" -v t="$tol" 'BEGIN{ if (g=="x"||g=="N/A") exit 1; d=g-n; if (d<0) d=-d; exit !(d<=t) }' \
                  || { echo "duration $got, want $n~$tol"; return 1; } ;;
    w=*)      got=$(probe_v width "$out");  [ "$got" = "${spec#w=}" ] || { echo "width $got, want ${spec#w=}"; return 1; } ;;
    h=*)      got=$(probe_v height "$out"); [ "$got" = "${spec#h=}" ] || { echo "height $got, want ${spec#h=}"; return 1; } ;;
    fps=*)    got=$(probe_v avg_frame_rate "$out")
              awk -v r="$got" -v w="${spec#fps=}" 'BEGIN{ split(r,a,"/"); if (a[2]==0) exit 1; f=a[1]/a[2]; exit !(f>w-0.5 && f<w+0.5) }' \
                  || { echo "fps $got, want ${spec#fps=}"; return 1; } ;;
    v=*)      got=$(probe_v codec_name "$out"); [ "$got" = "${spec#v=}" ] || { echo "video codec '$got', want ${spec#v=}"; return 1; } ;;
    a=*)      got=$(probe_a codec_name "$out"); [ "$got" = "${spec#a=}" ] || { echo "audio codec '$got', want ${spec#a=}"; return 1; } ;;
    noaudio)  got=$(probe_a codec_name "$out"); [ -z "$got" ] || { echo "has audio ($got), want none"; return 1; } ;;
    novideo)  got=$(probe_v codec_name "$out"); [ -z "$got" ] || { echo "has video ($got), want none"; return 1; } ;;
    files=*)  rest=${spec#files=}; glob=${rest%:*}; range=${rest##*:}; lo=${range%-*}; hi=${range#*-}
              set +f
              # shellcheck disable=SC2086
              cnt=$(ls -1 $glob 2>/dev/null | wc -l | tr -d ' ')
              [ "$cnt" -ge "$lo" ] && [ "$cnt" -le "$hi" ] || { echo "$cnt files match $glob, want $lo-$hi"; return 1; } ;;
    stdout=*) grep -q -- "${spec#stdout=}" "$log" || { echo "output lacks '${spec#stdout=}'"; return 1; } ;;
    *)        echo "unknown check '$spec'"; return 1 ;;
    esac
}

pass=0; fail=0
TAB=$(printf '\t')
while IFS="$TAB" read -r prompt checks; do
    case "$prompt" in ''|'#'*) continue ;; esac
    reset_run
    cmd=$("$NLS" ffmpeg "$prompt" 2>&1) || {
        fail=$((fail+1)); printf 'FAIL (nls error): %s\n  %s\n' "$prompt" "$cmd"; continue; }

    # output file = last shell word of the command
    eval "set -- $cmd"
    out=; for a in "$@"; do out=$a; done

    log="$WORK/log.txt"
    if ! (cd "$RUN" && eval "$cmd") </dev/null >"$log" 2>&1; then
        fail=$((fail+1))
        printf 'FAIL (command exited non-zero): nls ffmpeg "%s"\n  command: %s\n' "$prompt" "$cmd"
        tail -3 "$log" | sed 's/^/  | /'
        continue
    fi

    reason=
    set -f   # don't glob-expand check specs like files=in_*.png here
    for spec in $checks; do
        reason=$(cd "$RUN" && check "$spec" "$out" "$log") || break
        reason=
    done
    set +f
    if [ -z "$reason" ]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        printf 'FAIL: nls ffmpeg "%s"\n  command: %s\n  check:   %s\n' "$prompt" "$cmd" "$reason"
    fi
done < "$DIR/ffmpeg_exec_cases.tsv"
printf 'ffmpeg exec: %d passed, %d failed\n' "$pass" "$fail"
[ $fail -eq 0 ]
