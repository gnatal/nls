# Launch posts

Drafts to edit, not to paste as-is. Replace `<repo-url>` and `<demo.gif>`.
Each platform has different norms. Follow them, or the post gets ignored or
removed.

---

## Show HN

**Title** (80 chars max, no hype words):

> Show HN: nls – English to ffmpeg/jq/grep/find/tar/git commands, offline, no LLM

**Body:**

> I keep forgetting ffmpeg flags, jq syntax, which grep flag lists the
> files that *don't* match, how find's `-prune` works, which tar letter
> means xz, and whether undoing a commit is `reset --soft` or `--hard`. I
> didn't want an AI tool
> that sends my file names to an API and sometimes invents flags. So I built
> `nls`, a small C11 program that turns plain English into the exact command:
>
>     $ nls ffmpeg "extract from talk.mp4 the first 10 seconds into intro.mp4"
>     ffmpeg -i talk.mp4 -t 10 -c copy intro.mp4
>
>     $ nls jq "in users.json how many users have emails"
>     jq 'map(select(.email != null)) | length' users.json
>
>     $ nls grep "which python files contain TODO, ignoring case"
>     grep -rlEi --include='*.py' -e TODO .
>
>     $ nls find "files larger than 5MB ignoring node_modules"
>     find . -path '*/node_modules' -prune -o -type f -size +5M -print
>
>     $ nls tar "extract release.tgz into dist/ without the top-level folder"
>     tar -xzvf release.tgz -C dist/ --strip-components=1
>
>     $ nls git "undo last 2 commits keep changes staged"
>     git reset --soft HEAD~2
>
>     $ nls git "commits on feature-login not on main"
>     git log --oneline main..feature-login
>
> How it works: a lexer pulls out typed values (files, timestamps,
> resolutions, sizes, ages, JSON paths, archive names, branches and
> revisions). A scorer then picks the best of ~160 built-in
> templates, and an expander fills it in with safe shell quoting. For jq it
> peeks at the input file to learn its field names, which is why "emails"
> becomes `.email`. No model, no network, so the same input always gives the
> same output in about 2 ms.
>
> It only prints the command unless you pass `-x`, and even then it asks first.
> For find it's stricter still: `delete files` with no filter is refused, and
> if it can't express part of your request (say, "older than 30 days") it
> refuses rather than running a broader delete. tar works the same way:
> `backup.tar.gz with xz` is refused instead of writing an xz archive with a
> gzip name, and a bare archive name only lists, never extracts. For git,
> ties go to the safe variant ("undo last commit" is `reset --soft`, never
> `--hard`), and if it can't use part of your request ("from main", "by
> alice") it refuses instead of resetting or restoring the wrong thing.
>
> Limits: only ffmpeg, jq, grep, find, tar and git so far (git means common
> workflows, not all of git), POSIX shells only, jq filters can't combine
> conditions yet, and grep can't exclude directories yet. The test suite
> runs the generated commands against real ffmpeg, jq, grep, find, tar and
> git (deletes, extractions and history rewrites included, on throwaway
> trees and repos), and checks ffmpeg's output with ffprobe.
>
> I'd love to hear which commands you'd want it to know. <repo-url>

Tips: post on a weekday morning (US time). Reply to every comment in the
first 2 hours. Don't ask for upvotes.

---

## Reddit: r/commandline (also r/ffmpeg, r/C_Programming with a C-focused angle)

**Title:**

> I made a tiny offline tool that turns English into ffmpeg/jq/grep/find/tar/git commands (no AI, ~2 ms)

**Body:**

> `nls ffmpeg "make a gif from demo.mp4 between 2 and 5 seconds"` prints the
> full palette-optimized ffmpeg command. `nls jq "get names from users.json
> where age > 30"` prints `jq 'map(select(.age > 30) | .name)' users.json`.
> `nls grep "list files not containing 'license' in src/"` prints
> `grep -rLE -e license src/`. And `nls find "delete all node_modules
> directories"` prints
> `find . -mindepth 1 -type d -name node_modules -prune -exec rm -rf {} +`.
> And `nls tar "compress src/ into backup.tar.gz excluding .git and
> node_modules"` prints
> `tar --exclude='.git' --exclude='node_modules' -czvf backup.tar.gz src/`.
> And `nls git "find commit that added 'def retry'"` prints
> `git log -S 'def retry' --oneline`.
>
> It's 188 KB of C with no dependencies. There's no LLM: it matches your words
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
> exact command out. Offline. No AI. 188 KB. 🧵
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
> 4/ And grep: `nls grep "which python files contain TODO, ignoring case"`
> → `grep -rlEi --include='*.py' -e TODO .`
> "files not containing X" correctly becomes `-L`, not `-lv`.
>
> 5/ And find, minus the Stack Overflow trip:
> `nls find "files larger than 5MB ignoring node_modules"`
> → `find . -path '*/node_modules' -prune -o -type f -size +5M -print`
> Deletes with no filter are refused, so "delete files" can't wipe a folder.
>
> 6/ And tar, without the "which letter is xz?" moment:
> `nls tar "extract release.tgz into dist/ without the top-level folder"`
> → `tar -xzvf release.tgz -C dist/ --strip-components=1`
> The compression comes from the file name, and `-f` always goes last.
>
> 7/ And git, for the commands you're scared to type:
> `nls git "undo last 2 commits keep changes staged"` → `git reset --soft HEAD~2`
> `nls git "commits on feature-login not on main"` → `git log --oneline main..feature-login`
> "undo" picks `--soft` unless you say "throw away", and "already pushed"
> switches to `git revert`.
>
> 8/ Why no LLM? Same input always gives the same output, about 2 ms, nothing
> leaves your machine, and it can't invent flags. Every command comes from a
> template that's tested against real ffmpeg, jq, grep, find, tar and git.
>
> 9/ It prints by default. `-x` runs it after asking. Free and open source:
> <repo-url>

---

## LinkedIn

> Small side project I'm proud of: **nls**, a natural-language shell.
>
> You type `nls ffmpeg "extract from talk.mp4 the first 10 seconds into
> intro.mp4"` and get `ffmpeg -i talk.mp4 -t 10 -c copy intro.mp4`. It does
> the same for jq, grep, find, tar and git: `nls grep "which python files
> contain TODO"` gives `grep -rlE --include='*.py' -e TODO .`,
> `nls find "log files older than 30 days"` gives
> `find . -type f -name '*.log' -mtime +30`,
> `nls tar "list contents of backup.tar.gz"` gives `tar -tzvf backup.tar.gz`,
> and `nls git "squash last 3 commits with message 'login flow'"` gives
> `git reset --soft HEAD~3 && git commit -m 'login flow'`.
>
> The interesting part is what it *doesn't* use: no LLM, no API, no network.
> It's about 4,900 lines of C that compile English against a library of
> tested command templates. That makes it deterministic and fast (~2 ms), and
> private by default.
>
> A few things I learned building it:
> • Testing generated commands as text wasn't enough. Running them against
>   real ffmpeg, jq, grep, find, tar and git found bugs that looked correct on paper.
> • "Does fewer things, but gets them right" is a real design choice next
>   to AI tools.
> • Small UX details matter most. "from 1 to 3 seconds" has to mean the
>   range, not "from 3".
> • Refusing beats guessing. If a cleanup command can't express everything
>   you asked for, running a broader delete is the worst possible answer.
>
> Open source: <repo-url>
>
> #opensource #cli #ffmpeg #grep #find #git #c
