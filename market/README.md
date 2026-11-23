# Marketing kit: nls

Everything needed to explain and launch `nls`. Every claim below was measured
on the project itself. Re-check the **Fact sheet** before each launch, since
numbers change as templates are added.

| File | What it is |
|---|---|
| `README.md` | Positioning, messaging, fact sheet (this file) |
| `launch-posts.md` | Ready-to-edit posts: Show HN, Reddit, X/Bluesky thread, LinkedIn |
| `demo.tape` | [VHS](https://github.com/charmbracelet/vhs) script that records the terminal demo GIF |
| `make-demo-media.sh` | Generates royalty-free demo files (`sample.mp4`, `users.json`) |

---

## One-liner

> **Plain English in, the exact ffmpeg or jq command out. Offline, instant, no AI.**

Alternatives:
- "Stop googling ffmpeg flags."
- "`nls` compiles English into shell commands."
- "The ffmpeg/jq cheat sheet you can talk to."

## Elevator pitch (30 seconds)

Everyone has googled "ffmpeg cut video" more than once, and jq's syntax
hasn't gotten easier to remember either. `nls` lets you type what you want,
like `nls ffmpeg "extract the first 10 seconds of talk.mp4 into intro.mp4"`.
You get back the exact command, ready to run.

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
```

The jq example works because `nls` reads the file's field names: "emails"
becomes `.email` on its own.

## Who it's for

| Audience | Pain | Message |
|---|---|---|
| Developers who use ffmpeg/jq a few times a month | Forget the syntax every time | "Never look up ffmpeg flags again." |
| People who distrust AI shell tools | Hallucinated flags, data sent to the cloud, API cost | "Deterministic. Offline. Nothing leaves your machine." |
| C / systems programmers | Bloat | "86 KB, links only the system C library, about 2,000 lines of C11." |
| Content creators, podcasters | Need cuts, GIFs, audio extraction | "Cut, compress, GIF, extract audio in one sentence." |

## Differentiators (vs. AI command generators)

| | nls | LLM-based CLI helpers |
|---|---|---|
| Network / API key | None | Usually required |
| Same input → same output | Always | Not guaranteed |
| Latency | ~2 ms | Typically seconds |
| Your file names and data | Never leave the machine | Sent to a provider (unless self-hosted) |
| Invents flags that don't exist | Can't: every command comes from a tested template | Possible |
| Coverage | Limited to its templates (ffmpeg, jq for now) | Anything |

Be honest about the last row. "It does fewer things, but it gets them right"
is a stronger pitch than pretending it does everything.

## Fact sheet (measured 2026-11-23, v0.1.0, Apple Silicon Mac)

| Claim | Value | How to re-check |
|---|---|---|
| Binary size | 86 KB | `make && ls -l nls` |
| Dependencies | System C library only | `otool -L nls` (macOS) / `ldd nls` (Linux) |
| Source size | ~2,000 lines of C11 | `wc -l src/*` |
| Speed | ~1.7 ms per call, including process start | `time` a loop of 1,000 runs |
| Templates | 77 (30 ffmpeg, 47 jq) | `nls --list \| grep -c '^[a-z]*_'` |
| Tests | 194 (97 command-text, 59 run with real jq, 38 run with real ffmpeg + ffprobe) | `make test` |
| Safety | Prints only, unless `-x`; `-x` asks first; file names and strings are shell-quoted; jq strings go through `--arg` | `README.md` → Safety |

Wording rules:
- Say "about 2 ms", not "sub-millisecond". The 1.7 ms includes starting the
  process.
- Say "no AI / no LLM", not "no machine learning hype". Keep it factual.
- Don't claim Windows support yet. The quoting is POSIX-only.

## Before launching: checklist

- [ ] Public repo with the README and a license file (pick one: MIT is common for CLIs)
- [ ] Record the GIF: `sh market/make-demo-media.sh && vhs market/demo.tape`
- [ ] Put the GIF at the top of the main README
- [ ] Install instructions work on a fresh machine (`make install`, PATH note)
- [ ] A Homebrew tap (`brew install <you>/tap/nls`) lowers friction a lot
- [ ] Use only royalty-free media in demos. `make-demo-media.sh` generates
      it, so don't record with real songs or videos.
- [ ] Have 3–5 "what's next" items ready (grep/grop support, AND conditions,
      nested arrays). Launch threads always ask.
