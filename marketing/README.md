# Marketing kit: nls

Everything needed to explain and launch `nls`. Every claim below was measured
on the project itself. Re-check the **Fact sheet** before each launch, since
numbers change as templates are added.

| File | What it is |
|---|---|
| `README.md` | Positioning, messaging, fact sheet (this file) |
| `launch-posts.md` | Ready-to-edit posts: Show HN, Reddit, X/Bluesky thread, LinkedIn |
| `demo.tape` | [VHS](https://github.com/charmbracelet/vhs) script that records the terminal demo GIF |
| `infer-completion.tape` | VHS script for `docs/nls-infer-completion.gif` (tool inference, `-v`, Tab completion, Ctrl+G), shown in the main README |
| `race.tape` | VHS script for `nls-race.gif`: the landing page's race in a split terminal, the real nls (ten processes, timed) beside a labelled simulation of an LLM CLI helper (1–4 s waits, an invented tar flag, `'$TOKEN'`, `grep -lv`, an unquoted `>`). For the launch post |
| `race.cjs` | The split-screen race itself (`node ../race.cjs ../../nls` from `marketing/demo/`; any key quits). Its requests and simulated answers come from `#race-data` in `docs/index.html`, which `make test-race` checks |
| `make-demo-media.sh` | Generates royalty-free demo files (`talk.mp4`, `demo.mp4`, `users.json`, a small `src/` tree, `app.log`, plus old logs, big files, `node_modules/` and `.DS_Store` clutter for the find demo, `.git/`, `node_modules/` and a `release-1.0.tgz` for the tar demo, a `project/` git repo with two authors and a feature branch for the git demo, and a `data.json` body for the curl demo, and `hero.png`, `sprite.png` and `frames/` drawn with ImageMagick for the magick demo) |

---

## One-liner

> **Plain English in, the exact ffmpeg, jq, grep, find, tar, git, curl or ImageMagick command out. Offline, instant, no AI.**

Alternatives:
- "Stop googling ffmpeg flags."
- "`nls` compiles English into shell commands."
- "The ffmpeg/jq/grep/find/tar/git/curl/ImageMagick cheat sheet you can talk to."
- "Never type `-prune -o` again."
- "Stop looking up which tar flag is xz."
- "Undo that commit without googling `reset --soft` vs `--hard`."
- "POST that JSON without looking up `-H`, `-d` and `@`."
- "Resize, convert and compress images without remembering `-resize 800x`."
- "Search your code in plain English, without an AI reading it."

## Elevator pitch (30 seconds)

Everyone has googled "ffmpeg cut video" more than once, jq's syntax hasn't
gotten easier to remember, and nobody recalls whether "files that *don't*
match" is `grep -L` or `grep -lv`. `find` with `-prune` is something
people copy from Stack Overflow every single time, `tar` is the classic
"which letters, in which order?" command, and every developer has a git
command they're scared to type ("was it `reset --soft` or `--hard`?").
And testing an API with curl means remembering `-X POST`, the JSON header,
`-d @file` and how to print just the status code. ImageMagick hides its best
tricks in geometry syntax (`800x`, `'1920x1080>'`, `'800x600!'`) and swaps
the names you'd expect (`-flip` is upside down, `-flop` is a mirror). `nls` lets you type what
you want, like
`nls ffmpeg "extract the first 10 seconds of talk.mp4 into intro.mp4"`,
`nls grep "which python files contain TODO, ignoring case"`,
`nls find "files larger than 5MB ignoring node_modules"`,
`nls tar "extract release.tgz into dist/ without the top-level folder"`,
`nls git "undo last 2 commits keep changes staged"`,
`nls curl "post data.json to https://api.example.com/users"` or
`nls magick "resize hero.png to width 800 convert to webp quality 85"`. You get back
the exact command, ready to run.

It isn't a chatbot. There's no API key, no network, and no model. (curl's
commands use the network when you run them, of course; `nls` itself never
does.) A tiny C
program matches your words against a built-in library of command templates.
That means the same request always gives the same answer, in about 2 ms,
even on a plane.

## The demo that sells it

This run was not staged. It is real output:

```console
$ nls -x -y ffmpeg "keep the first 10 seconds of talk.mp4 into intro.mp4"
→ ffmpeg -i talk.mp4 -t 10 -c copy intro.mp4
```

Why it lands:
- **No parsing rules to learn.** The prompt is plain, natural English.
- **The output name is respected.** "into intro.mp4" became the output file.
- **The result is better than what most people would type by hand.** It uses
  `-c copy`, so there's no re-encode and it finishes almost instantly.

Other proven crowd-pleasers (all covered by the test suite):

```console
$ nls jq "in users.json tell me how many users have emails"
jq 'map(select(.email != null)) | length' users.json

$ nls ffmpeg "make a gif from demo.mp4 between 2 and 5 seconds"
ffmpeg -ss 2 -to 5 -i demo.mp4 -vf 'fps=10,scale=480:-1:flags=lanczos,split[s0][s1];[s0]palettegen[p];[s1][p]paletteuse' -loop 0 demo.gif

$ nls jq "get names from users.json where age > 30"
jq 'map(select(.age > 30) | .name)' users.json

$ nls -x -y grep "which python files contain TODO, ignoring case"
→ grep -rlEi --include='*.py' -e TODO .
./src/worker.py
./src/server.py

$ nls -x -y grep "find ERROR in app.log with 1 line of context"
→ grep -nE -C 1 -e ERROR app.log
3-2026-11-23 09:01:12 WARN  slow query (1840ms)
4:2026-11-23 09:01:13 ERROR connection reset by peer
5-2026-11-23 09:01:14 INFO  retrying in 2s
...

$ nls grep "list files not containing 'license' in src/"
grep -rLE -e license src/

$ nls grep "how many times does TODO appear in src/"
grep -rhoE -e TODO src/ | wc -l
```

The jq example works because `nls` reads the file's field names: "emails"
becomes `.email` on its own.

The grep examples show the parts people get wrong by hand:
- **"python files"** becomes `--include='*.py'`.
- **"not containing"** on a file listing becomes `-L` (files with no match),
  not `-lv` (files with at least one non-matching line, which is nearly all
  of them).
- **"ignoring case"**, **"whole word"** and **"literally"** add `-i`, `-w` and
  `-F` to any mode, whether you're searching, counting, listing files or
  asking for context.
- The pattern always goes through `-e`, so searching for `-v` can't turn into
  a flag.

find, run in the demo folder (`make-demo-media.sh` creates it):

```console
$ nls -x -y find "log files older than 30 days"
→ find . -type f -name '*.log' -mtime +30
./logs/archive-2025-06.log
./logs/archive-2025-03.log

$ nls -x -y find "files larger than 5MB ignoring node_modules"
→ find . -path '*/node_modules' -prune -o -type f -size +5M -print
./assets/backup.tar

$ nls -x -y find "py files with less than 10 lines"
→ find . -type f -name '*.py' -exec awk 'END { exit !(NR < 10) }' {} \; -print
./src/worker.py
./src/server.py

$ nls find "delete all node_modules directories"
find . -mindepth 1 -type d -name node_modules -prune -exec rm -rf {} +

$ nls find "delete files"
nls: refusing to delete everything; say which files (e.g. '*.log', named 'core', older than 30 days) (find_delete)
```

The find examples show the traps people fall into by hand:
- **Skipping a directory** needs `-path '*/X' -prune -o … -print`, with the
  `-print` at the end, or the pruned directory gets printed anyway.
- **Signs**: `older than 30 days` is `-mtime +30`, `in the last 2 hours` is
  `-mmin -120`. Nobody remembers which way round `+` and `-` go.
- **Rounding**: `-size -1M` only matches *empty* files, because find rounds
  up to whole units. `nls` writes "smaller than 2MB" as `-size -2048k`.
- **Line counts** aren't something find can do. `nls` pairs it with awk
  ("less than 10 lines") and keeps it combinable with every other filter.
- **Deleting safely**: a delete with no filter is refused; `-delete` is never
  combined with `-prune` (it silently turns pruning off); directory deletes
  use `-mindepth 1`, so the search root itself is never removed.
- **Nothing gets dropped**: if a template can't express part of the request,
  `nls` refuses instead of running a broader command.

tar, run in the same demo folder:

```console
$ nls -x -y tar "back up src/ into backup.tgz, skipping .git, node_modules and .DS_Store"
→ tar --exclude='.git' --exclude='node_modules' --exclude='.DS_Store' -czvf backup.tgz src/
a src
a src/worker.py
a src/server.py
a src/README.md
a src/app.js

$ nls -x -y tar "list contents of backup.tgz quietly"
→ tar -tzf backup.tgz
src/
src/worker.py
...

$ nls -x -y tar "extract release-1.0.tgz into dist/ without the top-level folder"
→ tar -xzvf release-1.0.tgz -C dist/ --strip-components=1
x bin/
x README.md
x bin/app

$ nls tar "compress src/ into backup.tar.gz with xz"
nls: 'backup.tar.gz' is a gzip archive, but xz was asked for (tar_create)
```

The tar examples show the traps people fall into by hand:
- **`-f` goes last.** `tar -cfz backup.tar.gz src/` creates an archive named
  `z`, because `-f` takes the next thing as the file name. `nls` always ends
  the cluster with `f`.
- **The compression letter** (`z` gzip, `j` bzip2, `J` xz, `--zstd`) comes
  from the archive name, and a mismatch is refused rather than producing a
  `.tar.gz` that is really xz. No name given? `compress src` makes
  `src.tar.gz`.
- **Excludes are quoted.** Type `--exclude=*.log` unquoted in zsh (the macOS
  default shell) and the command never runs: `no matches found`.
- **`--strip-components`**: "without the top-level folder" unpacks
  `release-1.0/bin/app` as `dist/bin/app`.
- **A bare archive name lists, never extracts**, so `nls tar "backup.tar.gz"`
  can't overwrite anything.

git, run in the demo's `project/` repo (hashes and blame dates change each time you run
`make-demo-media.sh`):

```console
$ nls -x -y git "find commit that added 'def retry'"
→ git log -S 'def retry' --oneline
c0585b3 add retry helper

$ nls -x -y git "who changed lines 3 to 4 of src/server.py"
→ git blame -L 3,4 src/server.py
46865555 (bruno 2026-11-24 10:52:21 -0300 3) PORT = int(os.environ.get("PORT", 8080))
46865555 (bruno 2026-11-24 10:52:21 -0300 4) HOST = os.environ.get("HOST", "0.0.0.0")

$ nls -x -y git "commits on feature-login not on main"
→ git log --oneline main..feature-login
b44ce11 validate passwords
ca74955 add login form

$ nls -x -y git "show src/server.py from 1 commit ago"
→ git show HEAD~1:src/server.py
import os

PORT = 8080
HOST = "0.0.0.0"

$ nls git "undo last 2 commits keep changes staged"
git reset --soft HEAD~2

$ nls git "squash last 3 commits with message 'login flow'"
git reset --soft HEAD~3 && git commit -m 'login flow'

$ nls git "restore deleted file src/old.c"
git checkout "$(git rev-list -n 1 HEAD -- src/old.c)^" -- src/old.c

$ nls git "undo 5"
nls: not sure what '5' refers to; numbers are used in e.g. 'last 3 commits', '3 commits ago', 'last 7 days' or 'lines 10 to 20' (git_undo_soft)
```

The git examples show the traps people fall into by hand:
- **Soft, mixed or hard.** "undo last commit" is `reset --soft` (changes
  stay staged), "and unstage" is `--mixed`, and only "throw away" or
  "discard" gets `--hard`. Ties always go to the safe one, and "already
  pushed" switches to `git revert`.
- **`-S` vs `--grep`.** "commit that added 'x'" searches the code (`-S`);
  "commits mentioning 'x'" searches messages (`--grep`). They're easy to
  mix up and give completely different answers.
- **Which side of `..`?** "on feature-login not on main" is
  `main..feature-login`; swap them and you get the wrong list (often empty).
- **The one-liners nobody memorizes**: bringing back a deleted file
  (`rev-list -n 1 … ^`), deleting merged branches without touching
  main/master/develop, `push --force-with-lease` instead of `--force`,
  `HEAD@{1}` to undo a reset.
- **Nothing gets dropped**: if a command can't use part of the request
  ("by alice", "from main"), it's refused rather than run without it. A
  reset or restore with the wrong target is exactly the mistake people
  fear.

curl (status and timing hit the real sites; numbers vary by network):

```console
$ nls curl "post data.json to https://api.example.com/users"
curl -sS -X POST -H 'Content-Type: application/json' -d @data.json 'https://api.example.com/users'

$ nls -x -y curl "check response status of https://example.com"
→ curl -sSIL -o /dev/null -w '%{http_code}\n' 'https://example.com'
200

$ nls -x -y curl "how long does https://example.com take"
→ curl -sS -o /dev/null -w 'dns:        %{time_namelookup}s\nconnect:    %{time_connect}s\ntls:        %{time_appconnect}s\nfirst byte: %{time_starttransfer}s\ntotal:      %{time_total}s\n' 'https://example.com'
dns:        0.002579s
connect:    0.230809s
tls:        0.264128s
first byte: 0.297451s
total:      0.297618s

$ nls curl 'delete https://api.example.com/users/42 with token $TOKEN'
curl -sS -X DELETE -H "Authorization: Bearer $TOKEN" 'https://api.example.com/users/42'

$ nls curl "download https://example.com/file.zip as out.zip"
curl -fL 'https://example.com/file.zip' -o out.zip

$ nls curl "get json from api.github.com/users/octocat"
curl -sS -H 'Accept: application/json' 'https://api.github.com/users/octocat' | jq .

$ nls curl "get data.json to https://api.example.com/users"
nls: this request doesn't send a body ('data.json'); did you mean post, put or patch? (curl_get)
```

The curl examples show the traps people fall into by hand:
- **The JSON POST incantation**: `-X POST`, the `Content-Type` header, and
  `-d @file` (the `@` is what makes curl read the file instead of sending
  the literal text `data.json`). Non-JSON files like `.csv` use
  `--data-binary`, since `-d` strips their newlines.
- **Just the status code** needs `-o /dev/null -w '%{http_code}\n'`, and `-L`
  so a redirect doesn't report `301`.
- **Downloads that fail loudly.** `-f` makes a 404 an error instead of
  saving the error page as `file.zip`; `-L` follows the CDN redirect.
- **Tokens stay out of the command.** `token $TOKEN` becomes
  `-H "Authorization: Bearer $TOKEN"` (double quotes, so your shell fills it
  in), so the secret never lands in your history or a pasted snippet.
- **URLs are always quoted.** An unquoted `?a=1&b=2` backgrounds the command
  at the `&`; `[]` in a query string gets `-g` so curl doesn't treat it as a
  glob.
- **Nothing gets dropped**: a body on a GET, two URLs or a stray number is
  refused, not silently ignored.

ImageMagick, run in the demo folder (`make-demo-media.sh` draws the images):

```console
$ nls -x -y magick "resize hero.png to width 800 convert to webp quality 85"
→ magick hero.png -resize 800x -quality 85 hero.webp

$ nls -x -y magick "what are the dimensions of hero.webp"
→ magick identify -format '%wx%h\n' hero.webp
800x450

$ nls -x -y magick "make sprite.png transparent background"
→ magick sprite.png -fuzz 5% -transparent white sprite_transparent.png

$ nls -x -y magick "make a gif from frames/*.png at 12 fps"
→ magick -delay 8 -loop 0 'frames/*.png' animation.gif

$ nls magick "shrink hero.png to 1920x1080 only if larger"
magick hero.png -resize '1920x1080>' hero_resized.png

$ nls magick "mirror hero.png horizontally"
magick hero.png -flop hero_mirrored.png

$ nls magick "make a favicon from sprite.png"
magick sprite.png -background none -define icon:auto-resize=256,64,48,32,16 favicon.ico

$ nls magick "convert all png files in frames/ to webp"
magick mogrify -format webp 'frames/*.png'

$ nls magick "make sprite.png transparent as sprite.jpg"
nls: jpg can't store transparency; use png or webp (magick_transparent)

$ nls magick "convert @list.txt to webp"
nls: refusing '@list.txt': magick reads @file as a list of file names (magick_convert)
```

The ImageMagick examples show the traps people fall into by hand:
- **Geometry syntax.** "width 800" is `800x` (keep the aspect ratio),
  "exactly" adds `!`, and "only if larger" adds `>`. Both need quotes: an
  unquoted `>` is a shell redirect, so magick errors out and leaves an empty
  output file behind.
- **`-flip` vs `-flop`.** `-flip` turns the image upside down; a left-right
  mirror is `-flop`. "Mirror horizontally" gets the right one.
- **Never overwrite the original.** `mogrify` edits files in place, so batch
  resizes go to another folder (`-path resized`), and format conversions
  leave the originals next to the new files. Single images get a new name,
  with a suffix (`_resized`) only when the format doesn't change.
- **Frame delay is in 1/100 s.** "12 fps" becomes `-delay 8`, not `-delay 12`.
- **Globs are quoted** so magick expands `frames/*.png` itself (zsh would
  otherwise stop with "no matches found" when nothing matches).
- **Format traps are refused.** Transparency saved to a `.jpg` comes out with
  a solid background, so `nls` refuses that instead. `@list.txt` and `png:x`
  are magick syntax for "read a list of files" and "force a format", so file
  names like that are refused too.
- **Stripping EXIF keeps photos upright.** "remove exif metadata" runs
  `-auto-orient` first; plain `-strip` would drop the rotation tag and leave
  phone photos sideways.

## Who it's for

| Audience | Pain | Message |
|---|---|---|
| Developers who use ffmpeg/jq a few times a month | Forget the syntax every time | "Never look up ffmpeg flags again." |
| Anyone searching code or logs | `-l` vs `-L`, `--include` globs, context flags | "Ask for 'python files that don't mention TODO' and get the right grep." |
| Anyone cleaning up disks or repos | `-prune` syntax, `+`/`-` signs, fear of `-delete` | "Find what's eating your disk, and delete it without the scary part." |
| Anyone shipping releases or making backups | tar flag letters and order, excludes, `--strip-components` | "Say what goes in the archive; get the tar line right the first time." |
| Web developers and designers | Resize/convert/compress assets, favicons, sprites, `-flip` vs `-flop` | "Get web-ready images in one sentence, without overwriting the originals." |
| Backend developers testing APIs | `-X`/`-H`/`-d @file`, status-only output, auth headers | "Say what to send where; get the curl line right the first time." |
| Every developer using git | Fear of `reset`, `-S` vs `--grep`, `A..B` order, recovering deleted files | "Say what you want to undo; get the safe git command." |
| People who distrust AI shell tools | Hallucinated flags, data sent to the cloud, API cost | "Deterministic. Offline. Nothing leaves your machine." |
| C / systems programmers | Bloat | "250 KB, links only the system C library, about 6,700 lines of C11." |
| Content creators, podcasters | Need cuts, GIFs, audio extraction | "Cut, compress, GIF, extract audio in one sentence." |

## Differentiators (vs. AI command generators)

| | nls | LLM-based CLI helpers |
|---|---|---|
| Network / API key | None | Usually required |
| Same input → same output | Always | Not guaranteed |
| Latency | ~2 ms | Typically seconds |
| Your file names and data | Never leave the machine | Sent to a provider (unless self-hosted) |
| Invents flags that don't exist | Can't: every command comes from a tested template | Possible |
| Coverage | Limited to its templates (ffmpeg, jq, grep, find, tar, git, curl, ImageMagick for now) | Anything |

Be honest about the last row. "It does fewer things, but it gets them right"
is a stronger pitch than pretending it does everything.

## Fact sheet (measured 2026-11-24, v0.1.0, Apple Silicon Mac)

| Claim | Value | How to re-check |
|---|---|---|
| Binary size | 250 KB | `make && ls -l nls` |
| Dependencies | System C library only | `otool -L nls` (macOS) / `ldd nls` (Linux) |
| Source size | ~6,700 lines of C11 | `wc -l src/*` |
| Speed | ~1.7 ms per call, including process start | `time` a loop of 1,000 runs |
| Templates | 206 (30 ffmpeg, 47 jq, 9 grep, 7 find, 3 tar, 67 git, 13 curl, 30 magick) | `nls --list \| grep -c '^[a-z]*_'` |
| Tests | 789 (486 command-text, 59 run with real jq, 16 run with real grep, 29 run with real find on a generated tree, 19 run with real tar creating, listing and extracting archives, 54 run with real git in a throwaway repo, 31 run with real curl against a local test server, 57 run with real ImageMagick on generated images, checking size, format, transparency, EXIF and pixel colours, 38 run with real ffmpeg + ffprobe) | `make test` |
| Safety | Prints only, unless `-x`; `-x` asks first; file names and strings are shell-quoted; jq strings go through `--arg`; grep patterns go through `-e`; find refuses unfiltered deletes and anything it can't fully express; tar refuses mismatched compression, non-tar names and stray values, and a bare archive name only lists; git refuses anything it can't fully use, stray numbers, unquoted shell syntax and invalid branch names, and ties go to the safe variant (`reset --soft`, `branch -d`, `clean -n`); curl only accepts http(s) URLs from a strict allowlist, always single-quotes them, keeps `$TOKEN` secrets as variables, and refuses bodies on GETs, two URLs and stray values; magick never overwrites an input, refuses `-x`/`@list`/`png:x` file names, transparency into JPEG and anything it can't fully express | `README.md` → Safety |

Wording rules:
- Say "about 2 ms", not "sub-millisecond". The 1.7 ms includes starting the
  process.
- Say "no AI / no LLM", not "no machine learning hype". Keep it factual.
- Don't claim Windows support yet. The quoting is POSIX-only.
- Tested with macOS's BSD grep. The flags used (`-r -n -l -L -c -o -i -w -v
  -F -E -e -A -B -C -m --include`) also exist in GNU grep, but say "tested on
  macOS" until the suite has run on Linux.
- Same for find: tested with macOS's BSD find. Every primary used (`-type
  -name -iname -path -prune -size -mtime -mmin -atime -empty -maxdepth
  -mindepth -exec -delete`) also exists in GNU find.
- The line-count filter runs one awk per file. Don't call it fast on huge
  trees.
- tar: tested with macOS's bsdtar (libarchive 3.7). The options used (`-c -x
  -t -z -j -J -v -f -C --zstd --exclude --strip-components`) also exist in
  GNU tar, but GNU tar hasn't run the suite yet. `--zstd` needs the `zstd`
  program installed.
- Extracting "into dist/" uses `-C dist/`, which fails if the folder doesn't
  exist yet. Don't demo it on a missing folder.
- git: tested with git 2.55 on macOS. The commands use `git switch` and
  `git restore`, which need git 2.23 or newer (2019); say so if someone asks
  about old distros. `delete merged branches` uses `xargs -r`, available in
  GNU and current macOS xargs.
- git covers common workflows (undo, squash, search, blame, branches, stash,
  tags, recovery), not all of git. Don't say "any git command". There's no
  `push --force` (only `--force-with-lease`) and no remote management yet.
- curl: tested with curl 8.7 on macOS. `download … into dir/` uses
  `--output-dir`, which needs curl 7.73 or newer (2020), and the folder must
  exist. The status check uses `-I` (a HEAD request), as is common; a few
  APIs answer HEAD with `405`, so demo it on a normal website.
- "No network" means `nls` itself. Say "offline" about compiling the
  command, never about running a curl command.
- Single-quote curl prompts that contain `$TOKEN`, or the shell expands it
  before `nls` sees it (and the secret ends up in the command). Show the
  single quotes in every demo.
- ImageMagick: tested with ImageMagick 7.1 on macOS. Commands use the IM7
  `magick` entry point; on ImageMagick 6 only `convert`/`mogrify` exist, so
  say "ImageMagick 7" if asked. AVIF and HEIC need IM built with libheif
  (Homebrew's is).
- magick covers common image chores (resize, convert, compress, metadata,
  transparency, rotate/flip, crop/pad/trim, thumbnails, borders, strips,
  GIFs, favicons, folder batches), not all of ImageMagick. No text,
  watermarks or compositing yet.
- Never run the git demo in a real repo you care about: `make-demo-media.sh`
  builds a throwaway one in `marketing/demo/project/`.

## Before launching: checklist

- [ ] Public repo with the README and a license file (pick one: MIT is common for CLIs)
- [ ] Record the GIF: `sh marketing/make-demo-media.sh && vhs marketing/demo.tape`
- [ ] Put the GIF at the top of the main README
- [ ] Record the race: `vhs marketing/race.tape` (after the demo media), for the Show HN / X post
- [ ] Install instructions work on a fresh machine (`make install`, PATH note)
- [ ] A Homebrew tap (`brew install <you>/tap/nls`) lowers friction a lot
- [ ] Use only royalty-free media in demos. `make-demo-media.sh` generates
      it, so don't record with real songs or videos.
- [ ] Run `make test` on Linux (GNU grep and find) before claiming Linux support.
- [ ] Have 3–5 "what's next" items ready (grep `--exclude-dir` and piped
      input, jq AND conditions, find "modified after 2026-01-01" and
      "run X on each file", tar creating the destination folder and
      appending to archives, git "discard all local changes" and remote
      management, curl cookies and retries on specific status codes, magick
      watermarks and text, openssl next). Launch threads always ask.
