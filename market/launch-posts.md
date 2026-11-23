# Launch posts

Drafts to edit, not to paste as-is. Replace `<repo-url>` and `<demo.gif>`.
Each platform has different norms. Follow them, or the post gets ignored or
removed.

---

## Show HN

**Title** (80 chars max, no hype words):

> Show HN: nls – English to ffmpeg/jq commands, offline, no LLM, 86 KB of C

**Body:**

> I keep forgetting ffmpeg flags and jq syntax, and I didn't want an AI tool
> that sends my file names to an API and sometimes invents flags. So I built
> `nls`, a small C11 program that turns plain English into the exact command:
>
>     $ nls ffmpeg "extract from talk.mp4 the first 10 seconds into intro.mp4"
>     ffmpeg -i talk.mp4 -t 10 -c copy intro.mp4
>
>     $ nls jq "in users.json how many users have emails"
>     jq 'map(select(.email != null)) | length' users.json
>
> How it works: a lexer pulls out typed values (files, timestamps,
> resolutions, JSON paths). A scorer then picks the best of ~77 built-in
> templates, and an expander fills it in with safe shell quoting. For jq it
> peeks at the input file to learn its field names, which is why "emails"
> becomes `.email`. No model, no network, so the same input always gives the
> same output in about 2 ms.
>
> It only prints the command unless you pass `-x`, and even then it asks first.
>
> Limits: only ffmpeg and jq so far, POSIX shells only, and jq filters
> can't combine conditions yet. The test suite runs every generated command
> against real ffmpeg/jq and checks the output with ffprobe.
>
> I'd love to hear which commands you'd want it to know. <repo-url>

Tips: post on a weekday morning (US time). Reply to every comment in the
first 2 hours. Don't ask for upvotes.

---

## Reddit: r/commandline (also r/ffmpeg, r/C_Programming with a C-focused angle)

**Title:**

> I made a tiny offline tool that turns English into ffmpeg/jq commands (no AI, ~2 ms)

**Body:**

> `nls ffmpeg "make a gif from demo.mp4 between 2 and 5 seconds"` prints the
> full palette-optimized ffmpeg command. `nls jq "get names from users.json
> where age > 30"` prints `jq 'map(select(.age > 30) | .name)' users.json`.
>
> It's 86 KB of C with no dependencies. There's no LLM: it matches your words
> against a set of tested templates, so it can't make up flags. It prints by
> default; `-x` runs the command after asking you.
>
> <demo.gif>
>
> Source + install: <repo-url>. What commands do you look up most often?
> I'll add them.

For **r/C_Programming**, lead with the implementation instead: "C11, standard
library only, fixed-size arena buffers, no heap allocation in the parse
path". Ask for code review.

---

## X / Bluesky thread

> 1/ I got tired of googling ffmpeg flags, so I built `nls`: plain English in,
> exact command out. Offline. No AI. 86 KB. 🧵
> <demo.gif>
>
> 2/ `nls ffmpeg "extract from talk.mp4 the first 10 seconds into intro.mp4"`
> → `ffmpeg -i talk.mp4 -t 10 -c copy intro.mp4`
> It even picks `-c copy`, so there's no re-encode.
>
> 3/ jq too: `nls jq "in users.json how many users have emails"`
> → `jq 'map(select(.email != null)) | length' users.json`
> It reads the file's fields, so "emails" means `.email`.
>
> 4/ Why no LLM? Same input always gives the same output, about 2 ms, nothing
> leaves your machine, and it can't invent flags. Every command comes from a
> template that's tested against real ffmpeg/jq.
>
> 5/ It prints by default. `-x` runs it after asking. Free and open source:
> <repo-url>

---

## LinkedIn

> Small side project I'm proud of: **nls**, a natural-language shell.
>
> You type `nls ffmpeg "extract from talk.mp4 the first 10 seconds into
> intro.mp4"` and get `ffmpeg -i talk.mp4 -t 10 -c copy intro.mp4`.
>
> The interesting part is what it *doesn't* use: no LLM, no API, no network.
> It's about 2,000 lines of C that compile English against a library of
> tested command templates. That makes it deterministic and fast (~2 ms), and
> private by default.
>
> A few things I learned building it:
> • Testing generated commands as text wasn't enough. Running them against
>   real ffmpeg and jq found bugs that looked correct on paper.
> • "Does fewer things, but gets them right" is a real design choice next
>   to AI tools.
> • Small UX details matter most. "from 1 to 3 seconds" has to mean the
>   range, not "from 3".
>
> Open source: <repo-url>
>
> #opensource #cli #ffmpeg #c
