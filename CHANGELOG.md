# Changelog

All notable changes to nls. Versions follow [semantic versioning](https://semver.org).
The release workflow uses the section matching the tag as the release notes.

## [Unreleased]

### Matching quality

- Ties are no longer settled silently by template order. A close runner-up
  matched by a word the winner ignored is listed on stderr ("this request
  also fits: …") next to the command it would produce, and a match that
  rests on one keyword while another meaningful word went unused is flagged
  as low-confidence. For such matches `-y` no longer skips the confirmation.
- Tie-breaking prefers templates matched by real words over filler:
  `nls ffmpeg "make video.mp4 smaller"` now compresses instead of making a GIF.
- Built-in synonyms ("shrink/reduce/smaller → compress", "erase/remove →
  delete", "rollback → revert", …) and light suffix stripping (-s, -ing,
  -ed, comparative -er), still compiled in and offline.
- jq: conditions can be combined with "and", "but" or "or":
  `where .age > 30 and .status is active`. When both are used, "and" binds
  tighter than "or", and the grouping is printed on stderr.
- jq: ranges. `where .age between 20 and 40` now gives
  `.age >= 20 and .age <= 40` (it used to give `.age == 20`).
- jq: a path right after "where"/"whose"/"if" is always the condition.
  `get .name where .age around 30` used to test `.name == 30`; an unknown
  comparison like "around" is now refused, as is a filter with several
  paths and none of them clearly the condition.
- jq: `where .age from 20 to 40` is an inclusive range, and
  `outside 20 and 40` / `not between 20 and 40` give `.age < 20 or .age > 40`
  (they used to keep the inside of the range).
- jq: "either … or …" and "both … and …" group conditions explicitly:
  `where either .a is 1 or .b is 2 and .c is 3` is `(a or b) and c`.
- grep: a folder or file named without a slash is used as the path
  (`search for TODO in src` searched `.`). If nothing by that name exists,
  nls says so instead of searching the current folder.
- magick: `combine a.png b.png into anim.gif at 10fps` makes an animation
  instead of being refused: a template that can't take a detail falls
  through to one that can, as git already did.
- grep: `count TODO in src/` takes TODO as the pattern ("count" now comes
  before a search pattern, like "search" or "find").
- magick: several images combined into a `.gif` animate (10 fps by default)
  instead of being joined side by side. "side by side" still makes a strip,
  with the animation listed as an alternative.
- git: `nls git "what changed yesterday"` gives `git log --since='yesterday' -p`
  instead of an error.

### Safety around `-x`

- Destructive commands are tagged: deleting files with find (`-delete`,
  `rm -rf`), `git reset --hard`, `git restore`, `git clean`, `git branch -D`,
  `git push --delete`, `git push --force-with-lease` and `curl -X DELETE`.
  `nls --list` marks them "(destructive)".
- With `-x`, nls says what such a command destroys and runs a dry run first
  where the tool allows it (`find … -print`, `git clean -n`,
  `git push --dry-run`, `git diff --stat`). The dry run's output goes to
  stderr, cut at 20 lines. If the dry run fails, the command isn't run.
- Only a typed `y` runs a destructive command; Enter aborts it.
- `-y` no longer skips that question unless `--force` is given too. Even
  `--force` doesn't skip it for an uncertain match.
- `-v` shows a destructive command's dry run without running anything.

### User experience

- The tool can be left out: `nls "convert demo.mov to gif"`. The request is
  compiled for every tool, and the best fit is used. A tool's score is its
  keyword score plus points for what the request names: media files,
  images, JSON files and paths, archives, URLs, git words, search words,
  file tests, the tool's own name. jq, ffmpeg, magick, git and curl only
  count when the request names something of theirs. A close runner-up is
  shown on stderr and makes `-y` ask; a request that points at no tool is
  refused. `nls -v` shows every tool's score.
- A one-argument request may start with the tool (`nls "git undo last
  commit"`), except `find` and `convert`, which are read as verbs there. A
  first word naming a tool nls doesn't support (`rsync`, `openssl`, …) is
  refused.
- `-e` / `--explain` breaks the command down part by part: each option
  with its value, bundled letters (`-czvf`), jq programs stage by stage,
  ffmpeg filter chains, ImageMagick geometries, find sizes, ages and
  `-exec`, git revisions, pipes and `&&`. With `-x`, it goes to stderr.
- `nls --shell zsh` / `nls --shell bash` (bash 4.0 or later) print a key
  binding: type a request on the command line, press Ctrl+G, and the line
  becomes the command, to edit before running. `NLS_KEY` picks another key.
- bash and zsh completions (`completions/`) and a man page (`man/nls.1`).
  `make install`, `install.sh` and the Homebrew formula install them.
- A request quoted as a whole (`'cut in.mp4 from 1:00 to 2:00'`) is
  unquoted, so its words are read as words.

## [0.1.0] - 2026-11-24

First release.

- Natural-language requests compiled to exact commands for **ffmpeg**, **jq**,
  **grep**, **find**, **tar**, **git**, **curl** and **ImageMagick** (`magick`,
  also reachable as `nls convert …`).
- Fully offline: no LLM, no network, no runtime dependencies; C11 standard
  library only, every template compiled into the binary.
- `-x` runs the command after confirmation, `-v` explains slots, tokens and
  template scores, `-l` lists templates with example prompts.
- jq peeks at the input file to use its field names and refuse templates that
  don't fit its shape.
- Prebuilt binaries for Linux (x86_64, arm64; static) and macOS (universal).
