# Changelog

All notable changes to nls. Versions follow [semantic versioning](https://semver.org).
The release workflow uses the section matching the tag as the release notes.

## [Unreleased]

### Fixes: wrong commands given without a warning

- git: `what haven't I pushed` listed unpushed commits only by luck of
  template order; it could give `git revert`. A negation before a push or
  pull verb ("haven't I pushed", "not pulled yet") now reads as
  unpushed / unpulled.
- git: `tag this commit v1.0` gave `git revert --no-edit HEAD`. With "tag"
  in the request, a version (`v1.0`, `2.3.1`, `v2.0.0-rc.1`) is the tag name.
- git: `tag this commit` and `label this commit` (no name) gave
  `git revert`. A command that only matched "this commit" no longer wins
  over the one the verb points at: nls asks for the tag's name instead.
- git: `stop the rebase in progress` gave `git rm -r --cached -- progress`
  ("in progress" was read as a path). It now aborts the rebase.
- git: `what tags exist` created a tag named "exist", and `which branches
  exist` created a branch. Both now list.
- git: `commit staged changes with the message '…'` ran `git add -A` first,
  committing unstaged work too. It now commits only what is staged.
- ffmpeg: `turn down the volume of movie.mkv` flipped the video upside down.
  With "volume", "sound" or "audio" in the request, "down" / "up" mean
  quieter / louder (`turn the sound up` also no longer extracts the audio),
  and `speed up the audio` speeds up instead of extracting.
- The low-confidence warning printed `only  matched` with an empty word when
  the match came from "this commit"; it now says a generic phrase matched.

## [0.1.0] - 2026-11-25

First release.

- Natural-language requests compiled to exact commands for **ffmpeg**, **jq**,
  **grep**, **find**, **tar**, **git**, **curl** and **ImageMagick** (`magick`,
  also reachable as `nls convert …`).
- Fully offline: no LLM, no network, no runtime dependencies; C11 standard
  library only, every template compiled into the binary.
- `-x` runs the command after confirmation, `-v` explains slots, tokens and
  template scores, `-l` lists templates with example prompts.
- Prebuilt binaries for Linux (x86_64, arm64; static) and macOS (universal),
  an `install.sh` that verifies their checksums, and a Homebrew formula.

### Picking the tool

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
- A request quoted as a whole (`'cut in.mp4 from 1:00 to 2:00'`) is
  unquoted, so its words are read as words.

### Matching

- Ties are never settled silently by template order. A close runner-up
  matched by a word the winner ignored is listed on stderr ("this request
  also fits: …") next to the command it would produce, and a match that
  rests on one keyword while another meaningful word went unused is flagged
  as low-confidence. For such matches `-y` still asks for confirmation.
- Ties prefer templates matched by real words over filler:
  `nls ffmpeg "make video.mp4 smaller"` compresses rather than making a GIF.
- Built-in synonyms ("shrink/reduce/smaller → compress", "erase/remove →
  delete", "rollback → revert", …) and light suffix stripping (-s, -ing,
  -ed, comparative -er), compiled in and offline.
- A template that can't take a detail falls through to one that can:
  `nls git "what changed yesterday"` gives `git log --since='yesterday' -p`,
  and `combine a.png b.png into anim.gif at 10fps` makes an animation.
- jq peeks at the input file to use its field names and refuse templates
  that don't fit its shape.
- jq conditions combine with "and", "but" or "or":
  `where .age > 30 and .status is active`. When both are used, "and" binds
  tighter than "or", and the grouping is printed on stderr. "either … or …"
  and "both … and …" group conditions explicitly:
  `where either .a is 1 or .b is 2 and .c is 3` is `(a or b) and c`.
- jq ranges: `where .age between 20 and 40` and `from 20 to 40` give
  `.age >= 20 and .age <= 40`; reversed bounds are put in order.
  `outside 20 and 40` / `not between 20 and 40` give `.age < 20 or .age > 40`.
- jq: a path right after "where"/"whose"/"if" is always the condition. An
  unknown comparison (`where .age around 30`) is refused, as is a filter
  with several paths and none of them clearly the condition.
- grep: a folder or file named without a slash is used as the path
  (`search for TODO in src`). If nothing by that name exists, nls says so
  instead of searching the current folder. "count" comes before a search
  pattern (`count TODO in src/`).
- magick: several images combined into a `.gif` animate (10 fps by default);
  "side by side" makes a strip, with the animation listed as an alternative.

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
- `-y` skips that question only with `--force`, and never for an uncertain
  match.
- `-v` shows a destructive command's dry run without running anything.
- Control characters in a request (an escape sequence in a file name) never
  reach the printed command; they separate words like spaces.

### Shell integration and docs

- `-e` / `--explain` breaks the command down part by part: each option
  with its value, bundled letters (`-czvf`), jq programs stage by stage,
  ffmpeg filter chains, ImageMagick geometries, find sizes, ages and
  `-exec`, git revisions, pipes and `&&`. With `-x`, it goes to stderr.
- `nls --shell zsh` / `nls --shell bash` (bash 4.0 or later) print a key
  binding: type a request on the command line, press Ctrl+G, and the line
  becomes the command, to edit before running. `NLS_KEY` picks another key.
- bash and zsh completions (`completions/`) and a man page (`man/nls.1`).
  `make install`, `install.sh` and the Homebrew formula install them.
- `nls --version` prints the commit it was built from between releases, and
  `make install` warns when another `nls` comes first on `PATH`.

### Testing

- CI runs `make test` on Linux and macOS for every push and pull request,
  along with `make test-sanitize` (every suite against an ASan + UBSan
  build) and a two-minute libFuzzer run.
- Paraphrase tests (`tests/paraphrase_cases.tsv`): about 400 varied
  wordings, at least one for every template, must keep picking their
  template. Wordings that don't match yet are listed as known misses.
- Fuzzing (`make fuzz`, `make fuzz-check`): the lexer, matcher, expanders and
  `--explain` run under ASan + UBSan, with an oracle that fails on an
  unquoted shell character the template didn't produce. Failing inputs are
  kept in `tests/fuzz/regressions/`.
