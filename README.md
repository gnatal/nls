# nls — natural language shell

`nls` turns plain-English requests into exact `ffmpeg`, `jq`, `grep`,
`find`, `tar` and `git` commands.
It works fully offline: no LLM, no network, no runtime dependencies. It is
written in C11 using only the standard library, and every command template is
compiled into the binary.

```console
$ nls ffmpeg "cut input.mp4 from 00:10 to 00:25 and scale to 720p"
ffmpeg -ss 00:10 -to 00:25 -i input.mp4 -vf scale=-2:720 -c:a copy input_cut.mp4

$ nls jq "get .name from users.json where .age > 30"
jq 'map(select(.age > 30) | .name)' users.json
```

Supported tools: **ffmpeg**, **jq**, **grep**, **find**, **tar** and **git**
(see `natural_language_shell_architecture_plan.md`).

## Build

```sh
make          # cc -std=c11 -O2 -Wall -Wextra -pedantic -Werror
make test     # all tests (each execution suite skips if its tool is missing):
              #  - command-string goldens        tests/cases.tsv
              #  - jq: run on tests/fixtures/*.json     tests/jq_exec_cases.tsv
              #  - grep: run on tests/fixtures/grep/    tests/grep_exec_cases.tsv
              #  - find: run on a generated temp tree   tests/find_exec_cases.tsv
              #  - tar: create/extract/list in a temp tree  tests/tar_exec_cases.tsv
              #  - ffmpeg: run on generated media,
              #    verified with ffprobe          tests/ffmpeg_exec_cases.tsv
make test-ffmpeg  # only the ffmpeg execution tests (~10 s)
make install  # installs to ~/.local/bin/nls (no sudo needed)
```

The install step prints a one-line fix if `~/.local/bin` isn't on your `PATH`. Options:

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
codecs, audio/video streams, fps and frame count with `ffprobe`.

## Usage

```
nls [options] <tool> "<what you want>"

  -x, --exec        run the command (shows it, asks for confirmation)
  -y, --yes         with -x: skip confirmation
  -v, --verbose     show extracted slots, tokens and template scores
  -l, --list [tool] list all templates with example prompts
```

By default `nls` only **prints** the command. To run it, use `-x`, or
`eval "$(nls ...)"`:

```sh
curl -s https://api.example.com/users | eval "$(nls jq 'list .email of each user')"
```

Quote the whole prompt so your shell doesn't interpret `>`, `'` or `*`.

## Examples

| Prompt | Command |
|---|---|
| `ffmpeg "convert demo.mov to gif at 12fps width 320"` | `ffmpeg -i demo.mov -vf 'fps=12,scale=320:-2:…paletteuse' -loop 0 demo.gif` |
| `ffmpeg "keep the last 10s of in.mp4"` | `ffmpeg -sseof -10 -i in.mp4 -c copy in_cut.mp4` |
| `ffmpeg "extract audio from video.mp4 as mp3 at 320k"` | `ffmpeg -i video.mp4 -vn -c:a libmp3lame -b:a 320k video.mp3` |
| `ffmpeg "compress video.mkv to 720p"` | `ffmpeg -i video.mkv -vf scale=-2:720 -c:v libx264 -crf 28 … video_compressed.mp4` |
| `ffmpeg "remove audio from video.mp4"` | `ffmpeg -i video.mp4 -c:v copy -an video_muted.mp4` |
| `jq "users.json where .status is active"` | `jq --arg v active 'map(select(.status == $v))' users.json` |
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

Run `nls --list` to see all templates.

## How it works

1. **Lexer** (`src/lexer.c`): extracts typed slots and keeps the remaining
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
   - Points per keyword: exact match +10, plural +8, shared 4-character stem
     +6. Each user word counts at most once.
   - Templates missing a required slot are blocked. If a blocked template
     scores higher than any valid one, `nls` reports what is missing instead of
     guessing.
   - Ties go to the template that uses more of what the user said.
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
     `REV`, and `not on main` / `since v1.0` is `EXCLUDE` (→ `main..`).
     `last 3 commits` is a count, `by alice` / `by me` an author, `in the
     last 2 weeks` → `--since='2 weeks ago'`, `lines 10 to 20` → `-L 10,20`.
     Quoted text is a message or search string unless it looks like a path.
3. **Expander** (`src/expander.c`): fills the winning template.
   - Placeholders: `{TYPE:i}`, `{TYPE:i|default}`, `{OUT_FILE|ext|suffix}` and
     `<< optional >>` groups.
   - Filenames and strings are POSIX single-quoted.
   - Other values come from a restricted character set and are checked again
     before they are inserted.

## Safety

- `nls` never runs anything without `-x`, and `-x` asks first unless you also
  pass `-y`.
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
  `branch -D`) are only run with `-x` after confirmation, and the safe
  variant wins ties (`undo last commit` → `reset --soft`, `delete branch` →
  `-d`, "which untracked files would be deleted" → `clean -n`).
- JSON paths may only contain `[A-Za-z0-9_.[]?]`. A prompt like
  `get .a;rm in data.json` is rejected.

## Adding templates

Add an entry to `NL_TEMPLATES[]` in `src/templates.c`, then add a line to
`tests/cases.tsv` and run `make test`. Use `nls -v` to see why a prompt
matched (or didn't match) a template.
