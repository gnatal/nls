#!/bin/sh
# Tests for --listen (src/voice.c). Nothing is recorded or transcribed for
# real: NLS_RECORD writes a placeholder file and NLS_STT prints a fixed
# transcript, so these check what nls does with what was "heard":
#   - the transcript is normalized (numbers, "dot", "slash", fillers, ...)
#     and goes through the same steps as a typed request;
#   - the failures say what is wrong (no engine, recorder or STT failing,
#     silence) and leave no temporary files;
#   - -x always asks for a spoken request, even with -y --force;
#   - in a terminal, a key stops the recording and Ctrl+C aborts
#     (python3, in a pseudo-terminal; skipped without it).
NL=$(cd "$(dirname "${1:-./nls}")" && pwd)/$(basename "${1:-./nls}")
W=$(mktemp -d "${TMPDIR:-/tmp}/nls-voice-t.XXXXXX") || exit 1
trap 'rm -rf "$W"' EXIT
export TMPDIR="$W/tmp"
mkdir -p "$TMPDIR"
export NLS_RECORD='printf x > "$NLS_WAV"'
unset NLS_LISTEN_SECONDS NLS_WHISPER_MODEL
pass=0; fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); printf 'FAIL: %s\n' "$*"; }

# ---- what was heard, and the command ----
# transcript<TAB>tool ("-" for none)<TAB>"nls: heard: ..." line<TAB>command (!ERR: fails)
TAB=$(printf '\t')
while IFS="$TAB" read -r said tool heard want; do
    case "$said" in ''|'#'*) continue ;; esac
    [ "$tool" = "-" ] && tool=
    out=$(printf '%s\n' "$said" > "$W/said"; NLS_STT='cat "$SAID"' SAID="$W/said" \
          "$NL" --listen $tool 2>"$W/err" </dev/null)
    rc=$?
    got_heard=$(sed -n 's/^nls: heard: //p' "$W/err")
    if [ "$got_heard" != "$heard" ]; then
        bad "'$said': heard '$got_heard', expected '$heard'"
    elif [ "$want" = "!ERR" ]; then
        if [ $rc -ne 0 ]; then ok; else bad "'$said': expected an error, got '$out'"; fi
    elif [ $rc -eq 0 ] && [ "$out" = "$want" ]; then ok
    else bad "'$said': got '$out' (exit $rc), expected '$want'"; fi
done <<'EOF'
Undo the last two commits, keep changes staged.	-	undo the last 2 commits, keep changes staged	git reset --soft HEAD~2
Cut input dot mp4 from 00:10 to 00:25 and scale to 720p.	-	cut input.mp4 from 00:10 to 00:25 and scale to 720p	ffmpeg -ss 00:10 -to 00:25 -i input.mp4 -vf scale=-2:720 -c:a copy input_cut.mp4
Get dot name from users dot JSON where dot age is greater than thirty.	-	get .name from users.json where .age is greater than 30	jq 'map(select(.age > 30) | .name)' users.json
Extract backup dot tar dot gz into restore slash.	-	extract backup.tar.gz into restore/	tar -xzvf backup.tar.gz -C restore/
Git, undo the last commit.	-	git undo the last commit	git reset --soft HEAD~1
Um, find files larger than one hundred megabytes modified in the last seven days.	-	find files larger than 100 megabytes modified in the last 7 days	find . -type f -size +100M -mtime -7
Search src slash for TODO.	-	search src/ for TODO	grep -rnE -e TODO src/
Pretty print data.json	jq	pretty print data.json	jq . data.json
[BLANK_AUDIO] Pretty print data.json (upbeat music)	jq	pretty print data.json	jq . data.json
Docker, list containers.	-	docker list containers	!ERR
EOF

# Normalization alone, through a request no tool takes (the heard line is
# printed before the tool is inferred).
while IFS="$TAB" read -r said heard; do
    case "$said" in ''|'#'*) continue ;; esac
    printf '%s\n' "$said" > "$W/said"
    NLS_STT='cat "$SAID"' SAID="$W/said" "$NL" --listen >/dev/null 2>"$W/err" </dev/null
    got=$(sed -n 's/^nls: heard: //p' "$W/err")
    if [ "$got" = "$heard" ]; then ok; else bad "normalize '$said': got '$got', expected '$heard'"; fi
done <<'EOF'
twenty-five and two hundred and fifty	25 and 250
three thousand four hundred one, nineteen	3401, 19
five five	5 5
one point five	1.5
in slash tmp	in /tmp
tilde slash notes	~/notes
my underscore file dash two	my_file-2
count dot items in data dot json	count .items in data.json
Users dot json	users.json
EOF

# ---- failures ----
expect() {   # description, expected exit, stderr pattern, then the command
    d=$1 want=$2 pat=$3; shift 3
    "$@" >"$W/out" 2>"$W/err" </dev/null
    rc=$?
    if [ $rc -ne "$want" ]; then bad "$d: exit $rc, expected $want ($(cat "$W/err"))"
    elif ! grep -qF -e "$pat" "$W/err"; then bad "$d: stderr lacks '$pat': $(cat "$W/err")"
    else ok; fi
}
expect "a request with --listen" 2 "only a tool may follow" \
    env NLS_STT=true "$NL" --listen "cut a.mp4"
expect "no whisper" 2 "whisper-cli isn't installed" \
    env -u NLS_STT PATH=/usr/bin:/bin "$NL" --listen
expect "no model" 2 "can't be read" \
    env -u NLS_STT NLS_WHISPER_MODEL="$W/missing.bin" "$NL" --listen
expect "the recorder fails" 1 "recording failed (exit 3)" \
    env NLS_RECORD='echo no mic >&2; exit 3' NLS_STT=true "$NL" --listen
grep -qF "no mic" "$W/err" && ok || bad "the recorder's error isn't shown"
expect "nothing recorded" 1 "nothing was recorded" \
    env NLS_RECORD=true NLS_STT=true "$NL" --listen
expect "the transcription fails" 1 "transcription failed (exit 4)" \
    env NLS_STT='echo bad model >&2; exit 4' "$NL" --listen
grep -qF "bad model" "$W/err" && ok || bad "the transcriber's error isn't shown"
expect "silence" 1 "didn't hear anything" \
    env NLS_STT='echo "[BLANK_AUDIO]"' "$NL" --listen
expect "a bad NLS_LISTEN_SECONDS" 2 "NLS_LISTEN_SECONDS" \
    env NLS_LISTEN_SECONDS=0 NLS_STT=true "$NL" --listen
expect "the time limit" 1 "nothing was recorded" \
    env NLS_LISTEN_SECONDS=1 NLS_RECORD='sleep 30' NLS_STT=true "$NL" --listen
[ -z "$(ls -A "$TMPDIR")" ] && ok || bad "temporary files left: $(ls -A "$TMPDIR")"

# ---- -x asks first ----
if command -v python3 >/dev/null 2>&1; then
    notty() {   # as in run_x_tests.sh: no terminal, so the answer comes from stdin
        python3 -I -c '
import os, sys
if os.getpgrp() == os.getpid():
    pid = os.fork()
    if pid:
        st = os.waitpid(pid, 0)[1]
        sys.exit(os.WEXITSTATUS(st) if os.WIFEXITED(st) else 128 + os.WTERMSIG(st))
os.setsid()
os.execvp(sys.argv[1], sys.argv[1:])' "$@"
    }
    mkdir -p "$W/run" && printf '{"a": 1}\n' > "$W/run/data.json"
    for flags in "-x" "-x -y" "-x -y --force"; do
        (cd "$W/run" && printf 'n\n' | NLS_STT='echo pretty print data.json' \
            notty "$NL" --listen $flags jq) >"$W/out" 2>"$W/err"
        rc=$?
        if [ $rc -eq 130 ] && grep -qF '[Enter to run' "$W/err" && [ ! -s "$W/out" ]; then ok
        else bad "--listen $flags: exit $rc, ran without asking? $(cat "$W/err")"; fi
    done
    grep -qF "not running a spoken request without confirmation (-y ignored)" "$W/err" && ok ||
        bad "--listen -x -y doesn't say why it asks"
    # find, not jq: it is there everywhere
    (cd "$W/run" && printf '\n' | NLS_STT='echo list the json files' \
        notty "$NL" --listen -x find) >"$W/out" 2>"$W/err"
    if [ $? -eq 0 ] && grep -qxF './data.json' "$W/out"; then ok
    else bad "--listen -x: Enter doesn't run it: $(cat "$W/err")"; fi

    # ---- in a terminal: a key stops the recording, Ctrl+C aborts ----
    pty_run() {   # key to send after half a second; prints exit status, then output
        python3 -I -c '
import os, pty, sys, time, select
key = sys.argv[1].encode().decode("unicode_escape").encode()
pid, fd = pty.fork()
if pid == 0:
    os.execv(sys.argv[2], sys.argv[2:])
out = b""
def pump(secs):
    global out
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try: out += os.read(fd, 4096)
            except OSError: return
pump(0.5)
os.write(fd, key)
start = time.time()
while time.time() - start < 8:
    pump(0.1)
    p, st = os.waitpid(pid, os.WNOHANG)
    if p: break
else:
    os.kill(pid, 9); p, st = os.waitpid(pid, 0)
pump(0.2)
print(os.WEXITSTATUS(st) if os.WIFEXITED(st) else 128 + os.WTERMSIG(st))
print("%.1f" % (time.time() - start))
sys.stdout.write(out.decode("utf-8", "replace"))' "$@"
    }
    # records until SIGINT, then writes the file (as sox, arecord and ffmpeg do)
    export NLS_RECORD='trap '\''printf x > "$NLS_WAV"; exit 0'\'' INT; while :; do sleep 0.1; done'
    export NLS_STT='echo pretty print data.json' NLS_LISTEN_SECONDS=30
    res=$(pty_run '\n' "$NL" --listen jq)
    rc=$(printf '%s\n' "$res" | sed -n 1p) secs=$(printf '%s\n' "$res" | sed -n 2p)
    if [ "$rc" = 0 ] && printf '%s' "$res" | grep -qF 'jq . data.json' &&
       printf '%s' "$res" | grep -qF 'press Enter to stop'; then ok
    else bad "Enter doesn't stop the recording: $res"; fi
    case $secs in [0-2].*) ok ;; *) bad "stopping took $secs s" ;; esac
    res=$(pty_run '\003' "$NL" --listen jq)
    rc=$(printf '%s\n' "$res" | sed -n 1p)
    if [ "$rc" = 130 ] && printf '%s' "$res" | grep -qF 'nls: aborted' &&
       ! printf '%s' "$res" | grep -qF 'jq . data.json'; then ok
    else bad "Ctrl+C doesn't abort: $res"; fi
    [ -z "$(ls -A "$TMPDIR")" ] && ok || bad "temporary files left after Ctrl+C: $(ls -A "$TMPDIR")"
else
    echo "voice tests: python3 not installed, -x and terminal cases skipped"
fi

printf 'voice: %d passed, %d failed\n' "$pass" "$fail"
[ $fail -eq 0 ]
