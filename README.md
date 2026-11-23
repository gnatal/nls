# nls — natural language shell

`nls` turns plain-English requests into exact `ffmpeg` and `jq` commands.
It works fully offline: no LLM, no network, no runtime dependencies. It is
written in C11 using only the standard library, and every command template is
compiled into the binary.

```console
$ nls ffmpeg "cut input.mp4 from 00:10 to 00:25 and scale to 720p"
ffmpeg -ss 00:10 -to 00:25 -i input.mp4 -vf scale=-2:720 -c:a copy input_cut.mp4

$ nls jq "get .name from users.json where .age > 30"
jq 'map(select(.age > 30) | .name)' users.json
```

v0.1 supports **ffmpeg** and **jq**. `grep` and `grop` are planned
(see `natural_language_shell_architecture_plan.md`).

## Build

```sh
make          # cc -std=c11 -O2 -Wall -Wextra -pedantic -Werror
make test     # all tests (each execution suite skips if its tool is missing):
              #  - command-string goldens        tests/cases.tsv
              #  - jq: run on tests/fixtures/*.json     tests/jq_exec_cases.tsv
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

Run `nls --list` to see all templates (about 30 per tool).

## How it works

1. **Lexer** (`src/lexer.c`): extracts typed slots and keeps the remaining
   words as intent tokens. The slot types are `FILE`, `EXT`, `TIME`, `RES`,
   `BITRATE`, `FPS`, `FACTOR`, `NUM`, `QUOTED_STR` and `JSON_PATH`.
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
- JSON paths may only contain `[A-Za-z0-9_.[]?]`. A prompt like
  `get .a;rm in data.json` is rejected.

## Adding templates

Add an entry to `NL_TEMPLATES[]` in `src/templates.c`, then add a line to
`tests/cases.tsv` and run `make test`. Use `nls -v` to see why a prompt
matched (or didn't match) a template.
