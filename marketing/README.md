# Marketing kit: nls

Everything needed to explain and launch `nls`. Every claim below was measured
on the project itself. Re-check the **Fact sheet** before each launch, since
numbers change as templates are added.

| File | What it is |
|---|---|
| `README.md` | Positioning, messaging, fact sheet (this file) |
| `launch-posts.md` | Ready-to-edit posts: Show HN, Reddit, X/Bluesky thread, LinkedIn |
| `demo.tape` | [VHS](https://github.com/charmbracelet/vhs) script that records the terminal demo GIF |
| `make-demo-media.sh` | Generates royalty-free demo files (`talk.mp4`, `demo.mp4`, `users.json`, a small `src/` tree, `app.log`) |

---

## One-liner

> **Plain English in, the exact ffmpeg, jq or grep command out. Offline, instant, no AI.**

Alternatives:
- "Stop googling ffmpeg flags."
- "`nls` compiles English into shell commands."
- "The ffmpeg/jq/grep cheat sheet you can talk to."
- "Search your code in plain English, without an AI reading it."

## Elevator pitch (30 seconds)

Everyone has googled "ffmpeg cut video" more than once, jq's syntax hasn't
gotten easier to remember, and nobody recalls whether "files that *don't*
match" is `grep -L` or `grep -lv`. `nls` lets you type what you want, like
`nls ffmpeg "extract the first 10 seconds of talk.mp4 into intro.mp4"` or
`nls grep "which python files contain TODO, ignoring case"`. You get back the
exact command, ready to run.

It isn't a chatbot. There's no API key, no network, and no model. A tiny C
program matches your words against a built-in library of command templates.
That means the same request always gives the same answer, in about 2 ms,
even on a plane.

## The demo that sells it

This run was not staged. It is real output:

```console
$ nls -x -y ffmpeg "extract from talk.mp4 the first 10 seconds into intro.mp4"
→ ffmpeg -i talk.mp4 -t 10 -c copy intro.mp4
```

Why it lands:
- **No parsing rules to learn.** The prompt is messy, natural English.
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

## Who it's for

| Audience | Pain | Message |
|---|---|---|
| Developers who use ffmpeg/jq a few times a month | Forget the syntax every time | "Never look up ffmpeg flags again." |
| Anyone searching code or logs | `-l` vs `-L`, `--include` globs, context flags | "Ask for 'python files that don't mention TODO' and get the right grep." |
| People who distrust AI shell tools | Hallucinated flags, data sent to the cloud, API cost | "Deterministic. Offline. Nothing leaves your machine." |
| C / systems programmers | Bloat | "100 KB, links only the system C library, about 2,400 lines of C11." |
| Content creators, podcasters | Need cuts, GIFs, audio extraction | "Cut, compress, GIF, extract audio in one sentence." |

## Differentiators (vs. AI command generators)

| | nls | LLM-based CLI helpers |
|---|---|---|
| Network / API key | None | Usually required |
| Same input → same output | Always | Not guaranteed |
| Latency | ~2 ms | Typically seconds |
| Your file names and data | Never leave the machine | Sent to a provider (unless self-hosted) |
| Invents flags that don't exist | Can't: every command comes from a tested template | Possible |
| Coverage | Limited to its templates (ffmpeg, jq, grep for now) | Anything |

Be honest about the last row. "It does fewer things, but it gets them right"
is a stronger pitch than pretending it does everything.

## Fact sheet (measured 2026-11-23, v0.1.0, Apple Silicon Mac)

| Claim | Value | How to re-check |
|---|---|---|
| Binary size | 100 KB | `make && ls -l nls` |
| Dependencies | System C library only | `otool -L nls` (macOS) / `ldd nls` (Linux) |
| Source size | ~2,400 lines of C11 | `wc -l src/*` |
| Speed | ~1.9 ms per call, including process start | `time` a loop of 1,000 runs |
| Templates | 86 (30 ffmpeg, 47 jq, 9 grep) | `nls --list \| grep -c '^[a-z]*_'` |
| Tests | 238 (125 command-text, 59 run with real jq, 16 run with real grep, 38 run with real ffmpeg + ffprobe) | `make test` |
| Safety | Prints only, unless `-x`; `-x` asks first; file names and strings are shell-quoted; jq strings go through `--arg`; grep patterns go through `-e` | `README.md` → Safety |

Wording rules:
- Say "about 2 ms", not "sub-millisecond". The 1.9 ms includes starting the
  process.
- Say "no AI / no LLM", not "no machine learning hype". Keep it factual.
- Don't claim Windows support yet. The quoting is POSIX-only.
- Tested with macOS's BSD grep. The flags used (`-r -n -l -L -c -o -i -w -v
  -F -E -e -A -B -C -m --include`) also exist in GNU grep, but say "tested on
  macOS" until the suite has run on Linux.

## Before launching: checklist

- [ ] Public repo with the README and a license file (pick one: MIT is common for CLIs)
- [ ] Record the GIF: `sh marketing/make-demo-media.sh && vhs marketing/demo.tape`
- [ ] Put the GIF at the top of the main README
- [ ] Install instructions work on a fresh machine (`make install`, PATH note)
- [ ] A Homebrew tap (`brew install <you>/tap/nls`) lowers friction a lot
- [ ] Use only royalty-free media in demos. `make-demo-media.sh` generates
      it, so don't record with real songs or videos.
- [ ] Run `make test` on Linux (GNU grep) before claiming Linux support.
- [ ] Have 3–5 "what's next" items ready (grep `--exclude-dir` and piped
      input, jq AND conditions, nested arrays, more tools). Launch threads
      always ask.
