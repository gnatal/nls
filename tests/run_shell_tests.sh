#!/bin/sh
# Tests for the shell integration (nls --shell zsh|bash): an interactive
# shell runs in a pseudo-terminal with the widget loaded, a request is typed,
# Ctrl+G pressed, and the edited line is written to a file by a second key
# binding (Ctrl+T) and compared. Each shell is skipped if it isn't
# installed; bash needs 4.0 or later (macOS ships 3.2).
#
# Cases below: shell<TAB>typed line<TAB>expected line after Ctrl+G
# ("=" means: unchanged, as typed)[<TAB>"undo": press Ctrl+_ after Ctrl+G].
NL=$(cd "$(dirname "${1:-./nls}")" && pwd)/$(basename "${1:-./nls}")
command -v python3 >/dev/null 2>&1 || { echo "shell tests: python3 not installed, skipped"; exit 0; }

W=$(mktemp -d "${TMPDIR:-/tmp}/nls-sh.XXXXXX") || exit 1
trap 'rm -rf "$W"' EXIT
# The widget runs `command nls`: put this build first on PATH as "nls".
mkdir -p "$W/bin" "$W/home"
ln -s "$NL" "$W/bin/nls"

# Drive one interactive shell: load the widget and a dump binding, type the
# line, press Ctrl+G then Ctrl+T, wait for the dump, quit.
drive() {   # shell typed-line dump-file [undo]
    python3 -I -c '
import os, pty, sys, time, select
shell, line, dump, bindir, home, extra = sys.argv[1:7]
env = dict(os.environ, PATH=bindir + ":" + os.environ["PATH"], HOME=home, TERM="dumb",
           PS1="$ ", NLS_KEY="")
env.pop("NLS_KEY")
if shell == "zsh":
    argv = ["zsh", "-f", "-i"]
    init = ("eval \"$(nls --shell zsh)\"; "
            "nls-dump() { print -r -- \"$BUFFER\" > %s; }; zle -N nls-dump; "
            "bindkey \"^T\" nls-dump\n" % dump)
else:
    argv = ["bash", "--norc", "--noprofile", "-i"]
    init = ("eval \"$(nls --shell bash)\"; "
            "bind -x '\''\"\\C-t\": printf \"%%s\\n\" \"$READLINE_LINE\" > %s'\''\n" % dump)
pid, fd = pty.fork()
if pid == 0:
    os.execvpe(argv[0], argv, env)
out = b""
def pump(secs):
    global out
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try: out += os.read(fd, 4096)
            except OSError: return
def send(s, wait=0.3):
    os.write(fd, s.encode()); pump(wait)
pump(0.5)
send(init, 0.8)
send(line, 0.3)
send("\x07", 1.5)          # Ctrl+G: the widget
if extra == "undo":
    send("\x1f", 0.5)      # Ctrl+_: undo
send("\x14", 0.5)          # Ctrl+T: dump the line
for _ in range(40):
    if os.path.exists(dump): break
    pump(0.1)
send("\x15", 0.2)          # Ctrl+U: clear the line
send("exit\n", 0.5)
try: os.kill(pid, 9)
except OSError: pass
os.waitpid(pid, 0)
sys.stdout.write(out.decode("utf-8", "replace"))
' "$1" "$2" "$3" "$W/bin" "$W/home" "${4:-}"
}

pass=0; fail=0; skipped=""
TAB=$(printf '\t')
while IFS="$TAB" read -r shell typed want extra; do
    case "$shell" in ''|'#'*) continue ;; esac
    if ! command -v "$shell" >/dev/null 2>&1; then
        case " $skipped " in *" $shell "*) ;; *) skipped="$skipped $shell" ;; esac
        continue
    fi
    if [ "$shell" = bash ] && ! bash -c '[ "${BASH_VERSINFO[0]}" -ge 4 ]'; then
        case " $skipped " in *" bash "*) ;; *) skipped="$skipped bash" ;; esac
        continue
    fi
    [ "$want" = "=" ] && want=$typed
    rm -f "$W/dump"
    log=$(cd "$W" && drive "$shell" "$typed" "$W/dump" "$extra")
    got=$(cat "$W/dump" 2>/dev/null)
    if [ -f "$W/dump" ] && [ "$got" = "$want" ]; then pass=$((pass+1)); continue; fi
    fail=$((fail+1))
    printf 'FAIL (%s): %s\n  expected: %s\n  actual:   %s\n  terminal:\n%s\n' \
        "$shell" "$typed" "$want" "${got:-(no dump)}" "$(printf '%s' "$log" | tail -n 8 | sed 's/^/    /')"
done <<'EOF'
zsh	cut input.mp4 from 00:10 to 00:25	ffmpeg -ss 00:10 -to 00:25 -i input.mp4 -c copy input_cut.mp4
zsh	git undo last commit	git reset --soft HEAD~1
zsh	nls jq pretty print data.json	jq . data.json
zsh	  nls tar "compress 'my files/' into b.tar.gz"	tar -czvf b.tar.gz 'my files/'
zsh	nls tar 'compress src/ into b.tar.gz'	tar -czvf b.tar.gz src/
zsh	gibberish blah blah	=
zsh	cut input.mp4 from 00:10 to 00:25	=	undo
zsh	nls ffmpeg resize video.mp4	=
bash	cut input.mp4 from 00:10 to 00:25	ffmpeg -ss 00:10 -to 00:25 -i input.mp4 -c copy input_cut.mp4
bash	git undo last commit	git reset --soft HEAD~1
bash	nls jq pretty print data.json	jq . data.json
bash	  nls tar "compress 'my files/' into b.tar.gz"	tar -czvf b.tar.gz 'my files/'
bash	nls tar 'compress src/ into b.tar.gz'	tar -czvf b.tar.gz src/
bash	gibberish blah blah	=
EOF
[ -n "$skipped" ] && echo "shell tests: skipped (not installed, or bash < 4):$skipped"
printf 'shell: %d passed, %d failed\n' "$pass" "$fail"
[ $fail -eq 0 ]
