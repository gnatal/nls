#!/bin/sh
# Execution tests for curl: compile each prompt with nls, run the command
# against a local server (tests/curl_server.py), then run a check command
# and compare its output.
#
# tests/curl_exec_cases.tsv lines:  prompt<TAB>check<TAB>expected
#   prompt     {BASE} is replaced by http://127.0.0.1:<port>
#   check      shell command run after the nls command; $OUT holds the
#              command's own output (stdout)
#   expected   the check's output (whitespace-normalized); "!ERR" means nls
#              itself must refuse the prompt, "!FAIL" that curl must exit
#              non-zero
# Needs curl and python3; skipped if either is missing.
NL=$(cd "$(dirname "${1:-./nls}")" && pwd)/$(basename "${1:-./nls}")
DIR=$(cd "$(dirname "$0")" && pwd)

for tool in curl python3; do
    if ! command -v $tool >/dev/null 2>&1; then
        echo "$tool not installed; skipping curl execution tests"
        exit 0
    fi
done

W=$(mktemp -d "${TMPDIR:-/tmp}/nls-curl.XXXXXX") || exit 1
python3 -I "$DIR/curl_server.py" > "$W/port" &
SRV=$!
trap 'kill $SRV 2>/dev/null; wait $SRV 2>/dev/null; rm -rf "$W"' EXIT
i=0
while [ ! -s "$W/port" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
PORT=$(cat "$W/port")
[ -n "$PORT" ] || { echo "curl exec: test server didn't start"; exit 1; }
BASE="http://127.0.0.1:$PORT"

normalize() { tr '\n' ' ' | tr -s ' \t' ' ' | sed 's/^ //; s/ $//'; }

make_tree() {
    cd / && rm -rf "$W/t" && mkdir -p "$W/t/downloads" && cd "$W/t" || exit 1
    printf '{"name": "bob"}\n' > data.json
    printf 'id,name\n1,bob\n' > users.csv
    printf 'PNG' > photo.png
    printf 'txt' > notes.txt
    printf '0123456789012345' > part.bin   # a download interrupted at 16 bytes
}

pass=0; fail=0
TAB=$(printf '\t')
while IFS="$TAB" read -r prompt check expected; do
    case "$prompt" in ''|'#'*) continue ;; esac
    case "$prompt" in *json\ from*) command -v jq >/dev/null 2>&1 || continue ;; esac
    prompt=$(printf '%s' "$prompt" | sed "s|{BASE}|$BASE|g")
    make_tree
    cmd=$("$NL" curl "$prompt" 2>/dev/null); rc=$?; [ $rc -ne 0 ] && cmd=$("$NL" curl "$prompt" 2>&1)
    if [ "$expected" = "!ERR" ]; then
        if [ $rc -ne 0 ]; then pass=$((pass+1)); else
            fail=$((fail+1)); printf 'FAIL: nls curl "%s" should be refused, got: %s\n' "$prompt" "$cmd"; fi
        continue
    fi
    if [ $rc -ne 0 ]; then
        fail=$((fail+1)); printf 'FAIL (nls error): %s\n  %s\n' "$prompt" "$cmd"; continue
    fi
    OUT=$(TOKEN=s3cret API_TOKEN=t0ken eval "$cmd" 2>/dev/null); crc=$?
    export OUT
    if [ "$expected" = "!FAIL" ]; then
        if [ $crc -ne 0 ]; then pass=$((pass+1)); else
            fail=$((fail+1)); printf 'FAIL: "%s" should make curl fail\n  command: %s\n' "$prompt" "$cmd"; fi
        continue
    fi
    if [ $crc -ne 0 ]; then
        fail=$((fail+1)); printf 'FAIL (curl exit %d): %s\n  command: %s\n' "$crc" "$prompt" "$cmd"
        continue
    fi
    actual=$(eval "$check" 2>&1 | normalize)
    if [ "$actual" = "$expected" ]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        printf 'FAIL: nls curl "%s"\n  command:  %s\n  check:    %s\n  expected: %s\n  actual:   %s\n' \
            "$prompt" "$cmd" "$check" "$expected" "$actual"
    fi
done < "$DIR/curl_exec_cases.tsv"
printf 'curl exec: %d passed, %d failed\n' "$pass" "$fail"
[ $fail -eq 0 ]
