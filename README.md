# nls — natural language shell

`nls` turns plain-English requests into exact `ffmpeg`, `jq`, `grep`,
`find`, `tar`, `git`, `curl` and ImageMagick (`magick`) commands.
It works fully offline: no LLM, no network, no runtime dependencies. It is
written in C11 using only the standard library, and every command template is
compiled into the binary.

```console
$ nls ffmpeg "cut input.mp4 from 00:10 to 00:25 and scale to 720p"
ffmpeg -ss 00:10 -to 00:25 -i input.mp4 -vf scale=-2:720 -c:a copy input_cut.mp4

$ nls jq "get .name from users.json where .age > 30"
jq 'map(select(.age > 30) | .name)' users.json

$ nls "extract backup.tar.gz into restore/"        # the tool can be left out
tar -xzvf backup.tar.gz -C restore/
```

`nls --explain` breaks a command down part by part, and a key binding turns
the request you typed on the command line into the command, ready to edit
(see [Usage](#usage)).

Supported tools: **ffmpeg**, **jq**, **grep**, **find**, **tar**, **git**, **curl** and
**magick** (also reachable as `nls convert …`)
(see `natural_language_shell_architecture_plan.md`).

## Install

Prebuilt binaries (Linux x86_64/arm64, macOS universal) are attached to each
[GitHub release](https://github.com/gnatal/nls/releases). To download, verify
and install the latest one into `~/.local/bin`:

```sh
curl -fsSL https://raw.githubusercontent.com/gnatal/nls/main/install.sh | sh
```

Set `NLS_VERSION=0.1.0` to pick a release, or `PREFIX=/usr/local` to install
system-wide. You can also build from source (below): it's one `make` with no
dependencies. Both install the man page (`man nls`) and bash/zsh
completions too; see [Completions and man page](#completions-and-man-page).

## Build

```sh
make          # cc -std=c11 -O2 -Wall -Wextra -pedantic -Werror
make test     # all tests (each execution suite skips if its tool is missing):
              #  - command-string goldens        tests/cases.tsv
              #  - -x: answers at the prompt, dry runs
              #    and --force, in a temp git repo      tests/x_cases.tsv
              #  - tool inference                       tests/infer_cases.tsv
              #  - paraphrases: varied wordings must
              #    keep picking their template          tests/paraphrase_cases.tsv
              #  - --explain: every golden command and
              #    template example fully explained     tests/explain_cases.tsv
              #  - shell integration, typed into zsh / bash
              #    in a pseudo-terminal                 tests/run_shell_tests.sh
              #  - completions and man page             tests/run_completion_tests.sh
              #  - jq: run on tests/fixtures/*.json     tests/jq_exec_cases.tsv
              #  - grep: run on tests/fixtures/grep/    tests/grep_exec_cases.tsv
              #  - find: run on a generated temp tree   tests/find_exec_cases.tsv
              #  - tar: create/extract/list in a temp tree  tests/tar_exec_cases.tsv
              #  - ffmpeg: run on generated media,
              #    verified with ffprobe          tests/ffmpeg_exec_cases.tsv
              #  - magick: run on generated images,
              #    verified with magick identify  tests/magick_exec_cases.tsv
make test-ffmpeg  # only the ffmpeg execution tests (~10 s)
make test-magick  # only the magick execution tests
make test-paraphrase  # only the paraphrase tests
make test-sanitize    # every suite against an AddressSanitizer + UBSan build
make fuzz-check   # fuzz the lexer and expanders: replay the corpus, then
                  # 200k random mutations, under ASan + UBSan (any compiler)
make fuzz         # coverage-guided libFuzzer run for FUZZ_TIME=60 seconds
                  # (needs clang with libFuzzer: Linux, or Homebrew llvm via
                  # FUZZ_CC=$(brew --prefix llvm)/bin/clang)
make wasm-docker  # rebuild the landing page's playground (docs/wasm/) with
                  # Emscripten in Docker (`make wasm` if emcc is installed)
make test-wasm    # that build must print what ./nls prints (needs node)
make install  # installs to ~/.local/bin/nls (no sudo needed), plus the man
              # page and completions under ~/.local/share
```

CI (`.github/workflows/ci.yml`) runs `make test` on Linux and macOS for every
push and pull request, plus `make test-sanitize`, a two-minute `make fuzz` and
`make test-wasm`.

The fuzzer (`tests/fuzz/fuzz_nls.c`) feeds each input through the lexer,
matcher, every expandable template, its dry run and `--explain`. Besides
crashes and undefined behaviour, it fails when a generated command has
unbalanced quotes, a control character, or an unquoted shell character
(`;`, `|`, `$(`, a backquote, …) that the template didn't put there: a way
for the request to inject a command. A failing input is saved
(`build/crash-*`, or `crash-input` for `fuzz-check`); once fixed, add it to
`tests/fuzz/regressions/` so it is replayed every run.

The install step prints a one-line fix if `~/.local/bin` isn't on your `PATH`,
and a warning if another `nls` comes first on it (an older copy keeps the old
behaviour). Each build reports its commit (`nls --version` →
`nls 0.1.0+16.g2827e3e`), so you can compare that copy with your checkout
(`make build-version`). Options:

```sh
make install BIN_NAME=other        # install under a different command name
make install PREFIX=/usr/local     # system-wide; may need sudo
make uninstall                     # same variables apply
```

The command is called `nls` (natural language shell). The shorter `nl` is
already taken by the standard Unix line-numbering tool.

The ffmpeg tests don't need any media in the repo. Each run generates 5-second
320x180 clips with ffmpeg's built-in test pattern and tone in a temp folder,
runs every command on them, and checks the output's duration, resolution,
codecs, audio/video streams, fps and frame count with `ffprobe`. The magick
tests work the same way: they draw small images with `magick` itself (a
gradient, a red/blue split, a sprite on white, a logo on transparency, one
photo tagged with an EXIF rotation) and check size, format, frame count,
transparency, colour space, EXIF and individual pixel colours.

## Usage

```
nls [options] [tool] "<what you want>"

  -x, --exec        run the command (shows it, asks for confirmation)
  -y, --yes         with -x: skip confirmation (not for uncertain matches
                    or destructive commands)
      --force       with -x -y: skip it for destructive commands too
  -e, --explain     explain the command part by part
  -v, --verbose     show extracted slots, tokens and template scores,
                    and a destructive command's dry run
  -l, --list [tool] list all templates with example prompts
      --shell <zsh|bash>
                    print the key binding (see Shell integration)
```

By default `nls` only **prints** the command. To run it, use `-x`, or
`eval "$(nls ...)"`:

```sh
curl -s https://api.example.com/users | eval "$(nls jq 'list .email of each user')"
```

Quote the whole prompt so your shell doesn't interpret `>`, `'` or `*`.
`man nls` has the full reference.

### Without a tool

The tool can be left out. nls then compiles the request for every tool and
uses the one that fits best:

```console
$ nls "convert demo.mov to gif"
ffmpeg -i demo.mov -vf 'fps=10,scale=480:-1:flags=lanczos,split[s0][s1];[s0]palettegen[p];[s1][p]paletteuse' -loop 0 demo.gif
$ nls "which files mention deprecated in src/"
grep -rlE -e deprecated src/
$ nls "files bigger than 500MB in ~/Downloads"
find ~/Downloads -type f -size +500M
```

A tool's score is its best template's keyword score plus points for what
the request names: a media file, an image, a JSON file or path, an archive
(`.tar.gz`), a URL, git words (`commit`, `branch`, `stash`, a tag such as
`v1.0`), search words (`containing`, `mentioning`), file tests (`named`,
`larger`, `modified`) or the tool's own name (`use jq to …`). jq, ffmpeg,
magick, git and curl only count when the request names something of
theirs: their keywords are too generic otherwise ("merge feature-x" is not
jq's slurp). `nls -v` shows every tool's score.

- If another tool fits about as well and gives a different command, nls
  prints its pick and shows the runner-up's command on stderr. `-y` then no
  longer skips the confirmation.
- If nothing points at a tool ("go back to main"), nls refuses rather than
  guess. Name the tool then: `nls git "go back to main"`.
- If the best tool knows what you mean but is missing something
  ("cut video.mp4"), it says what (here: the timestamps).
- A first word naming a tool nls doesn't support (`rsync`, `openssl`,
  `docker`, …) is refused, not read as part of a request.
- The tool can also be the first word of a single-argument request
  (`nls "git undo last commit"`, as the shell integration sends it), except
  `find` and `convert`, which are read as verbs there:
  `nls "find TODO in src"` is a grep.

When this was written, 465 of the 471 golden prompts in `tests/cases.tsv`
gave the same command without their tool. Five are refused as unclear ("what changed
yesterday", "what haven't I pulled"), and one is reported as a close call between grep and find.
The tests require that a command printed without a warning is always the
golden one.

### Explain a command

`-e` / `--explain` prints the command, then what each part does:

```console
$ nls --explain find "5 biggest files ignoring .git"
find . -path '*/.git' -prune -o -type f -exec du -h {} + | sort -rh | head -n 5

  find                  walk folders and select files by name, type, size or
                        age
  .                     start searching in the current folder
  -path '*/.git'        whose path matches */.git
  -prune                don't go inside it
  -o                    or (what follows applies to everything else)
  -type f               only regular files
  -exec                 run this on the files found, as many at a time as fit:
    du                    show how much disk space each file uses
    -h                    sizes in K, M, G
    {}                    the path(s) found
    +                     end of the command (files in batches)

  |                     send the output to the next command:
  sort                  sort lines
  -rh                   short for -r -h:
    -r                    largest first
    -h                    compare sizes like 2K < 1M

  |                     send the output to the next command:
  head                  keep the first lines
  -n 5                  keep the first 5 line(s)
```

Values with a structure of their own are decoded too. A jq program is
explained stage by stage, with `--arg` values filled in ("for each element:
keep it only if .status == "active", then take .name"). ffmpeg filter
chains (`scale=-2:720` → "resize to 720 px high"), ImageMagick geometries
(`'1920x1080>'` → "to fit within 1920x1080 (keeps the aspect ratio), only
if it is larger"), find sizes
and ages, tar's bundled letters (`-czvf`) and git revisions (`HEAD~3`,
`main..HEAD`, `'HEAD@{1}'`) are decoded as well. With `-x`, the explanation
goes to stderr with the command, before the question. The tests check that
every command nls can generate is explained in full.

### Shell integration

```sh
eval "$(nls --shell zsh)"     # in ~/.zshrc
eval "$(nls --shell bash)"    # in ~/.bashrc (bash 4.0 or later)
```

Type a request on the command line and press **Ctrl+G**. The line is
replaced by the command, so you can read and edit it before pressing Enter:

```console
$ cut input.mp4 from 00:10 to 00:25          ← typed, then Ctrl+G
$ ffmpeg -ss 00:10 -to 00:25 -i input.mp4 -c copy input_cut.mp4
```

- A leading `nls` on the line is ignored, so `nls git undo last commit`
  works too, and so does a request quoted as a whole.
- nls's messages (an uncertain match, what is missing) are shown under the
  line in zsh and above it in bash. When no command can be made, the line
  stays as typed.
- In zsh, undo (Ctrl+_) brings the request back.
- To use another key, set `NLS_KEY` before the `eval`: `NLS_KEY='^X^N'`
  (zsh `bindkey` syntax) or `NLS_KEY='\C-x\C-n'` (readline syntax, bash).

The widget only edits the line. Nothing runs until you press Enter.

### Completions and man page

`make install` and `install.sh` also install:

| File | Installed as |
|---|---|
| `man/nls.1` | `$PREFIX/share/man/man1/nls.1` |
| `completions/nls.bash` | `$PREFIX/share/bash-completion/completions/nls` |
| `completions/_nls` | `$PREFIX/share/zsh/site-functions/_nls` |

The completions offer the options, the tools (first argument, and after
`--list`), `zsh`/`bash` after `--shell`, and file names inside the request.
bash-completion finds the file on its own with the default prefix
(`~/.local`). zsh needs the folder on `$fpath` before `compinit`:

```sh
fpath=(~/.local/share/zsh/site-functions $fpath)   # in ~/.zshrc, before compinit
```

The completions are registered for the name `nls`. With
`make install BIN_NAME=other`, they are installed under the new name but
still complete `nls`.

Tool inference, Tab completion and the Ctrl+G widget in action:

![nls without a tool, Tab completion and the Ctrl+G key binding](docs/nls-infer-completion.gif)

The GIF is recorded with [VHS](https://github.com/charmbracelet/vhs) from
`marketing/infer-completion.tape`.

### Destructive commands

Commands that delete or overwrite data, or rewrite a remote, are tagged as
destructive, and `nls --list` marks them. They are: deleting files with
find, `git reset --hard`, `git restore`, `git clean`, `git branch -D`,
`git push --delete`, force pushes and `curl -X DELETE`. With `-x`, nls says
what the command destroys and, where the tool allows it, runs a dry run
first. Only a typed `y` runs it; Enter aborts.

```console
$ nls -x find "delete .DS_Store files recursively"
→ find . -type f -name .DS_Store -delete
nls: destructive: deletes the matching files for good
nls: dry run: find . -type f -name .DS_Store -print
  ./.DS_Store
  ./src/.DS_Store
[type y to run / Enter to abort] y
```

| Command | Dry run |
|---|---|
| `find … -delete`, `find … -exec rm -rf {} +` | `find … -print` |
| `git clean -fd`, `git clean -fdx` | `git clean -nd`, `git clean -ndx` |
| `git push origin --delete x`, `git push --force-with-lease` | the same command with `--dry-run` |
| `git reset --hard X`, `git restore [--source=X] -- files` | `git diff --stat [X] [-- files]` |
| `git branch -D`, `curl -X DELETE` | none: only the warning and the question |

The dry run changes nothing. Its output goes to stderr, and only the first
20 lines are shown. If it fails (not a git repository, no remote, a folder
find can't read), the command isn't run. Deleting empty directories also
removes parents left empty along the way, which the dry run can't list.

`-y` doesn't skip this question. `-y --force` does, and runs the command
without a dry run, but even `--force` never skips the question for an
uncertain match. To see the dry run without running anything, use `nls -v`.

## Examples

| Prompt | Command |
|---|---|
| `ffmpeg "convert demo.mov to gif at 12fps width 320"` | `ffmpeg -i demo.mov -vf 'fps=12,scale=320:-2:…paletteuse' -loop 0 demo.gif` |
| `ffmpeg "keep the last 10s of in.mp4"` | `ffmpeg -sseof -10 -i in.mp4 -c copy in_cut.mp4` |
| `ffmpeg "extract audio from video.mp4 as mp3 at 320k"` | `ffmpeg -i video.mp4 -vn -c:a libmp3lame -b:a 320k video.mp3` |
| `ffmpeg "compress video.mkv to 720p"` | `ffmpeg -i video.mkv -vf scale=-2:720 -c:v libx264 -crf 28 … video_compressed.mp4` |
| `ffmpeg "remove audio from video.mp4"` | `ffmpeg -i video.mp4 -c:v copy -an video_muted.mp4` |
| `jq "users.json where .status is active"` | `jq --arg v active 'map(select(.status == $v))' users.json` |
| `jq "users.json where .age > 30 and .status is active"` | `jq --arg v2 active 'map(select((.age > 30) and (.status == $v2)))' users.json` |
| `jq "users.json where .age between 20 and 40"` | `jq 'map(select(.age >= 20 and .age <= 40))' users.json` |
| `jq "count users.json by .status"` | `jq 'group_by(.status) \| map({key: …, count: length})' users.json` |
| `jq "sort users.json by .age descending"` | `jq 'sort_by(.age) \| reverse' users.json` |
| `jq "convert users.json to csv with .name .email"` | `jq -r '.[] \| [.name, .email] \| @csv' users.json` |
| `grep "find TODO or FIXME in the src folder"` | `grep -rnE -e TODO -e FIXME src` |
| `grep "which python files contain 'import os'"` | `grep -rlE --include='*.py' -e 'import os' .` |
| `grep "list files not containing 'license' in src/"` | `grep -rLE -e license src/` |
| `grep "find ERROR in app.log ignoring case with 3 lines of context"` | `grep -nEi -C 3 -e ERROR app.log` |
| `grep "how many times does TODO appear in src/"` | `grep -rhoE -e TODO src/ \| wc -l` |
| `find "files larger than 100MB modified in last 7 days"` | `find . -type f -size +100M -mtime -7` |
| `find "c files in src ignoring build"` | `find src -path '*/build' -prune -o -type f -name '*.c' -print` |
| `find "files named 'readme*' ignoring case, top level only"` | `find . -maxdepth 1 -type f -iname 'readme*'` |
| `find "10 largest files ignoring .git"` | `find . -path '*/.git' -prune -o -type f -exec du -h {} + \| sort -rh \| head -n 10` |
| `find "delete all node_modules directories"` | `find . -mindepth 1 -type d -name node_modules -prune -exec rm -rf {} +` |
| `tar "compress src/ into backup.tar.gz excluding .git and node_modules"` | `tar --exclude='.git' --exclude='node_modules' -czvf backup.tar.gz src/` |
| `tar "extract archive.tar.xz into dist/ without the top-level folder"` | `tar -xJvf archive.tar.xz -C dist/ --strip-components=1` |
| `tar "list contents of backup.tar.gz"` | `tar -tzvf backup.tar.gz` |
| `git "undo last 2 commits keep changes staged"` | `git reset --soft HEAD~2` |
| `git "squash last 4 commits with message 'login flow'"` | `git reset --soft HEAD~4 && git commit -m 'login flow'` |
| `git "commits touching src/lexer.c in last 7 days"` | `git log --since='7 days ago' -p -- src/lexer.c` |
| `git "find commit that added 'inspect.c'"` | `git log -S 'inspect.c' --oneline` |
| `git "commits on feature-x not on main"` | `git log --oneline main..feature-x` |
| `git "who changed lines 10 to 20 of src/main.c"` | `git blame -L 10,20 src/main.c` |
| `git "restore src/main.c from 3 commits ago"` | `git restore --source=HEAD~3 -- src/main.c` |
| `git "restore deleted file src/old.c"` | `git checkout "$(git rev-list -n 1 HEAD -- src/old.c)^" -- src/old.c` |
| `git "create branch feature-x from main"` | `git switch -c feature-x main` |
| `git "delete merged branches"` | `git branch --merged HEAD \| grep -vE '^[*+]\|…(main\|master\|develop)$' \| xargs -r git branch -d` |
| `curl "post data.json to https://api.example.com/users"` | `curl -sS -X POST -H 'Content-Type: application/json' -d @data.json 'https://api.example.com/users'` |
| `curl "check response status of https://example.com"` | `curl -sSIL -o /dev/null -w '%{http_code}\n' 'https://example.com'` |
| `curl "download https://example.com/file.zip as out.zip"` | `curl -fL 'https://example.com/file.zip' -o out.zip` |
| `curl 'delete https://api.example.com/users/42 with token $TOKEN'` | `curl -sS -X DELETE -H "Authorization: Bearer $TOKEN" 'https://api.example.com/users/42'` |
| `curl "get json from api.github.com/users/octocat"` | `curl -sS -H 'Accept: application/json' 'https://api.github.com/users/octocat' \| jq .` |
| `curl "get https://self.local ignoring ssl errors, retry 3 times, timeout 10s"` | `curl -sS -k --retry 3 --max-time 10 'https://self.local'` |
| `curl "how long does https://example.com take"` | `curl -sS -o /dev/null -w 'dns:        %{time_namelookup}s\n…total:      %{time_total}s\n' 'https://example.com'` |
| `magick "resize hero.png to width 800 convert to webp quality 85"` | `magick hero.png -resize 800x -quality 85 hero.webp` |
| `magick "make sprite.png transparent background"` | `magick sprite.png -fuzz 5% -transparent white sprite_transparent.png` |
| `magick "shrink photo.jpg to 1920x1080 only if larger"` | `magick photo.jpg -resize '1920x1080>' photo_resized.jpg` |
| `magick "remove exif metadata from photo.jpg"` | `magick photo.jpg -auto-orient -strip photo_clean.jpg` |
| `magick "mirror photo.jpg horizontally"` | `magick photo.jpg -flop photo_mirrored.jpg` |
| `magick "pad photo.jpg to 1080x1080 with a black background"` | `magick photo.jpg -gravity center -background black -extent 1080x1080 photo_padded.jpg` |
| `magick "combine a.png b.png c.png side by side into sprite.png"` | `magick a.png b.png c.png +append sprite.png` |
| `magick "make a gif from frames/*.png at 12 fps"` | `magick -delay 8 -loop 0 'frames/*.png' animation.gif` |
| `magick "make a favicon from logo.png"` | `magick logo.png -background none -define icon:auto-resize=256,64,48,32,16 favicon.ico` |
| `magick "convert all png files in assets/ to webp"` | `magick mogrify -format webp 'assets/*.png'` |
| `magick "resize all jpg files in photos/ to width 1200 into small/"` | `mkdir -p small/ && magick mogrify -path small/ -resize 1200x 'photos/*.jpg'` |

Run `nls --list` to see all templates.

## How it works

1. **Lexer** (`src/lexer.c` tokenizes, then the tool's `src/lexer_<tool>.c`
   classifies): extracts typed slots and keeps the remaining
   words as intent tokens. The slot types are `FILE`, `EXT`, `TIME`, `RES`,
   `BITRATE`, `FPS`, `FACTOR`, `NUM`, `QUOTED_STR`, `JSON_PATH`, `SIZE` and `EXCLUDE`.
   - It uses a little context: `10 seconds` → TIME `10`, `width 640` → RES
     `640:-2`, `at 15 fps` → FPS `15`.
   - In jq mode, a bare word after a comparator becomes a string
     (`where .status is active`).
   - JSON paths followed by a comparator are condition paths and are ordered
     first.
2. **Matcher** (`src/matcher.c`): scores templates of the chosen tool by
   keyword overlap.
   - Points per keyword: exact match +10, inflection +8 (plural, or the same
     stem after stripping -s, -ing, -ed, or -er on comparatives: "changed" =
     "change", "bigger" = "big"), built-in synonym +7 ("shrink" = "compress",
     "erase" = "delete", "rollback" = "revert"), shared 4-character prefix
     +6. Each user word counts at most once. The synonym table and stemmer
     are compiled in (`src/matcher.c`).
   - Templates missing a required slot are blocked. If a blocked template
     scores higher than any valid one, `nls` reports what is missing instead of
     guessing. It does the same when the valid one only matched generic words
     or "this commit": `tag this commit` asks for the tag's name rather than
     reverting the commit.
   - Ties go to the template that uses more of what the user said, then the
     more specific one, then the one matched by real words rather than filler
     ("make video.mp4 smaller" compresses; "make" alone doesn't make it a GIF).
   - If a close runner-up was matched by a word the winner ignored, `nls`
     still prints its pick but lists the alternatives on stderr. A match
     that rests on a single keyword while another meaningful word went
     unused is flagged as low-confidence. Either way, `-y` no longer skips
     the confirmation for that command.
   - git: when a template can't take a detail you gave (git diff has no date
     filter), one that can wins instead: "what changed yesterday" →
     `git log --since='yesterday' -p`.
   - jq conditions can be joined with "and", "but" or "or": each clause is
     matched on its own and the conditions are combined into one `select()`.
     When both are used, "and" binds tighter than "or" (as in jq), and nls
     prints the grouping it chose on stderr: `.a is 1 or .b is 2 and .c is 3`
     is read as `.a is 1 or (.b is 2 and .c is 3)`.
   - jq ranges: `where .age between 20 and 40` (or "within", or "from 20 to
     40") keeps items with `.age >= 20 and .age <= 40`; reversed bounds are
     put in order. Sorting "from highest to lowest" is descending, "lowest to
     highest" ascending.
   - For jq, if the input file exists, `nls` reads its first non-blank byte to
     see whether it is an array or an object. It then skips templates that
     can't work on that shape. For example, `get .name` becomes
     `.[] | .name` on an array. With stdin, nothing is skipped.
   - The same peek (`src/inspect.c`, first 64 KB) collects field names, so
     plain words work: "users with emails" means `.email`, "by status" means
     `.status`. Plurals are matched too (emails, statuses, countries).
   - "how many" / "count" / "number of" together with a filter adds
     `| length`: `map(select(.email != null)) | length`.
   - For grep, modifier phrases ("ignore case", "whole word",
     "not containing", "literally") become flags (`-i -w -v -F`) on top of
     the chosen mode (search, list files, count, context, ...). Patterns can
     be quoted or bare after words like "for", "containing" or "find";
     "python files" / `*.py` become `--include`, and `-r` is dropped when
     only regular files are searched.
   - For find, every filter is optional and they combine: file types
     (`c files`, `*.py`), names (`named 'x*'`, `.DS_Store`), sizes
     (`larger than 100MB` → `-size +100M`), ages (`in the last 2 hours` →
     `-mmin -120`, `older than 30 days` → `-mtime +30`), line counts
     (`less than 100 lines` → `-exec awk 'END { exit !(NR < 100) }' {} \;`,
     one awk per file), skipped directories
     (`ignoring build` → `-path '*/build' -prune -o … -print`) and depth
     (`top level` → `-maxdepth 1`).
   - For tar, paths are sorted into the archive (anything ending in `.tar`,
     `.tar.gz`/`.tgz`, `.tar.bz2`, `.tar.xz`, `.tar.zst`), the destination
     (`into dist/`) and sources or members. Compression follows the archive
     suffix or a format word (`with xz`, `as zstd`), gzip by default; with no
     archive name one is derived (`compress src` → `src.tar.gz`). Also:
     `excluding X and Y` → `--exclude`, `without the top-level folder` /
     `strip 2` → `--strip-components`, `quietly` drops `-v`.
   - For git, names are sorted by the word before them: the branch acted on
     (`branch x`, `switch to x`, `merge x`) is `BRANCH`, the revision worked
     from (`from main`, `onto main`, `3 commits ago` → `HEAD~3`, a hash) is
     `REV`, and `not on main` / `since v1.0` is `EXCLUDE` (→ `main..`). With
     "tag" in the request, a version such as `v1.0` is the tag's name
     (`tag this commit v1.0`), and a negation before push or pull ("what
     haven't I pushed") means unpushed or unpulled commits.
     `last 3 commits` is a count, `by alice` / `by me` an author, `in the
     last 2 weeks` → `--since='2 weeks ago'`, `lines 10 to 20` → `-L 10,20`.
     Quoted text is a message or search string unless it looks like a path.
   - For curl, the URL is `http(s)://…` (or a bare `localhost:3000/x` /
     `api.example.com/x`, given a scheme), checked against the allowlist
     `[A-Za-z0-9._~:/?#[]@!$&'()*+,;=%-]`. A path after `post`/`put`/`upload`
     is the body; after `as`, `into` or `save … to` it is where the response
     goes. Inline `{"a":1}` or `name=value` is a body too. `header 'X: y'` and
     `token $TOKEN` / `bearer GITHUB_TOKEN` become `-H`; `follow redirects`,
     `ignore ssl errors`, `retry 3`, `timeout 10s`, `as user alice` become
     `-L`, `-k`, `--retry`, `--max-time`, `-u`.
   - For magick, sizes become a geometry (`800x600`, `width 800` → `800x`,
     `height 400` → `x400`, `50%`, `2x` → `200%`, `half size` → `50%`;
     `exactly` adds `!`, `only if larger` adds `>`), `quality 85` is the
     quality, and `fuzz 10%` / `90 degrees` / `blur by 8` / `10px border` is
     the operation's amount. Colours are names or `#hex`. A path after
     `to`/`as`/`into` (once there is an input) is the output; a folder there
     keeps the name and gets a `mkdir -p`. `all png files in assets/` is a
     batch (`mogrify` on `'assets/*.png'`), and the output name only gets a
     suffix (`_resized`) when it would otherwise overwrite the input
     (`hero.png` → `hero.webp`, but → `hero_resized.png`).
3. **Expander** (`src/expander.c`, plus `src/expander_<tool>.c` for tools with
   their own builders and refusal rules): fills the winning template.
   - Placeholders: `{TYPE:i}`, `{TYPE:i|default}`, `{OUT_FILE|ext|suffix}` and
     `<< optional >>` groups.
   - Filenames and strings are POSIX single-quoted.
   - Other values come from a restricted character set and are checked again
     before they are inserted.

## Safety

- `nls` never runs anything without `-x`, and `-x` asks first unless you also
  pass `-y`. Even with `-y` it asks when the match was uncertain (a close
  runner-up or a low-confidence match, see above), and for a destructive
  command unless you add `--force`. A destructive command also gets its dry
  run first and needs a typed `y` (see [Destructive commands](#destructive-commands)).
- Files and strings are always shell-quoted. jq string values are passed with
  `--arg`, never spliced into the jq program.
- grep patterns are passed with `-e`, so a pattern starting with `-` is never
  read as an option.
- find refuses a template that can't express everything you asked for, rather
  than dropping it: a lost "older than 30 days" would turn a delete into a
  much broader one. Deleting also needs at least one filter (`delete files`
  alone is refused), and directory deletes use `-mindepth 1` so the search
  root itself is never removed.
- tar refuses conflicting or unplaceable requests (`backup.tar.gz with xz`,
  a `.zip` name, two archives, a stray number) instead of guessing. A source
  or derived archive name starting with `-` gets a `./` prefix so it can't be
  read as an option.
- git refuses a template that would drop part of the request (a lost
  `by alice` or `from main` changes what a reset or restore does), a stray
  number (`undo 5`), unquoted shell syntax (`switch to $(whoami)`), and
  branch names git itself would reject (`..x`, `x.lock`, leading `-`).
  Messages and search text are always single-quoted; file lists go after
  `--`. Destructive commands (`reset --hard`, `clean -fd`, `restore`,
  `branch -D`, `push --delete`, force pushes) get a dry run and a typed
  confirmation with `-x`, and the safe variant wins ties (`undo last
  commit` → `reset --soft`, `delete branch` → `-d`, "which untracked files
  would be deleted" → `clean -n`).
- curl only accepts `http(s)` URLs from a strict character allowlist and
  always single-quotes them (`[`/`]` add `-g` so curl doesn't glob them).
  Bearer tokens given as `$NAME` or an uppercase name are emitted as
  `"Authorization: Bearer $NAME"`, so the secret stays in your environment
  and out of the command and your shell history. (Single-quote the prompt so
  your shell doesn't expand `$TOKEN` first.) A body on a GET, two bodies, two
  URLs, a stray number or a saved response on a status check is refused
  rather than dropped.
- magick never overwrites an input: single-image commands write a new file,
  `mogrify` batches only change the format (originals stay) or write into
  another folder (`-path resized`). Globs are passed quoted so magick expands
  them itself. It refuses file names magick would read as something else:
  a leading `-` (an option), `@list.txt` (a list of files) and `png:x` style
  format prefixes. It also refuses requests it can't fully express (a quality
  on a rotate, `exactly` with `only shrink`, a folder for a single-image
  command, transparency into a `.jpg`, a stray number) instead of dropping
  them. Commands target ImageMagick 7 (`magick`); on 6 use `convert` /
  `mogrify` without the `magick` prefix.
- JSON paths may only contain `[A-Za-z0-9_.[]?]`. A prompt like
  `get .a;rm in data.json` is rejected.
- Control characters in a request (anything below space except tab) separate
  words like spaces, even inside quotes. An escape sequence in a file name
  therefore can't reach the printed command and redraw the terminal before
  `-x` asks.
- All of this is fuzzed (see [Build](#build)): an unquoted shell character
  in a generated command that the template didn't put there fails the fuzzer.

## Adding templates

Add an entry to `NL_TEMPLATES[]` in `src/templates.c`, then add a line to
`tests/cases.tsv` and a few different wordings of the request to
`tests/paraphrase_cases.tsv`, then run `make test`. The paraphrase tests fail
if a change pulls any template's wordings to another template. Use `nls -v` to
see why a prompt matched (or didn't match) a template.

The playground on the landing page runs a WebAssembly build of nls that is
committed in `docs/wasm/`. After changing what nls prints, run
`make wasm-docker && make test-wasm`, or CI's `wasm` job fails.

## Adding a tool

Each tool lives in its own files behind two small interfaces:

| File | What goes in it |
|---|---|
| `src/lexer_<tool>.c` | `nl_lex_<tool>()`: turns raw tokens into slots and intent tokens. Declare it in `src/lexer_int.h` and add it to `LEXERS[]` in `src/lexer.c`. Shared helpers (`add_slot`, `looks_like_file`, `exclude_item`, …) are declared in `lexer_int.h`. |
| `src/expander_<tool>.c` | Optional. A `ToolExpander` (`check`, `prologue`, `placeholder`, any of which may be `NULL`) for tool-specific placeholders and refusals. Register it in `EXPANDERS[]` in `src/expander.c`. ffmpeg, jq and grep get by with the generic template grammar alone. |
| `src/templates.c` | The tool's `NL_TEMPLATES[]` entries. |
| `src/nl.h`, `src/lexer.c` | A `TOOL_<NAME>` id and its name in `nl_tool_from_name()` / `nl_tool_name()`. |
| `src/explain.c` | The command in `COMMANDS[]` and its options in `OPTS[]` (subcommands as `"tool sub"`), and what its positional words are in `positional()`. |
| `src/main.c` | Inference: the clues that point at the tool (file extensions, words) in `evidence_word()`, and whether it may only be picked with one (`NEEDS_EVIDENCE[]`). Add its name to `NL_TOOL_LIST`, and take it out of `UNSUPPORTED[]` if it is listed there (`rsync`, `openssl`, …). |
| `completions/`, `man/nls.1` | The tool's name (`tests/run_completion_tests.sh` checks it is there). |

Then add golden cases to `tests/cases.tsv`, a few requests without the tool to
`tests/infer_cases.tsv` and, ideally, a `tests/run_<tool>_exec.sh` suite that runs
the generated commands. `make test` checks that every generated command is fully
explained by `--explain`. The Makefile builds every `src/*.c`, so new files need
no build changes.

## License

[MIT](LICENSE): free to use, modify and redistribute, including commercially.
