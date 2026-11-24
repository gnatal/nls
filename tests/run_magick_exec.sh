#!/bin/sh
# Execution tests for magick: compile each prompt with nls, run the command
# on small generated images, and verify the result with `magick identify`.
#
# No images are stored in the repo: fixtures are drawn with magick itself
# into a temp dir on each run.
#
# tests/magick_exec_cases.tsv lines:  prompt<TAB>checks
# Checks (space-separated, all must hold) apply to the command's output file
# (its last argument, or out=PATH):
#   out=PATH        inspect PATH instead (batch commands, folders)
#   exists          output file exists and is non-empty
#   w=N  h=N        width / height of the first frame
#   fmt=FMT         magick format of the first frame (PNG, JPEG, WEBP, GIF, ICO, ...)
#   frames=N        number of frames / sub-images
#   alpha opaque    has (no) transparent pixels
#   gray            colour space is Gray
#   noexif          no EXIF properties left
#   px=X,Y:COLOR    pixel colour, roughly: red green blue white black transparent
#   files=GLOB:MIN-MAX  number of files matching GLOB
#   stdout=TEXT     the command's combined output contains TEXT
NLS=$(cd "$(dirname "${1:-./nls}")" && pwd)/$(basename "${1:-./nls}")
DIR=$(cd "$(dirname "$0")" && pwd)

if ! command -v magick >/dev/null 2>&1; then
    echo "ImageMagick 7 (magick) not installed; skipping magick execution tests (brew install imagemagick)"
    exit 0
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/nls-magick.XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT INT TERM
SRC="$WORK/src"; RUN="$WORK/run"
mkdir -p "$SRC/frames" "$SRC/assets" "$SRC/photos" "$RUN"

gen() { magick "$@" || { echo "fixture generation failed: magick $*"; exit 1; }; }
# 400x200, red at the top fading to blue at the bottom
gen -size 400x200 gradient:red-blue "$SRC/hero.png"
# 300x200, red left half, blue right half
gen -size 150x200 xc:red -size 150x200 xc:blue +append "$SRC/photo.jpg"
# same, tagged with EXIF orientation 6 ("rotate 90 CW to display"). magick only
# writes the tag into an existing EXIF profile, so attach a minimal one first.
printf 'Exif\000\000MM\000\052\000\000\000\010\000\001\001\022\000\003\000\000\000\001\000\006\000\000\000\000\000\000' > "$WORK/orient.app1"
gen "$SRC/photo.jpg" -profile "$WORK/orient.app1" -orient RightTop "$SRC/rotated.jpg"
# 64x64 red disc on white
gen -size 64x64 xc:white -fill red -draw 'circle 32,32 32,10' "$SRC/sprite.png"
# 100x100 transparent with a 40x40 blue square in the middle
gen -size 100x100 xc:none -fill blue -draw 'rectangle 30,30 69,69' "$SRC/logo.png"
gen -size 40x30 xc:red "$SRC/a.png"
gen -size 40x30 xc:lime "$SRC/b.png"
gen -size 40x30 xc:blue "$SRC/c.png"
for n in 1 2 3; do gen -size 50x50 "xc:gray$((n * 30))" "$SRC/frames/f$n.png"; done
gen -size 40x40 xc:red "$SRC/assets/x.png"
gen -size 40x40 xc:blue "$SRC/assets/y.png"
gen -size 400x300 xc:red "$SRC/photos/p1.jpg"
gen -size 400x300 xc:blue "$SRC/photos/p2.jpg"
cp "$SRC/hero.png" "$SRC/my photo.png"

reset_run() {
    rm -rf "$RUN"; mkdir -p "$RUN"
    cp -R "$SRC"/. "$RUN"/
}

ident() { magick identify -format "$1" "$2[0]" 2>/dev/null; }

# px_color <file> <x> <y> -> red green blue white black transparent other
# (an image without an alpha channel reports alpha 0, hence the %A test)
px_color() {
    magick "$1[0]" -format "%[fx:int(255*p{$2,$3}.r)] %[fx:int(255*p{$2,$3}.g)] %[fx:int(255*p{$2,$3}.b)] %[fx:int(255*p{$2,$3}.a)] %A" info: 2>/dev/null |
    awk '{ r=$1; g=$2; b=$3; a=($5 == "Undefined" || $5 == "False") ? 255 : $4
           if (a < 128) print "transparent"
           else if (r > 200 && g > 200 && b > 200) print "white"
           else if (r < 60 && g < 60 && b < 60) print "black"
           else if (r > 150 && g < 100 && b < 100) print "red"
           else if (g > 150 && r < 100 && b < 100) print "green"
           else if (b > 150 && r < 100 && g < 100) print "blue"
           else print "other(" r "," g "," b "," a ")" }'
}

# check <spec> <outfile> <logfile>  -> prints a reason and returns 1 on failure
check() {
    spec=$1; out=$2; log=$3
    case "$spec" in
    out=*)    ;;
    exists)   [ -s "$out" ] || { echo "output '$out' missing or empty"; return 1; } ;;
    w=*)      got=$(ident '%w' "$out"); [ "$got" = "${spec#w=}" ] || { echo "width $got, want ${spec#w=}"; return 1; } ;;
    h=*)      got=$(ident '%h' "$out"); [ "$got" = "${spec#h=}" ] || { echo "height $got, want ${spec#h=}"; return 1; } ;;
    fmt=*)    got=$(ident '%m' "$out"); [ "$got" = "${spec#fmt=}" ] || { echo "format '$got', want ${spec#fmt=}"; return 1; } ;;
    frames=*) got=$(magick identify "$out" 2>/dev/null | wc -l | tr -d ' ')
              [ "$got" = "${spec#frames=}" ] || { echo "$got frames, want ${spec#frames=}"; return 1; } ;;
    alpha)    got=$(ident '%[opaque]' "$out"); [ "$got" = "False" ] || { echo "no transparent pixels (opaque=$got)"; return 1; } ;;
    opaque)   got=$(ident '%[opaque]' "$out"); [ "$got" = "True" ] || { echo "has transparent pixels (opaque=$got)"; return 1; } ;;
    gray)     got=$(ident '%[colorspace]' "$out"); [ "$got" = "Gray" ] || { echo "colour space $got, want Gray"; return 1; } ;;
    noexif)   got=$(magick identify -format '%[EXIF:*]' "$out[0]" 2>/dev/null)
              [ -z "$got" ] || { echo "EXIF still present: $(echo "$got" | head -2 | tr '\n' ' ')"; return 1; } ;;
    px=*)     rest=${spec#px=}; xy=${rest%%:*}; want=${rest#*:}
              got=$(px_color "$out" "${xy%,*}" "${xy#*,}")
              [ "$got" = "$want" ] || { echo "pixel $xy is $got, want $want"; return 1; } ;;
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
    cmd=$("$NLS" magick "$prompt" 2>/dev/null) || {
        cmd=$("$NLS" magick "$prompt" 2>&1); fail=$((fail+1)); printf 'FAIL (nls error): %s\n  %s\n' "$prompt" "$cmd"; continue; }

    # output file = last shell word of the command (after any "mkdir -p x &&"),
    # unless out= says otherwise. Only the final segment is parsed: eval'ing
    # "set -- a && b" would run b here.
    set -f
    last=${cmd##*&& }
    eval "set -- $last"
    out=; for a in "$@"; do out=$a; done
    for spec in $checks; do case "$spec" in out=*) out=${spec#out=} ;; esac; done
    set +f

    log="$WORK/log.txt"
    if ! (cd "$RUN" && eval "$cmd") </dev/null >"$log" 2>&1; then
        fail=$((fail+1))
        printf 'FAIL (command exited non-zero): nls magick "%s"\n  command: %s\n' "$prompt" "$cmd"
        tail -3 "$log" | sed 's/^/  | /'
        continue
    fi

    reason=
    set -f   # don't glob-expand check specs like files=assets/*.webp here
    for spec in $checks; do
        reason=$(cd "$RUN" && check "$spec" "$out" "$log") || break
        reason=
    done
    set +f
    if [ -z "$reason" ]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        printf 'FAIL: nls magick "%s"\n  command: %s\n  check:   %s\n' "$prompt" "$cmd" "$reason"
    fi
done < "$DIR/magick_exec_cases.tsv"
printf 'magick exec: %d passed, %d failed\n' "$pass" "$fail"
[ $fail -eq 0 ]
