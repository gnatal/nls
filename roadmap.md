# nls roadmap

Planned improvements, roughly in priority order. The first two sections
address the two ways nls can hurt a user: a confidently wrong command, and a
wrong command that gets run.

## 1. Matching quality

The first pass is done (see "Done"). Open issues found since; the first
three give a confidently wrong command with no warning:

- **jq: an unknown comparator swaps the paths.** A path counts as the
  condition only when a known comparator word follows it. Otherwise the
  first path in the prompt wins, so the projection becomes the condition:
  - `get .name from users.json where .age around 30` →
    `select(.name == 30) | .age`
  - `... where .age from 20 to 40` → `select(.name >= 20 and .name <= 40) | .age`

  A path right after "where"/"whose"/"if" should count as the condition
  whatever follows it. When nls can't tell which path is which, it should
  refuse instead of guessing.
- **jq: "outside X and Y" gives the inside range.** "outside" isn't a
  keyword, so `where .age outside 20 and 40` matches `jq_filter_between`
  and keeps exactly the items the user wanted dropped. It needs its own
  template (`< X or > Y`), or must at least never match "between".
- **grep: a folder name without a slash is dropped.**
  `grep "search for TODO in src"` gives `grep -rnE -e TODO .`. It silently
  searches the current folder, even when `src` exists, while `in src/`
  works. A word after "in"/"under" that names an existing directory should
  be taken as the path. If no such directory exists, nls should say so.
- **magick: "combine a.png b.png c.png into anim.gif at 10fps" is
  refused.** It matches `magick_append`, which can't take a frame rate,
  instead of the animation template ("animate …" or "make a gif from …"
  work). The git-only fallthrough to a template that can take every
  detail could be extended to magick, where every fallback is
  non-destructive.
- **Explicit grouping words** ("either ... or ...", "both ... and ...")
  could override the default "and before or" reading of mixed jq
  conditions.

## 2. Safety around `-x`

- **Tag destructive templates as dangerous:** `find -delete`, `rm -rf`,
  `git reset --hard`, `git clean -fdx`, `git push --delete`. For these, `-y`
  should not skip the confirmation without an extra `--force` (or should at
  least show a prominent warning).
- **Dry run first** where the tool has one: `find … -print` before
  `-delete`, `git clean -n` before `git clean -fd`.
- Section 1 already makes `-y` ask for an uncertain match (a close runner-up
  or a low-confidence match). Destructive templates still need their own tag,
  so a confident but dangerous match also gets the extra step.

## 3. User experience

- **Infer the tool.** In `nls "cut input.mp4 from 0:10 to 0:25"`, the
  `.mp4` extension, a `.json` file or a URL often already says which tool is
  meant. Score the prompt against every tool and pick the best one.
- **Shell integration.** A zsh/bash widget that replaces the typed prompt
  with the generated command on the command line, so the user can edit it
  before running.
- **Shell completions** for tools and options, and a **man page**.
- **`--explain`.** Break the generated command down flag by flag
  ("`-crf 28` sets quality, lower is better"). This turns nls into a learning
  tool as well as a generator.

## 4. Engineering

- **CI on every push and pull request.** The release workflow only tests
  when a tag is pushed. A `ci.yml` that runs `make test` on macOS and Linux
  for every push and pull request would catch regressions earlier.
- **Paraphrase tests.** Keep a file of varied wordings for each template
  that must keep matching it, so a template change can't quietly pull those
  prompts elsewhere (like the GIF tie in section 1).
- **Spot a stale installed binary.** A copy of nls installed with
  `make install` keeps old behaviour after the source changes, which looks
  like a regression (a fixed jq range bug kept showing). `nls --version`
  could print the git commit it was built from, so the two can be compared.
- **Fuzzing.** Run a libFuzzer/AFL harness on `nl_lex` + `nl_expand`. The
  lexer works on fixed-size buffers, and an injection or overflow in the
  quoting is the worst bug this tool could have.
- **Publish the Homebrew tap.** Create the `homebrew-tap` repo from
  `packaging/homebrew/nls.rb` (see `RELEASING.md`).

## 5. More tools

From `part2.md`, ordered by how often people have to look up the syntax:

1. `openssl`
2. `rsync`
3. `sed` / `awk`
4. `docker`
5. `ssh` / `scp`

With the per-tool layout, each tool is a `lexer_<tool>.c`, an optional
`expander_<tool>.c` and its templates (see "Adding a tool" in the README).
Matching quality (section 1) should come first, though: every new template
makes ties more likely.

## Done

- **Matching quality.** Close runners-up are listed on stderr and
  low-confidence matches are flagged; `-y` asks for both. Ties prefer
  templates matched by real words over filler (`make video.mp4 smaller` now
  compresses). A built-in synonym table and light stemming were added. jq
  conditions combine with `and`/`but`/`or` ("and" binds tighter, and the
  grouping is shown on stderr). `between X and Y` / `within X and Y` gives
  an inclusive range, reversed bounds are put in order, and ranges combine
  with other conditions. git falls through to a template
  that can take a date (`what changed yesterday` → `git log --since -p`).

- **Per-tool modules.** `lexer.c` and `expander.c` were split into
  `lexer_<tool>.c` / `expander_<tool>.c` behind small interfaces
  (`lexer_int.h`, `expander_int.h`). Output is unchanged.
- **Releases.** A tag-triggered GitHub workflow builds static Linux
  (x86_64, arm64) and universal macOS binaries, runs the tests and publishes
  them with checksums. Also added: `make dist`, `install.sh` and a Homebrew
  formula.
- **License.** MIT.
