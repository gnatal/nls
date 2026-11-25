/*
 * explain.c — --explain: break a generated command down part by part.
 *
 *   ffmpeg -ss 00:10 -i in.mp4 -c copy in_cut.mp4
 *
 *     ffmpeg              convert, cut and filter video and audio
 *     -ss 00:10           start at 00:10
 *     -i in.mp4           the input file
 *     -c copy             copy the streams as they are: fast, no quality loss
 *     in_cut.mp4          the output file (its extension sets the format)
 *
 * The command is split like a shell would (quotes, backslashes, |, &&, ;),
 * then each command's words are looked up in OPTS[] by command (a git
 * subcommand has its own entries, "git log", before the plain "git" ones).
 * Bundled short options (-czvf) are explained letter by letter. Values with
 * structure of their own are decoded: jq programs, ffmpeg filter chains,
 * magick geometries, find sizes and ages, git revisions.
 *
 * A part with no explanation is shown with NL_NO_EXPLANATION; the tests
 * check that no command nls can generate contains one.
 */
#include "nl.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_WORDS   160
#define MAX_SEGS    12
#define DESC_COL    24     /* where descriptions start                    */
#define LINE_WIDTH  79

/* ------------------------------------------------------------------ */
/* Splitting the command like a shell                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *raw;    /* as written: 'my file.mp4'                    */
    const char *val;    /* as the program sees it: my file.mp4          */
} Word;

typedef struct {
    const char *op;     /* what joins it to the previous one: "|", "&&", ";" */
    size_t first, n;    /* its words: words[first .. first+n)           */
} Seg;

typedef struct {
    Word w[MAX_WORDS];
    size_t nw;
    Seg seg[MAX_SEGS];
    size_t nseg;
    char store[NL_MAX_CMD_LEN * 3];
    size_t used;
} Parsed;

static char *store_put(Parsed *p, const char *s, size_t n) {
    if (p->used + n + 1 > sizeof p->store) return NULL;
    char *d = p->store + p->used;
    memcpy(d, s, n);
    d[n] = '\0';
    p->used += n + 1;
    return d;
}

static bool split_command(const char *cmd, Parsed *p) {
    memset(p, 0, sizeof *p);
    const char *s = cmd;
    p->seg[0].op = NULL;
    p->nseg = 1;
    char val[NL_MAX_CMD_LEN];
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '\n') s++;
        if (!*s) break;
        const char *op = NULL;
        if (strncmp(s, "&&", 2) == 0) op = "&&";
        else if (strncmp(s, "||", 2) == 0) op = "||";
        else if (*s == '|') op = "|";
        else if (*s == ';') op = ";";
        if (op) {
            if (p->nseg >= MAX_SEGS) return false;
            p->seg[p->nseg].op = op;
            p->seg[p->nseg].first = p->nw;
            p->nseg++;
            s += strlen(op);
            continue;
        }
        const char *start = s;
        size_t n = 0;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '|' && *s != ';' &&
               !(s[0] == '&' && s[1] == '&')) {
            if (*s == '\'') {
                s++;
                while (*s && *s != '\'') { if (n + 1 < sizeof val) val[n++] = *s; s++; }
                if (*s) s++;
            } else if (*s == '"') {
                s++;
                int depth = 0;   /* "$(...)" may hold quotes of its own */
                while (*s && (*s != '"' || depth > 0)) {
                    if (s[0] == '$' && s[1] == '(') depth++;
                    else if (*s == ')' && depth > 0) depth--;
                    if (*s == '\\' && s[1] && strchr("\"\\$`", s[1])) s++;
                    if (n + 1 < sizeof val) val[n++] = *s;
                    s++;
                }
                if (*s) s++;
            } else if (*s == '\\' && s[1]) {
                if (n + 1 < sizeof val) val[n++] = s[1];
                s += 2;
            } else {
                if (n + 1 < sizeof val) val[n++] = *s;
                s++;
            }
        }
        if (p->nw >= MAX_WORDS) return false;
        Word *w = &p->w[p->nw];
        w->raw = store_put(p, start, (size_t)(s - start));
        w->val = store_put(p, val, n);
        if (!w->raw || !w->val) return false;
        p->nw++;
        p->seg[p->nseg - 1].n++;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Output                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    FILE *out;
    int indent;         /* extra indent of the code column (nested commands) */
} Ex;

/* Print `text` from column `col` on, wrapped at LINE_WIDTH, continuing at
 * `col`. The cursor is at column `at`. */
static void wrap(FILE *out, const char *text, int col, int at) {
    if (at > col) { fputc('\n', out); at = 0; }
    fprintf(out, "%*s", col - at, "");
    int pos = col;
    const char *p = text;
    while (*p) {
        size_t n = strcspn(p, " ");
        if (pos > col && pos + 1 + (int)n > LINE_WIDTH) {
            fprintf(out, "\n%*s", col, "");
            pos = col;
        } else if (pos > col) {
            fputc(' ', out);
            pos++;
        }
        fwrite(p, 1, n, out);
        pos += (int)n;
        p += n;
        while (*p == ' ') p++;
    }
    fputc('\n', out);
}

static void row(Ex *x, const char *code, const char *fmt, ...) {
    char desc[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(desc, sizeof desc, fmt, ap);
    va_end(ap);
    int at = 2 + x->indent;
    fprintf(x->out, "%*s%s", at, "", code);
    at += (int)strlen(code);
    wrap(x->out, desc, DESC_COL + x->indent, at + 1 > DESC_COL + x->indent ? at + 1 : at);
}

/* A row for a word and the arguments it took: "-ss 00:10". */
static void row_words(Ex *x, const Word *w, size_t n, const char *desc) {
    char code[512];
    size_t len = 0;
    code[0] = '\0';
    for (size_t k = 0; k < n && len < sizeof code; k++)
        len += (size_t)snprintf(code + len, sizeof code - len, "%s%s", k ? " " : "", w[k].raw);
    row(x, code, "%s", desc);
}

/* `what` with %1 / %2 replaced by the values. */
static void fill(char *out, size_t size, const char *what, const char *a1, const char *a2) {
    size_t n = 0;
    for (const char *p = what; *p && n + 1 < size; p++) {
        if (p[0] == '%' && (p[1] == '1' || p[1] == '2')) {
            const char *v = p[1] == '1' ? a1 : a2;
            n += (size_t)snprintf(out + n, size - n, "%s", v ? v : "");
            if (n >= size) n = size - 1;
            p++;
        } else {
            out[n++] = *p;
        }
    }
    out[n] = '\0';
}

/* A growing string for decoded values. */
typedef struct { char s[1024]; size_t n; bool bad; } Buf;

static void bput(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(b->s + b->n, sizeof b->s - b->n, fmt, ap);
    va_end(ap);
    if (k < 0) return;
    b->n += (size_t)k;
    if (b->n >= sizeof b->s) b->n = sizeof b->s - 1;
}

/* ------------------------------------------------------------------ */
/* Option tables                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *cmd;    /* "ffmpeg", "git log", ...                       */
    const char *flag;   /* "-ss"; a subcommand for "git" / "magick" too   */
    int nargs;          /* words it takes after it (0-2), or after '='    */
    const char *match;  /* NULL, or the exact first value this entry is for */
    const char *what;   /* %1, %2: the values                             */
} Opt;

static const Opt OPTS[] = {
    /* ---------------- ffmpeg ---------------- */
    { "ffmpeg", "-ss", 1, NULL, "start at %1 (seconds or [hh:]mm:ss)" },
    { "ffmpeg", "-to", 1, NULL, "stop at %1" },
    { "ffmpeg", "-t", 1, NULL, "keep %1 seconds" },
    { "ffmpeg", "-sseof", 1, NULL, "start %1 seconds before the end" },
    { "ffmpeg", "-i", 1, NULL, "the input file" },
    { "ffmpeg", "-c", 1, "copy", "copy the audio and video as they are: fast, no quality loss" },
    { "ffmpeg", "-c:v", 1, "copy", "copy the video as it is (no re-encoding)" },
    { "ffmpeg", "-c:v", 1, "libx265", "encode the video as H.265/HEVC" },
    { "ffmpeg", "-c:v", 1, "libx264", "encode the video as H.264, which plays almost everywhere" },
    { "ffmpeg", "-c:a", 1, "copy", "copy the audio as it is (no re-encoding)" },
    { "ffmpeg", "-c:a", 1, "aac", "encode the audio as AAC" },
    { "ffmpeg", "-c:a", 1, "libmp3lame", "encode the audio as MP3" },
    { "ffmpeg", "-crf", 1, NULL, "quality %1: lower is better and bigger (0-51; 18 looks "
                                 "lossless, 28 is a good size)" },
    { "ffmpeg", "-preset", 1, NULL, "encoding speed %1: slower presets give smaller files at "
                                    "the same quality" },
    { "ffmpeg", "-tag:v", 1, "hvc1", "tag the video hvc1 so Apple players open it" },
    { "ffmpeg", "-b:a", 1, NULL, "audio bitrate %1 (bits per second; higher is better)" },
    { "ffmpeg", "-an", 0, NULL, "drop the audio" },
    { "ffmpeg", "-vn", 0, NULL, "drop the video (keep only the audio)" },
    { "ffmpeg", "-frames:v", 1, NULL, "write %1 video frame(s), then stop" },
    { "ffmpeg", "-loop", 1, "0", "loop the GIF forever" },
    { "ffmpeg", "-hide_banner", 0, NULL, "skip ffmpeg's version and build banner" },
    { "ffprobe", "-hide_banner", 0, NULL, "skip the version and build banner" },

    /* ---------------- jq ---------------- */
    { "jq", "-r", 0, NULL, "raw output: print strings without JSON quotes" },
    { "jq", "-c", 0, NULL, "compact output: one line per value" },
    { "jq", "-s", 0, NULL, "slurp: read every input value (e.g. each line of a .jsonl) "
                           "into one array" },
    { "jq", "--arg", 2, NULL, "set $%1 to the string \"%2\" (passed as data, never as code)" },

    /* ---------------- grep ---------------- */
    { "grep", "-r", 0, NULL, "search folders recursively" },
    { "grep", "-n", 0, NULL, "show line numbers" },
    { "grep", "-E", 0, NULL, "extended regular expressions (+, ?, | and () work unescaped)" },
    { "grep", "-F", 0, NULL, "fixed string: the pattern is plain text, not a regex" },
    { "grep", "-i", 0, NULL, "ignore case" },
    { "grep", "-w", 0, NULL, "whole words only" },
    { "grep", "-v", 0, NULL, "invert: keep the lines that do NOT match" },
    { "grep", "-l", 0, NULL, "list only the names of files with a match" },
    { "grep", "-L", 0, NULL, "list only the names of files with no match" },
    { "grep", "-c", 0, NULL, "count the matching lines in each file" },
    { "grep", "-h", 0, NULL, "don't print file names" },
    { "grep", "-o", 0, NULL, "print only the matched text, one match per line" },
    { "grep", "-A", 1, NULL, "also show %1 lines after each match" },
    { "grep", "-B", 1, NULL, "also show %1 lines before each match" },
    { "grep", "-C", 1, NULL, "also show %1 lines around each match" },
    { "grep", "-m", 1, NULL, "stop after %1 matching line(s) per file" },
    { "grep", "-e", 1, NULL, "the pattern to search for (after -e, even a leading - is "
                             "a pattern)" },
    { "grep", "--include", 1, NULL, "only search files named like %1" },

    /* ---------------- find ---------------- */
    { "find", "-type", 1, "f", "only regular files" },
    { "find", "-type", 1, "d", "only directories" },
    { "find", "-type", 1, "l", "only symbolic links" },
    { "find", "-name", 1, NULL, "whose name matches %1" },
    { "find", "-iname", 1, NULL, "whose name matches %1, ignoring case" },
    { "find", "-path", 1, NULL, "whose path matches %1" },
    { "find", "-prune", 0, NULL, "don't go inside it" },
    { "find", "-o", 0, NULL, "or (what follows applies to everything else)" },
    { "find", "-maxdepth", 1, NULL, "go at most %1 level(s) down" },
    { "find", "-mindepth", 1, "1", "skip the starting folder itself" },
    { "find", "-mindepth", 1, NULL, "skip the first %1 level(s)" },
    { "find", "-empty", 0, NULL, "that are empty" },
    { "find", "-print", 0, NULL, "print the path" },
    { "find", "-delete", 0, NULL, "delete it, for good" },
    { "find", "(", 0, NULL, "group start: either of the tests until \\)" },
    { "find", ")", 0, NULL, "group end" },
    { "find", "{}", 0, NULL, "the path found" },

    /* commands run by find -exec and in pipelines */
    { "du", "-h", 0, NULL, "sizes in K, M, G" },
    { "sort", "-r", 0, NULL, "largest first" },
    { "sort", "-h", 0, NULL, "compare sizes like 2K < 1M" },
    { "head", "-n", 1, NULL, "keep the first %1 line(s)" },
    { "wc", "-l", 0, NULL, "count lines" },
    { "rm", "-r", 0, NULL, "recursively: folders and everything in them" },
    { "rm", "-f", 0, NULL, "don't ask, and don't complain about missing files" },
    { "mkdir", "-p", 0, NULL, "create parent folders too; no error if it already exists" },
    { "xargs", "-r", 0, NULL, "don't run it at all when there is no input" },

    /* ---------------- tar ---------------- */
    { "tar", "-c", 0, NULL, "create a new archive" },
    { "tar", "-x", 0, NULL, "extract files from an archive" },
    { "tar", "-t", 0, NULL, "list what the archive contains" },
    { "tar", "-z", 0, NULL, "gzip (.tar.gz) compression" },
    { "tar", "-j", 0, NULL, "bzip2 (.tar.bz2) compression" },
    { "tar", "-J", 0, NULL, "xz (.tar.xz) compression" },
    { "tar", "-v", 0, NULL, "verbose: list each file as it goes" },
    { "tar", "-f", 1, NULL, "the archive file" },
    { "tar", "-C", 1, NULL, "in the folder %1" },
    { "tar", "--zstd", 0, NULL, "zstd (.tar.zst) compression" },
    { "tar", "--exclude", 1, NULL, "leave out anything named like %1" },
    { "tar", "--strip-components", 1, NULL, "drop the first %1 folder level(s) from each path" },

    /* ---------------- git ---------------- */
    { "git", "add", 0, NULL, "stage changes for the next commit" },
    { "git", "blame", 0, NULL, "show who last changed each line, and in which commit" },
    { "git", "branch", 0, NULL, "list, create, rename or delete branches" },
    { "git", "checkout", 0, NULL, "check out files (here: from another commit)" },
    { "git", "cherry-pick", 0, NULL, "copy a commit onto the current branch" },
    { "git", "clean", 0, NULL, "remove untracked files" },
    { "git", "commit", 0, NULL, "record the staged changes as a commit" },
    { "git", "diff", 0, NULL, "show changes, line by line" },
    { "git", "fetch", 0, NULL, "download new commits from the remote (changes no files)" },
    { "git", "log", 0, NULL, "show the commit history" },
    { "git", "merge", 0, NULL, "join another branch's history into this one" },
    { "git", "pull", 0, NULL, "fetch from the remote and integrate it" },
    { "git", "push", 0, NULL, "upload commits to the remote" },
    { "git", "rebase", 0, NULL, "replay your commits on top of another base" },
    { "git", "reflog", 0, NULL, "show where HEAD has been, lost commits included" },
    { "git", "reset", 0, NULL, "move the current branch to another commit" },
    { "git", "restore", 0, NULL, "restore files (or unstage them)" },
    { "git", "rev-list", 0, NULL, "list commit hashes" },
    { "git", "revert", 0, NULL, "undo commits by adding new commits that reverse them" },
    { "git", "rm", 0, NULL, "remove files from git" },
    { "git", "show", 0, NULL, "show a commit, or a file at a commit" },
    { "git", "stash", 0, NULL, "set changes aside without committing them" },
    { "git", "switch", 0, NULL, "change branches" },
    { "git", "tag", 0, NULL, "list, create or delete tags" },
    { "git config", "user.name", 0, NULL, "your configured name" },

    { "git add", "-A", 0, NULL, "everything: new, changed and deleted files" },
    { "git blame", "-L", 1, NULL, "only lines %1 (from,to)" },
    { "git branch", "--merged", 0, NULL, "only branches already merged into" },
    { "git branch", "--sort", 1, "-committerdate", "most recently committed first" },
    { "git branch", "-d", 0, NULL, "delete the branch (refused if it isn't merged)" },
    { "git branch", "-D", 0, NULL, "delete the branch even if it isn't merged" },
    { "git branch", "-m", 0, NULL, "rename (with one name: the current branch)" },
    { "git clean", "-f", 0, NULL, "really delete (git refuses without it)" },
    { "git clean", "-n", 0, NULL, "dry run: only show what would be deleted" },
    { "git clean", "-d", 0, NULL, "untracked folders too" },
    { "git clean", "-x", 0, NULL, "ignored files too (build output, .env, ...)" },
    { "git commit", "-m", 1, NULL, "the commit message" },
    { "git commit", "--amend", 0, NULL, "replace the last commit instead of adding one" },
    { "git commit", "--no-edit", 0, NULL, "keep the commit message as it is" },
    { "git diff", "--staged", 0, NULL, "staged changes (what the next commit will contain)" },
    { "git diff", "--cached", 0, NULL, "staged changes (what the next commit will contain)" },
    { "git diff", "--stat", 0, NULL, "only a summary: files changed and lines added/removed" },
    { "git diff", "--name-only", 0, NULL, "only the names of the changed files" },
    { "git log", "--oneline", 0, NULL, "one line per commit: short hash and subject" },
    { "git log", "-p", 0, NULL, "show each commit's changes (the patch)" },
    { "git log", "-n", 1, NULL, "at most %1 commit(s)" },
    { "git log", "--follow", 0, NULL, "keep following the file across renames" },
    { "git log", "--diff-filter", 1, "A", "only commits that added the file" },
    { "git log", "--diff-filter", 1, "D", "only commits that deleted the file" },
    { "git log", "--grep", 1, NULL, "only commits whose message matches %1" },
    { "git log", "-S", 1, NULL, "only commits that added or removed the text %1" },
    { "git log", "--author", 1, NULL, "only commits by %1" },
    { "git log", "--since", 1, NULL, "only commits since %1" },
    { "git log", "--until", 1, NULL, "only commits until %1" },
    { "git rev-list", "--count", 0, NULL, "print only how many commits there are" },
    { "git rev-list", "-n", 1, NULL, "at most %1 commit(s)" },
    { "git merge", "--abort", 0, NULL, "stop the merge in progress and go back to before it" },
    { "git merge", "--squash", 0, NULL, "stage all its changes as one, without a merge commit "
                                        "(commit them yourself)" },
    { "git pull", "--rebase", 0, NULL, "rebase your local commits on top instead of merging" },
    { "git push", "-u", 0, NULL, "remember the remote branch as upstream (plain git push "
                                 "and git pull use it)" },
    { "git push", "--delete", 0, NULL, "delete the branch on the remote" },
    { "git push", "--tags", 0, NULL, "push all tags" },
    { "git push", "--force-with-lease", 0, NULL, "overwrite the remote branch, but only if "
                                                 "nobody pushed since you last fetched" },
    { "git push", "--dry-run", 0, NULL, "only show what would be pushed" },
    { "git rebase", "-i", 0, NULL, "interactive: an editor opens to pick, squash, reword "
                                   "or drop each commit" },
    { "git rebase", "--abort", 0, NULL, "stop the rebase in progress and go back to before it" },
    { "git reflog", "-n", 1, NULL, "the last %1 entries" },
    { "git reset", "--soft", 0, NULL, "keep the changes, staged" },
    { "git reset", "--mixed", 0, NULL, "keep the changes, unstaged" },
    { "git reset", "--hard", 0, NULL, "discard the changes and uncommitted work, for good" },
    { "git restore", "--staged", 0, NULL, "unstage (the file's changes stay on disk)" },
    { "git restore", "--source", 1, NULL, "take the files as they were in %1" },
    { "git revert", "--no-edit", 0, NULL, "use the default commit message" },
    { "git rm", "-r", 0, NULL, "folders too" },
    { "git rm", "--cached", 0, NULL, "only stop tracking: the files stay on disk" },
    { "git show", "--stat", 0, NULL, "only a summary of the files changed" },
    { "git stash", "push", 0, NULL, "stash the changes and clean the working tree" },
    { "git stash", "pop", 0, NULL, "re-apply the latest stash and drop it" },
    { "git stash", "apply", 0, NULL, "re-apply the latest stash and keep it" },
    { "git stash", "list", 0, NULL, "list the stashes" },
    { "git stash", "-m", 1, NULL, "the stash's description" },
    { "git stash", "-u", 0, NULL, "untracked files too" },
    { "git switch", "-c", 1, NULL, "create the branch %1 and switch to it" },
    { "git switch", "-", 0, NULL, "back to the previous branch" },
    { "git tag", "-a", 0, NULL, "an annotated tag (with author, date and message)" },
    { "git tag", "-m", 1, NULL, "the tag message" },
    { "git tag", "-d", 0, NULL, "delete the tag" },
    { "git tag", "--sort", 1, "-creatordate", "newest first" },

    /* ---------------- curl ---------------- */
    { "curl", "-s", 0, NULL, "silent: no progress meter" },
    { "curl", "-S", 0, NULL, "but still show errors" },
    { "curl", "-f", 0, NULL, "fail on HTTP errors (4xx/5xx) instead of saving the error page" },
    { "curl", "-L", 0, NULL, "follow redirects" },
    { "curl", "-I", 0, NULL, "HEAD request: fetch only the headers" },
    { "curl", "-k", 0, NULL, "insecure: don't verify the TLS certificate" },
    { "curl", "-v", 0, NULL, "verbose: show the connection, TLS and both sides' headers" },
    { "curl", "-g", 0, NULL, "don't treat [ ] { } in the URL as patterns" },
    { "curl", "-O", 0, NULL, "save it under the file name from the URL" },
    { "curl", "-o", 1, "/dev/null", "throw the body away" },
    { "curl", "-o", 1, NULL, "save the response to %1" },
    { "curl", "--output-dir", 1, NULL, "in the folder %1" },
    { "curl", "-X", 1, NULL, "use the HTTP method %1" },
    { "curl", "-H", 1, NULL, "send the header %1" },
    { "curl", "-d", 1, NULL, "send %1 as the request body" },
    { "curl", "--data-binary", 1, NULL, "send %1 as the body, byte for byte" },
    { "curl", "-F", 1, NULL, "upload as a multipart form field: %1" },
    { "curl", "-u", 1, NULL, "log in as %1 (curl asks for the password)" },
    { "curl", "-C", 1, "-", "resume a partial download where it stopped" },
    { "curl", "-D", 1, "-", "print the response headers" },
    { "curl", "-w", 1, NULL, "when done, print %1" },
    { "curl", "--retry", 1, NULL, "retry up to %1 time(s) on a transient error" },
    { "curl", "--max-time", 1, NULL, "give up after %1 seconds" },
    { "curl", "--compressed", 0, NULL, "ask for a compressed response and unpack it" },

    /* ---------------- magick ---------------- */
    { "magick", "identify", 0, NULL, "describe the image instead of converting it" },
    { "magick", "mogrify", 0, NULL, "process many images in one go" },
    { "magick", "-quality", 1, NULL, "quality %1 (1-100; higher is better and bigger)" },
    { "magick", "-strip", 0, NULL, "remove metadata (EXIF, GPS, colour profiles)" },
    { "magick", "-auto-orient", 0, NULL, "rotate it upright using its EXIF orientation" },
    { "magick", "-flip", 0, NULL, "flip it upside down" },
    { "magick", "-flop", 0, NULL, "mirror it left to right" },
    { "magick", "+repage", 0, NULL, "reset the canvas offset left by the crop" },
    { "magick", "-gravity", 1, "center", "measure from the center" },
    { "magick", "-background", 1, "none", "transparent background" },
    { "magick", "-background", 1, NULL, "background colour %1" },
    { "magick", "-alpha", 1, "remove", "paint the transparent parts with the background colour" },
    { "magick", "-alpha", 1, "off", "drop the transparency channel" },
    { "magick", "-fuzz", 1, NULL, "colours within %1 count as the same" },
    { "magick", "-transparent", 1, NULL, "make %1 transparent" },
    { "magick", "-trim", 0, NULL, "cut away borders of a single colour" },
    { "magick", "-bordercolor", 1, NULL, "border colour %1" },
    { "magick", "-border", 1, NULL, "add a %1-pixel border" },
    { "magick", "-colorspace", 1, "Gray", "convert to grayscale" },
    { "magick", "-delay", 1, NULL, "show each frame for %1/100 of a second" },
    { "magick", "-loop", 1, "0", "loop forever" },
    { "magick", "+append", 0, NULL, "join the images side by side" },
    { "magick", "-append", 0, NULL, "stack the images top to bottom" },
    { "magick", "-path", 1, NULL, "write the results into %1 (the originals stay)" },
    { "magick", "-verbose", 0, NULL, "every property, EXIF included" },
};
#define NOPTS (sizeof OPTS / sizeof OPTS[0])

/* The entry for `flag` of `cmd` whose `match` fits `next` (NULL: none). */
static const Opt *find_opt(const char *cmd, const char *flag, const char *next) {
    for (size_t i = 0; i < NOPTS; i++) {
        if (strcmp(OPTS[i].cmd, cmd) != 0 || strcmp(OPTS[i].flag, flag) != 0) continue;
        if (OPTS[i].match && (!next || strcmp(OPTS[i].match, next) != 0)) continue;
        return &OPTS[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Decoders                                                           */
/* ------------------------------------------------------------------ */

static bool is_int(const char *s) {
    if (*s == '-' || *s == '+') s++;
    if (!*s) return false;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return false;
    return true;
}

/* ---- ffmpeg filters: "fps=12,scale=320:-2:flags=lanczos" ---- */

static void ff_scale(const char *args, Buf *b) {
    char w[16] = "", h[16] = "";
    sscanf(args, "%15[^:]:%15[^:]", w, h);
    bool wk = strcmp(w, "-1") == 0 || strcmp(w, "-2") == 0;
    bool hk = strcmp(h, "-1") == 0 || strcmp(h, "-2") == 0;
    if (wk && !hk) bput(b, "resize to %s px high (the width keeps the aspect ratio)", h);
    else if (hk && !wk) bput(b, "resize to %s px wide (the height keeps the aspect ratio)", w);
    else if (w[0] && h[0]) bput(b, "resize to %sx%s", w, h);
    else { bput(b, "resize to %s", args); b->bad = true; }
    if (strstr(args, "flags=lanczos")) bput(b, " with the sharp lanczos filter");
}

static void ff_filters(const char *chain, Buf *b) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", chain);
    bool gif = strstr(buf, "palettegen") && strstr(buf, "paletteuse");
    bool first = true;
    for (char *f = strtok(buf, ",;"); f; f = strtok(NULL, ",;")) {
        while (*f == '[') { char *e = strchr(f, ']'); if (!e) break; f = e + 1; }
        char *eq = strchr(f, '=');
        char name[32];
        size_t nl = eq ? (size_t)(eq - f) : strcspn(f, "[");
        if (nl >= sizeof name) nl = sizeof name - 1;
        memcpy(name, f, nl);
        name[nl] = '\0';
        const char *args = eq ? eq + 1 : "";
        if (strcmp(name, "split") == 0 || strcmp(name, "palettegen") == 0 ||
            strcmp(name, "paletteuse") == 0)
            continue;   /* described once, below */
        if (!first) bput(b, ", ");
        first = false;
        if (strcmp(name, "scale") == 0) ff_scale(args, b);
        else if (strcmp(name, "fps") == 0) bput(b, "%s frames per second", args);
        else if (strcmp(name, "transpose") == 0 && strcmp(args, "1") == 0)
            bput(b, "rotate 90 degrees clockwise");
        else if (strcmp(name, "transpose") == 0 && strcmp(args, "2") == 0)
            bput(b, "rotate 90 degrees counter-clockwise");
        else if (strcmp(name, "hflip") == 0) bput(b, "mirror left to right");
        else if (strcmp(name, "vflip") == 0) bput(b, "flip upside down");
        else if (strcmp(name, "format") == 0 && strcmp(args, "gray") == 0) bput(b, "grayscale");
        else if (strcmp(name, "reverse") == 0) bput(b, "play the video backwards");
        else if (strcmp(name, "areverse") == 0) bput(b, "play the audio backwards");
        else if (strcmp(name, "setpts") == 0 && strncmp(args, "PTS/", 4) == 0)
            bput(b, "play the video %sx as fast", args + 4);
        else if (strcmp(name, "atempo") == 0)
            bput(b, "play the audio %sx as fast, at the same pitch", args);
        else if (strcmp(name, "volume") == 0) bput(b, "multiply the volume by %s", args);
        else if (strcmp(name, "loudnorm") == 0)
            bput(b, "even out the loudness (EBU R128 normalization)");
        else { bput(b, "%s", f); b->bad = true; }
    }
    if (gif) bput(b, "%sthen build a palette from the clip's own colours and map every "
                     "frame to it (a GIF has only 256 colours)", first ? "" : ", ");
}

/* ---- magick geometry: 800x600, 800x, x400, 50%, '1920x1080>', 800x600+0+0 ---- */

static void mg_geometry(const char *g, bool crop, Buf *b) {
    size_t n = strlen(g);
    char flag = n && strchr("!><^", g[n - 1]) ? g[n - 1] : 0;
    char core[64];
    snprintf(core, sizeof core, "%.*s", (int)(flag ? n - 1 : n), g);
    char *off = crop ? strpbrk(core + 1, "+-") : NULL;
    if (off) *off++ = '\0';
    char *x = strchr(core, 'x');
    if (crop) {
        bput(b, "a %s region", core);
        if (off && strcmp(off, "0+0") != 0) bput(b, " at offset +%s", off);
        return;
    }
    size_t cl = strlen(core);
    if (cl && core[cl - 1] == '%' && !x) bput(b, "%s of the size", core);
    else if (x == core) bput(b, "%s px high (the width keeps the aspect ratio)", core + 1);
    else if (x && x[1] == '\0') bput(b, "%.*s px wide (the height keeps the aspect ratio)",
                                     (int)(x - core), core);
    else if (!x && is_int(core)) bput(b, "%s px wide (the height keeps the aspect ratio)", core);
    else if (x && flag == '!') bput(b, "exactly %s (the aspect ratio may change)", core);
    else if (x) bput(b, "to fit within %s (keeps the aspect ratio)", core);
    else { bput(b, "%s", g); b->bad = true; }
    if (flag == '>') bput(b, ", only if it is larger");
}

/* ---- find -size / -mtime / -mmin / -atime ---- */

static void find_size(const char *v, Buf *b) {
    const char *cmp = *v == '+' ? "larger than " : *v == '-' ? "smaller than " : "exactly ";
    if (*v == '+' || *v == '-') v++;
    size_t n = strlen(v);
    char unit = n ? v[n - 1] : 0;
    const char *u = unit == 'k' ? " KiB" : unit == 'M' ? " MiB" : unit == 'G' ? " GiB" :
                    unit == 'c' ? " bytes" : NULL;
    if (u) bput(b, "%s%.*s%s", cmp, (int)(n - 1), v, u);
    else bput(b, "%s%s blocks of 512 bytes", cmp, v);
}

static void find_age(const char *verb, const char *v, const char *unit, Buf *b) {
    const char *cmp = *v == '-' ? "less than " : *v == '+' ? "more than " : "exactly ";
    if (*v == '+' || *v == '-') v++;
    bput(b, "%s %s%s %s ago", verb, cmp, v, unit);
}

/* ---- awk 'END { exit !(NR < 100) }' (find's line-count test) ---- */

static bool awk_lines(const char *prog, Buf *b) {
    const char *p = strstr(prog, "NR ");
    if (!p || !strstr(prog, "END")) return false;
    char op[3] = "";
    int n = 0;
    if (sscanf(p + 3, "%2[<>=] %d", op, &n) != 2) return false;
    bput(b, "count the lines; the test passes if there are %s %d", op, n);
    return true;
}

/* ---- git revisions ---- */

static bool hexish(const char *s) {
    size_t n = strlen(s);
    if (n < 7 || n > 40) return false;
    for (size_t i = 0; i < n; i++) if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

/* A description of a revision, or false if `v` doesn't look like one. */
static bool git_rev(const char *v, Buf *b) {
    const char *dots = strstr(v, "..");
    if (dots && dots != v) {
        char from[128];
        snprintf(from, sizeof from, "%.*s", (int)(dots - v), v);
        const char *to = dots + 2;
        if (strcmp(from, "HEAD") == 0 && strcmp(to, "@{upstream}") == 0)
            bput(b, "commits on the upstream branch that you don't have yet");
        else if (strcmp(from, "@{upstream}") == 0 && strcmp(to, "HEAD") == 0)
            bput(b, "your commits that aren't pushed yet");
        else if (strncmp(from, "HEAD~", 5) == 0 && strcmp(to, "HEAD") == 0)
            bput(b, "the last %s commits", from + 5);
        else
            bput(b, "commits in %s that aren't in %s", to, from);
        return true;
    }
    if (strcmp(v, "HEAD") == 0) { bput(b, "the current commit"); return true; }
    if (strncmp(v, "HEAD~", 5) == 0 && is_int(v + 5)) {
        bput(b, "%s commit%s before the current one", v + 5, strcmp(v + 5, "1") ? "s" : "");
        return true;
    }
    if (strcmp(v, "HEAD@{1}") == 0) {
        bput(b, "where the branch was before its last move (from the reflog)");
        return true;
    }
    if (strcmp(v, "ORIG_HEAD") == 0) {
        bput(b, "where the branch was before the last reset, merge or rebase");
        return true;
    }
    if (strncmp(v, "origin/", 7) == 0) {
        bput(b, "the remote's %s branch, as of the last fetch", v + 7);
        return true;
    }
    if (hexish(v)) { bput(b, "the commit %s", v); return true; }
    if (strncmp(v, "$(git rev-list -n 1 HEAD -- ", 28) == 0 && v[strlen(v) - 1] == '^') {
        bput(b, "the commit before (^) the last one that touched the file: the command in "
                "$(...) finds it");
        return true;
    }
    const char *colon = strchr(v, ':');
    if (colon && colon != v && colon[1]) {
        bput(b, "the file %s as it was in %.*s", colon + 1, (int)(colon - v), v);
        return true;
    }
    return false;
}

/* ---- jq programs ---- */

typedef struct { const char *name, *value; } JqArg;
typedef struct { JqArg a[8]; size_t n; } JqArgs;

/* Copy [s, s+n) trimmed, with $name of each --arg replaced by its value. */
static void jq_code(const char *s, size_t n, const JqArgs *args, Buf *b) {
    while (n && isspace((unsigned char)*s)) { s++; n--; }
    while (n && isspace((unsigned char)s[n - 1])) n--;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '$') {
            size_t k = i + 1;
            while (k < n && (isalnum((unsigned char)s[k]) || s[k] == '_')) k++;
            bool done = false;
            for (size_t a = 0; a < args->n && !done; a++) {
                if (strlen(args->a[a].name) == k - i - 1 &&
                    strncmp(args->a[a].name, s + i + 1, k - i - 1) == 0) {
                    bput(b, "\"%s\"", args->a[a].value);
                    i = k - 1;
                    done = true;
                }
            }
            if (done) continue;
        }
        bput(b, "%c", s[i]);
    }
}

/* The next top-level `sep` in [s, s+n), or n. */
static size_t jq_split(const char *s, size_t n, char sep) {
    int depth = 0;
    bool str = false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (str) { if (c == '\\') i++; else if (c == '"') str = false; continue; }
        if (c == '"') str = true;
        else if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') depth--;
        else if (c == sep && depth == 0) return i;
    }
    return n;
}

static bool jq_is(const char *s, size_t n, const char *word) {
    return strlen(word) == n && strncmp(s, word, n) == 0;
}

/* Is [s, s+n) "name(...)" with nothing after? Then the inside is set. */
static bool jq_call(const char *s, size_t n, const char *name, const char **in, size_t *in_n) {
    size_t k = strlen(name);
    if (n < k + 2 || strncmp(s, name, k) != 0 || s[k] != '(' || s[n - 1] != ')') return false;
    if (jq_split(s + k + 1, n - k - 2, ')') != n - k - 2) return false;  /* "f(a) | g(b)" */
    *in = s + k + 1;
    *in_n = n - k - 2;
    return true;
}

static void jq_pipeline(const char *s, size_t n, const JqArgs *args, Buf *b);

static bool jq_path(const char *s, size_t n) {
    if (n == 0 || s[0] != '.') return false;
    for (size_t i = 0; i < n; i++)
        if (!(isalnum((unsigned char)s[i]) || strchr("._[]-:?", s[i]))) return false;
    return true;
}

/* A condition inside select(). */
static void jq_cond(const char *s, size_t n, const JqArgs *args, Buf *b) {
    size_t bar = jq_split(s, n, '|');
    if (bar < n) {
        /* ".email | strings | contains($v)" */
        size_t bar2 = bar + 1 + jq_split(s + bar + 1, n - bar - 1, '|');
        const char *last = s + bar2 + 1;
        size_t last_n = bar2 < n ? n - bar2 - 1 : 0;
        while (last_n && isspace((unsigned char)*last)) { last++; last_n--; }
        const char *in;
        size_t in_n;
        if (bar2 < n && (jq_call(last, last_n, "contains", &in, &in_n) ||
                         jq_call(last, last_n, "test", &in, &in_n))) {
            jq_code(s, bar, args, b);
            bput(b, last[0] == 'c' ? " is a string containing " : " is a string matching the regex ");
            jq_code(in, in_n, args, b);
            return;
        }
        bput(b, "this holds: ");
        jq_code(s, n, args, b);
        return;
    }
    const char *in;
    size_t in_n;
    if (jq_call(s, n, "contains", &in, &in_n)) {
        bput(b, "it contains ");
        jq_code(in, in_n, args, b);
        return;
    }
    size_t k = n;
    if (n > 8 && strncmp(s + n - 8, " != null", 8) == 0) k = n - 8;
    if (k != n) { jq_code(s, k, args, b); bput(b, " is set (not null)"); return; }
    if (n > 8 && strncmp(s + n - 8, " == null", 8) == 0) {
        jq_code(s, n - 8, args, b);
        bput(b, " is missing or null");
        return;
    }
    jq_code(s, n, args, b);
}

/* One stage of a pipeline. */
static void jq_stage(const char *s0, size_t n, const JqArgs *args, Buf *b) {
    char s[512];   /* a NUL-terminated copy, trimmed */
    while (n && isspace((unsigned char)*s0)) { s0++; n--; }
    while (n && isspace((unsigned char)s0[n - 1])) n--;
    if (n >= sizeof s) { bput(b, "%.*s", (int)n, s0); b->bad = true; return; }
    memcpy(s, s0, n);
    s[n] = '\0';
    static const struct { const char *code, *what; } WORDS[] = {
        { ".", "the input as it is, pretty-printed" },
        { ".[]", "each element" },
        { "..", "every value, at any depth" },
        { ".[0]", "the first element" },
        { ".[-1]", "the last element" },
        { "keys", "its keys, sorted" },
        { "length", "its length (items in an array, keys in an object, characters in a string)" },
        { "type", "its type (object, array, string, number, boolean or null)" },
        { "add", "add them up (numbers are summed, strings and arrays joined)" },
        { "add / length", "their average (the sum divided by the count)" },
        { "unique", "sort them and drop duplicates" },
        { "sort", "sort them" },
        { "reverse", "reverse the order" },
        { "flatten", "flatten nested arrays into one" },
        { "tostring", "as a string" },
        { "paths", "every path in the document, each as an array of keys" },
        { "strings", "only the strings" },
        { "empty", "no output: jq only parses the input, so it fails on invalid JSON" },
        { "@csv", "format it as a CSV row" },
        { "@tsv", "format it as a tab-separated row" },
    };
    for (size_t k = 0; k < sizeof WORDS / sizeof WORDS[0]; k++)
        if (jq_is(s, n, WORDS[k].code)) { bput(b, "%s", WORDS[k].what); return; }

    const char *in;
    size_t in_n;
    if (jq_call(s, n, "map", &in, &in_n)) {
        bput(b, "for each element: ");
        jq_pipeline(in, in_n, args, b);
        return;
    }
    if (jq_call(s, n, "select", &in, &in_n)) {
        bput(b, "keep it only if ");
        jq_cond(in, in_n, args, b);
        return;
    }
    static const struct { const char *fn, *what; } CALLS[] = {
        { "sort_by", "sort by " }, { "group_by", "group the items by " },
        { "min_by", "the item with the smallest " }, { "max_by", "the item with the largest " },
        { "del", "remove " }, { "join", "join them with " },
        { "contains", "it contains " }, { "test", "it matches the regex " },
    };
    for (size_t k = 0; k < sizeof CALLS / sizeof CALLS[0]; k++) {
        if (jq_call(s, n, CALLS[k].fn, &in, &in_n)) {
            bput(b, "%s", CALLS[k].what);
            jq_code(in, in_n, args, b);
            return;
        }
    }
    /* group_by(.x) | map({key: (.[0].x | tostring), count: length}) */
    if (n > 2 && s[0] == '{' && strstr(s, "key: (.[0]") && strstr(s, "count: length")) {
        const char *p = strstr(s, "key: (.[0]") + 10;
        size_t pn = strcspn(p, " |");
        bput(b, "the group's %.*s and how many items it has", (int)pn, p);
        return;
    }
    /* [.name, .email] */
    if (n > 2 && s[0] == '[' && s[n - 1] == ']' && jq_split(s + 1, n - 2, ']') == n - 2) {
        bput(b, "an array of ");
        jq_code(s + 1, n - 2, args, b);
        return;
    }
    int k = 0;
    char tail[8] = "";
    if (sscanf(s, ".[:%d%7s", &k, tail) == 2 && strcmp(tail, "]") == 0) {
        bput(b, "the first %d elements", k);
        return;
    }
    if (sscanf(s, ".[-%d:%7s", &k, tail) == 2 && strcmp(tail, "]") == 0) {
        bput(b, "the last %d elements", k);
        return;
    }
    if (jq_path(s, n)) {
        bput(b, "take %.*s", (int)n, s);
        if (memchr(s, '[', n) && strstr(s, "[]")) bput(b, " (from every element)");
        return;
    }
    jq_code(s, n, args, b);
    b->bad = true;
}

static void jq_pipeline(const char *s, size_t n, const JqArgs *args, Buf *b) {
    bool first = true;
    while (n > 0) {
        size_t k = jq_split(s, n, '|');
        const char *st = s;
        size_t sn = k;
        /* "add / length" is one stage */
        if (!first) bput(b, ", then ");
        jq_stage(st, sn, args, b);
        first = false;
        if (k >= n) break;
        s += k + 1;
        n -= k + 1;
    }
}

static void jq_program(const char *prog, const JqArgs *args, Buf *b) {
    if (strncmp(prog, "(.[0] | keys_unsorted) as $k | $k, (.[] | [.[$k[]]]) | @", 56) == 0) {
        bput(b, "a header row with the first object's keys, then one row per object with its "
                "values in that order, formatted as %s",
             strcmp(prog + 56, "csv") == 0 ? "CSV" : "tab-separated values");
        return;
    }
    jq_pipeline(prog, strlen(prog), args, b);
}

/* ------------------------------------------------------------------ */
/* Explaining one command                                             */
/* ------------------------------------------------------------------ */

static void explain_words(Ex *x, const Word *w, size_t n);

static const char *const BUNDLED[] = { "grep", "tar", "curl", "git clean", "rm", "sort", NULL };

static bool in_words(const char *s, const char *const *list) {
    for (size_t i = 0; list[i]; i++) if (strcmp(s, list[i]) == 0) return true;
    return false;
}

static void unexplained(Ex *x, const Word *w) {
    row(x, w->raw, "%s", NL_NO_EXPLANATION);
}

/* What a command (and git/magick subcommand) is. */
static const struct { const char *cmd, *what; } COMMANDS[] = {
    { "ffmpeg", "convert, cut and filter video and audio" },
    { "ffprobe", "show a media file's format, duration and streams" },
    { "jq", "filter and transform JSON" },
    { "grep", "search text for a pattern" },
    { "find", "walk folders and select files by name, type, size or age" },
    { "tar", "create, list or extract archives" },
    { "git", "version control" },
    { "curl", "make an HTTP request" },
    { "magick", "ImageMagick: convert and edit images" },
    { "mkdir", "create a folder" },
    { "wc", "count lines, words or bytes" },
    { "sort", "sort lines" },
    { "head", "keep the first lines" },
    { "du", "show how much disk space each file uses" },
    { "rm", "delete" },
    { "echo", "print a line" },
    { "awk", "run a small text-processing program" },
    { "xargs", "run the next command with the input lines as its arguments" },
};

/* Positional words: what each tool does with them. `pos` counts them. */
typedef struct {
    const char *cmd;        /* "git log", "tar", ...                      */
    int pos;                /* positionals seen so far                    */
    int total;              /* positionals in the command (magick: last is the output) */
    bool dashdash;          /* git: after "--"                            */
    bool pattern_given;     /* grep: a -e was seen                        */
    char tar_mode;          /* 'c', 'x', 't'                              */
    bool created;           /* git switch -c                              */
    bool seeked;            /* ffmpeg: -ss / -to / -t / -sseof given      */
    JqArgs jq;
} State;

static void positional(Ex *x, State *st, const Word *w) {
    const char *v = w->val;
    const char *c = st->cmd;
    Buf b = { .n = 0 };
    st->pos++;
    if (strcmp(c, "ffmpeg") == 0) {
        if (strstr(v, "%0") && strchr(v, 'd'))
            row(x, w->raw, "the output files, numbered 0001, 0002, ... (the extension sets "
                           "the format)");
        else
            row(x, w->raw, "the output file (its extension sets the format)");
    } else if (strcmp(c, "ffprobe") == 0) {
        row(x, w->raw, "the file to inspect");
    } else if (strcmp(c, "jq") == 0) {
        if (st->pos == 1) {
            jq_program(v, &st->jq, &b);
            row(x, w->raw, "%s%s", b.bad ? NL_NO_EXPLANATION " " : "", b.s);
        } else {
            row(x, w->raw, "the input file (without one, jq reads standard input)");
        }
    } else if (strcmp(c, "grep") == 0) {
        if (!st->pattern_given && st->pos == 1) {
            if (strcmp(v, "^[*+]|^[[:space:]]*(main|master|develop)$") == 0)
                row(x, w->raw, "lines for the current branch (*), a branch checked out "
                               "elsewhere (+), or main, master or develop");
            else
                row(x, w->raw, "the pattern to search for");
        } else if (strcmp(v, ".") == 0) {
            row(x, w->raw, "search in the current folder");
        } else {
            row(x, w->raw, "search in %s", v);
        }
    } else if (strcmp(c, "find") == 0) {
        if (strcmp(v, ".") == 0) row(x, w->raw, "start searching in the current folder");
        else row(x, w->raw, "start searching in %s", v);
    } else if (strcmp(c, "tar") == 0) {
        row(x, w->raw, st->tar_mode == 'c' ? "put %s in the archive" :
                       st->tar_mode == 'x' ? "extract only %s" : "only %s", v);
    } else if (strcmp(c, "curl") == 0) {
        row(x, w->raw, "the URL");
    } else if (strcmp(c, "mkdir") == 0) {
        row(x, w->raw, "the folder to create");
    } else if (strcmp(c, "echo") == 0) {
        row(x, w->raw, "the text to print");
    } else if (strcmp(c, "magick") == 0 || strcmp(c, "magick identify") == 0) {
        if (strcmp(c, "magick identify") == 0) row(x, w->raw, "the image to describe");
        else if (st->pos == st->total) row(x, w->raw, "the output file (its extension sets the format)");
        else row(x, w->raw, strpbrk(v, "*?[") ? "the input images (magick expands the pattern itself)"
                                              : "an input image");
    } else if (strcmp(c, "magick mogrify") == 0) {
        row(x, w->raw, "the images to process (magick expands the pattern itself)");
    } else if (strncmp(c, "git", 3) == 0) {
        const char *sub = c[3] ? c + 4 : "";
        if (st->dashdash) { row(x, w->raw, "only the path %s", v); return; }
        if (strcmp(sub, "push") == 0 && strcmp(v, "origin") == 0) {
            row(x, w->raw, "the remote");
        } else if (strcmp(sub, "push") == 0 && strcmp(v, "HEAD") == 0) {
            row(x, w->raw, "the current branch, under the same name");
        } else if (strcmp(sub, "push") == 0) {
            row(x, w->raw, "the branch %s", v);
        } else if (strcmp(sub, "switch") == 0) {
            row(x, w->raw, st->created ? "start it from %s" : "switch to the branch %s", v);
        } else if (strcmp(sub, "merge") == 0) {
            row(x, w->raw, "the branch to merge into the current one");
        } else if (strcmp(sub, "branch") == 0 || strcmp(sub, "tag") == 0) {
            if (git_rev(v, &b) && strcmp(sub, "branch") == 0) row(x, w->raw, "%s", b.s);
            else row(x, w->raw, "%s %s", sub, v);
        } else if (strcmp(sub, "blame") == 0) {
            row(x, w->raw, "the file");
        } else if (strcmp(sub, "config") == 0) {
            const Opt *o = find_opt(c, v, NULL);
            if (o) row(x, w->raw, "%s", o->what); else unexplained(x, w);
        } else if (git_rev(v, &b)) {
            const char *lead = strcmp(sub, "rebase") == 0 ? "the new base: " :
                               strcmp(sub, "reset") == 0 ? "move it to " :
                               strcmp(sub, "revert") == 0 ? "undo " :
                               strcmp(sub, "cherry-pick") == 0 ? "copy " :
                               strcmp(sub, "diff") == 0 ? "compare with " : "";
            row(x, w->raw, "%s%s", lead, b.s);
        } else if (strcmp(sub, "rebase") == 0 || strcmp(sub, "diff") == 0 ||
                   strcmp(sub, "log") == 0 || strcmp(sub, "reset") == 0 ||
                   strcmp(sub, "show") == 0 || strcmp(sub, "rev-list") == 0) {
            row(x, w->raw, strcmp(sub, "rebase") == 0 ? "the new base: %s" :
                           strcmp(sub, "diff") == 0 ? "compare with %s" : "the revision %s", v);
        } else {
            unexplained(x, w);
        }
    } else {
        unexplained(x, w);
    }
}

/* Explain the option `o` given at w[0] with its values (`n` words in all,
 * or 1 when the value was attached with '='). */
static void option_row(Ex *x, State *st, const Opt *o, const Word *w, size_t n,
                       const char *v1, const char *v2) {
    char desc[768];
    Buf b = { .n = 0 };
    const char *c = st->cmd;
    /* values with structure of their own */
    if ((strcmp(c, "ffmpeg") == 0 && (strcmp(o->flag, "-vf") == 0 ||
                                      strcmp(o->flag, "-af") == 0 ||
                                      strncmp(o->flag, "-filter:", 8) == 0))) {
        ff_filters(v1, &b);
    } else if (strcmp(c, "find") == 0 && strcmp(o->flag, "-size") == 0) {
        find_size(v1, &b);
    } else if (strcmp(c, "find") == 0 && strcmp(o->flag, "-mtime") == 0) {
        find_age("modified", v1, "days", &b);
    } else if (strcmp(c, "find") == 0 && strcmp(o->flag, "-atime") == 0) {
        find_age("last read", v1, "days", &b);
    } else if (strcmp(c, "find") == 0 && strcmp(o->flag, "-mmin") == 0) {
        find_age("modified", v1, "minutes", &b);
    } else if (strncmp(c, "magick", 6) == 0 && (strcmp(o->flag, "-resize") == 0 ||
                                               strcmp(o->flag, "-thumbnail") == 0 ||
                                               strcmp(o->flag, "-extent") == 0 ||
                                               strcmp(o->flag, "-crop") == 0)) {
        bool crop = strcmp(o->flag, "-crop") == 0;
        bput(&b, strcmp(o->flag, "-thumbnail") == 0 ? "a small copy, metadata dropped: " :
                 strcmp(o->flag, "-extent") == 0 ? "pad or cut the canvas to " :
                 crop ? "crop " : "resize: ");
        if (strcmp(o->flag, "-extent") == 0) bput(&b, "%s", v1);
        else mg_geometry(v1, crop, &b);
    } else if (strncmp(c, "magick", 6) == 0 && strcmp(o->flag, "-rotate") == 0) {
        if (v1[0] == '-') bput(&b, "rotate %s degrees counter-clockwise", v1 + 1);
        else bput(&b, "rotate %s degrees clockwise", v1);
    } else if (strncmp(c, "magick", 6) == 0 && (strcmp(o->flag, "-blur") == 0 ||
                                               strcmp(o->flag, "-sharpen") == 0)) {
        const char *sig = strchr(v1, 'x');
        bput(&b, "%s with a strength (sigma) of %s; 0 lets magick pick the radius",
             o->flag[1] == 'b' ? "blur" : "sharpen", sig ? sig + 1 : v1);
    } else if (strcmp(c, "magick") == 0 && strcmp(o->flag, "-define") == 0 &&
               strncmp(v1, "icon:auto-resize=", 17) == 0) {
        bput(&b, "put these sizes in the .ico: %s px", v1 + 17);
    } else if (strcmp(c, "magick mogrify") == 0 && strcmp(o->flag, "-format") == 0) {
        bput(&b, "write each as a new .%s file (the originals stay)", v1);
    } else if (strcmp(c, "magick identify") == 0 && strcmp(o->flag, "-format") == 0) {
        if (strcmp(v1, "%wx%h\\n") == 0) bput(&b, "print only the width x height");
        else { bput(&b, "print %s", v1); b.bad = true; }
    } else if (strcmp(c, "curl") == 0 && strcmp(o->flag, "-w") == 0 &&
               strcmp(v1, "%{http_code}\\n") == 0) {
        bput(&b, "when done, print the HTTP status code");
    } else if (strcmp(c, "curl") == 0 && strcmp(o->flag, "-w") == 0 &&
               strstr(v1, "%{time_")) {
        bput(&b, "when done, print how long each phase took (DNS, connect, TLS, first "
                 "byte, total)");
    } else if (strcmp(c, "curl") == 0 && (strcmp(o->flag, "-d") == 0 ||
                                          strcmp(o->flag, "--data-binary") == 0) &&
               v1[0] == '@') {
        bput(&b, "send the contents of %s as the request body", v1 + 1);
    } else if (strcmp(c, "curl") == 0 && strcmp(o->flag, "-F") == 0 && strstr(v1, "=@")) {
        const char *at = strstr(v1, "=@");
        bput(&b, "upload %s as the form field \"%.*s\" (multipart)", at + 2, (int)(at - v1), v1);
    } else if (strcmp(c, "curl") == 0 && strcmp(o->flag, "-H") == 0 &&
               strncmp(v1, "Authorization: Bearer $", 23) == 0) {
        bput(&b, "send the token from $%s (your shell fills it in; it stays out of the "
                 "command)", v1 + 23);
    } else if (strcmp(c, "git log") == 0 && strcmp(o->flag, "--author") == 0 &&
               strcmp(v1, "$(git config user.name)") == 0) {
        bput(&b, "only your commits (the name from git config user.name)");
    } else {
        fill(desc, sizeof desc, o->what, v1, v2);
        bput(&b, "%s", desc);
        if (strcmp(c, "ffmpeg") == 0 && strcmp(o->flag, "-c") == 0 && st->seeked)
            bput(&b, "; without re-encoding, a cut starts at the nearest keyframe");
    }
    row_words(x, w, n, b.bad ? NL_NO_EXPLANATION : b.s);
}

/* Options that take a value but whose text depends on it (see option_row). */
static const Opt VALUED[] = {
    { "ffmpeg", "-vf", 1, NULL, "" }, { "ffmpeg", "-af", 1, NULL, "" },
    { "ffmpeg", "-filter:v", 1, NULL, "" }, { "ffmpeg", "-filter:a", 1, NULL, "" },
    { "find", "-size", 1, NULL, "" }, { "find", "-mtime", 1, NULL, "" },
    { "find", "-mmin", 1, NULL, "" }, { "find", "-atime", 1, NULL, "" },
    { "magick", "-resize", 1, NULL, "" }, { "magick", "-thumbnail", 1, NULL, "" },
    { "magick", "-extent", 1, NULL, "" }, { "magick", "-crop", 1, NULL, "" },
    { "magick", "-rotate", 1, NULL, "" }, { "magick", "-blur", 1, NULL, "" },
    { "magick", "-sharpen", 1, NULL, "" }, { "magick", "-define", 1, NULL, "" },
    { "magick", "-format", 1, NULL, "" },
};

static const Opt *lookup(const char *cmd, const char *flag, const char *next) {
    const Opt *o = find_opt(cmd, flag, next);
    if (o) return o;
    /* git rev-list takes git log's commit filters */
    if (strcmp(cmd, "git rev-list") == 0 && (o = find_opt("git log", flag, next))) return o;
    /* "git log" falls back to "git"; "magick mogrify" to "magick" */
    const char *sp = strchr(cmd, ' ');
    char base[32];
    if (sp && (size_t)(sp - cmd) < sizeof base) {
        snprintf(base, sizeof base, "%.*s", (int)(sp - cmd), cmd);
        if (strcmp(base, "magick") == 0 && (o = find_opt(base, flag, next))) return o;
    }
    for (size_t i = 0; i < sizeof VALUED / sizeof VALUED[0]; i++) {
        bool cmd_ok = strcmp(VALUED[i].cmd, cmd) == 0 ||
                      (strcmp(VALUED[i].cmd, "magick") == 0 && strncmp(cmd, "magick", 6) == 0);
        if (cmd_ok && strcmp(VALUED[i].flag, flag) == 0) return &VALUED[i];
    }
    return NULL;
}

/* find -exec CMD ... {} \; / {} + : explain CMD nested. Returns words used. */
static size_t find_exec(Ex *x, const Word *w, size_t n) {
    size_t end = 1;
    while (end < n && strcmp(w[end].val, ";") != 0 && strcmp(w[end].val, "+") != 0) end++;
    bool each = end < n && strcmp(w[end].val, ";") == 0;
    Buf b = { .n = 0 };
    if (end > 2 && strcmp(w[1].val, "awk") == 0 && awk_lines(w[2].val, &b)) {
        row(x, "-exec awk ...", "run awk on each file found: %s", b.s);
        x->indent += 2;
        row(x, w[2].raw, "the awk program: at the END of the file, exit 0 (true) only if "
                         "the line count NR passes");
        row(x, "{}", "the path found");
        row(x, w[end < n ? end : n - 1].raw, "end of the command; it runs once per file");
        x->indent -= 2;
        return end < n ? end + 1 : n;
    }
    row(x, "-exec", each ? "run this for each file found; it is also a test: the file "
                           "passes only if it succeeds" :
                           "run this on the files found, as many at a time as fit:");
    x->indent += 2;
    size_t inner = end;
    while (inner > 1 && strcmp(w[inner - 1].val, "{}") == 0) inner--;
    explain_words(x, w + 1, inner - 1);
    for (size_t k = inner; k < end; k++) row(x, w[k].raw, "the path(s) found");
    if (end < n) row(x, w[end].raw, each ? "end of the command (once per file)"
                                         : "end of the command (files in batches)");
    x->indent -= 2;
    return end < n ? end + 1 : n;
}

static void explain_words(Ex *x, const Word *w, size_t n) {
    if (n == 0) return;
    State st;
    memset(&st, 0, sizeof st);
    char cmd[48];
    snprintf(cmd, sizeof cmd, "%s", w[0].val);

    bool known = false;
    for (size_t k = 0; k < sizeof COMMANDS / sizeof COMMANDS[0]; k++)
        if (strcmp(COMMANDS[k].cmd, cmd) == 0) { row(x, w[0].raw, "%s", COMMANDS[k].what); known = true; }
    if (!known) { unexplained(x, &w[0]); return; }

    size_t i = 1;
    /* git and magick: the subcommand */
    if ((strcmp(cmd, "git") == 0 || strcmp(cmd, "magick") == 0) && n > 1 &&
        w[1].val[0] != '-' && (strcmp(cmd, "git") == 0 || find_opt("magick", w[1].val, NULL))) {
        const Opt *o = find_opt(cmd, w[1].val, NULL);
        if (o) row(x, w[1].raw, "%s", o->what);
        else if (strcmp(cmd, "git") == 0 && strcmp(w[1].val, "config") == 0)
            row(x, w[1].raw, "read a setting");
        else unexplained(x, &w[1]);
        snprintf(cmd + strlen(cmd), sizeof cmd - strlen(cmd), " %s", w[1].val);
        i = 2;
        /* git stash push / pop / ... */
        if (strcmp(cmd, "git stash") == 0 && i < n && (o = find_opt(cmd, w[i].val, NULL))) {
            row(x, w[i].raw, "%s", o->what);
            i++;
        }
    }
    st.cmd = cmd;

    /* positionals in the whole command, for magick's "the last is the output" */
    for (size_t k = i; k < n; k++) {
        const char *v = w[k].val;
        if (v[0] == '-' || v[0] == '+') {
            const Opt *o = lookup(cmd, v, k + 1 < n ? w[k + 1].val : NULL);
            if (o) k += (size_t)o->nargs;
            continue;
        }
        st.total++;
    }

    if (strcmp(cmd, "xargs") == 0) {
        /* xargs -r git branch -d: its options, then the command it runs */
        while (i < n && w[i].val[0] == '-') {
            const Opt *o = find_opt(cmd, w[i].val, NULL);
            if (o) row(x, w[i].raw, "%s", o->what); else unexplained(x, &w[i]);
            i++;
        }
        x->indent += 2;
        explain_words(x, w + i, n - i);
        x->indent -= 2;
        return;
    }

    while (i < n) {
        const char *v = w[i].val;
        const char *next = i + 1 < n ? w[i + 1].val : NULL;

        if (strcmp(cmd, "find") == 0 && strcmp(v, "-exec") == 0) {
            i += find_exec(x, w + i, n - i);
            continue;
        }
        if (strcmp(cmd, "awk") == 0) {
            Buf b = { .n = 0 };
            if (awk_lines(v, &b)) row(x, w[i].raw, "%s", b.s); else unexplained(x, &w[i]);
            i++;
            continue;
        }
        if (strncmp(cmd, "git", 3) == 0 && strcmp(v, "--") == 0) {
            row(x, w[i].raw, "what follows are paths, not branches or options");
            st.dashdash = true;
            i++;
            continue;
        }
        bool optlike = (v[0] == '-' && v[1] != '\0' && !st.dashdash) ||
                       (v[0] == '+' && strncmp(cmd, "magick", 6) == 0) ||
                       (strcmp(cmd, "find") == 0 && (strcmp(v, "(") == 0 ||
                                                     strcmp(v, ")") == 0 ||
                                                     strcmp(v, "{}") == 0));
        if (strcmp(cmd, "git switch") == 0 && strcmp(v, "-") == 0) optlike = true;
        if (!optlike) { positional(x, &st, &w[i]); i++; continue; }

        /* --name=value */
        const char *eq = v[0] == '-' && v[1] == '-' ? strchr(v, '=') : NULL;
        if (eq) {
            char flag[48];
            snprintf(flag, sizeof flag, "%.*s", (int)(eq - v), v);
            const Opt *o = lookup(cmd, flag, eq + 1);
            if (o && o->nargs == 1) option_row(x, &st, o, &w[i], 1, eq + 1, NULL);
            else unexplained(x, &w[i]);
            i++;
            continue;
        }

        const Opt *o = lookup(cmd, v, next);
        if (o && i + (size_t)o->nargs < n) {
            const char *v1 = o->nargs >= 1 ? w[i + 1].val : NULL;
            const char *v2 = o->nargs >= 2 ? w[i + 2].val : NULL;
            if (strcmp(cmd, "grep") == 0 && strcmp(v, "-e") == 0) st.pattern_given = true;
            if (strcmp(cmd, "git switch") == 0 && strcmp(v, "-c") == 0) st.created = true;
            if (strcmp(cmd, "ffmpeg") == 0 && (strcmp(v, "-ss") == 0 || strcmp(v, "-to") == 0 ||
                                               strcmp(v, "-t") == 0 || strcmp(v, "-sseof") == 0))
                st.seeked = true;
            if (strcmp(cmd, "jq") == 0 && strcmp(v, "--arg") == 0 && st.jq.n < 8) {
                st.jq.a[st.jq.n].name = v1;
                st.jq.a[st.jq.n++].value = v2;
            }
            if (strcmp(cmd, "tar") == 0 && strcmp(v, "-C") == 0 && st.tar_mode == 'c')
                row_words(x, &w[i], 2, "change to this folder first");
            else if (strcmp(cmd, "tar") == 0 && strcmp(v, "-C") == 0 && st.tar_mode == 'x') {
                char d[256];
                snprintf(d, sizeof d, "extract into the folder %s (it must exist)", v1);
                row_words(x, &w[i], 2, d);
            } else
                option_row(x, &st, o, &w[i], 1 + (size_t)o->nargs, v1, v2);
            i += 1 + (size_t)o->nargs;
            continue;
        }

        /* bundled short options: -czvf, -rnE, -sSIL, -fdx */
        size_t len = strlen(v);
        bool bundle = in_words(cmd, BUNDLED) && v[0] == '-' && v[1] != '-' && len > 2;
        for (size_t k = 1; bundle && k < len; k++)
            if (!isalpha((unsigned char)v[k])) bundle = false;
        if (bundle) {
            char letters[96] = "";
            for (size_t k = 1; k < len && strlen(letters) + 4 < sizeof letters; k++)
                snprintf(letters + strlen(letters), sizeof letters - strlen(letters), "%s-%c",
                         k > 1 ? " " : "", v[k]);
            row(x, w[i].raw, "short for %s:", letters);
            x->indent += 2;
            size_t used = 1;
            for (size_t k = 1; k < len; k++) {
                char letter[3] = { '-', v[k], '\0' };
                const Opt *lo = find_opt(cmd, letter, NULL);
                if (strcmp(cmd, "tar") == 0 && strchr("cxt", v[k])) st.tar_mode = v[k];
                if (!lo) { row(x, letter, "%s", NL_NO_EXPLANATION); continue; }
                if (lo->nargs == 1 && i + used < n) {
                    Word pair[2] = { { letter, letter }, w[i + used] };
                    char d[512];
                    fill(d, sizeof d, lo->what, w[i + used].val, NULL);
                    row_words(x, pair, 2, d);
                    used++;
                } else {
                    row(x, letter, "%s", lo->what);
                }
            }
            x->indent -= 2;
            i += used;
            continue;
        }
        unexplained(x, &w[i]);
        i++;
    }
}

static void explain_segment(Ex *x, const Parsed *p, size_t s) {
    const Seg *seg = &p->seg[s];
    if (seg->op) {
        const char *what = strcmp(seg->op, "|") == 0 ? "send the output to the next command:" :
                           strcmp(seg->op, "&&") == 0 ? "if that worked, then:" :
                           strcmp(seg->op, "||") == 0 ? "if that failed, then:" : "then:";
        fputc('\n', x->out);
        row(x, seg->op, "%s", what);
    }
    explain_words(x, p->w + seg->first, seg->n);
}

void nl_explain(const char *cmd, FILE *out) {
    static Parsed p;
    Ex x = { out, 0 };
    fputc('\n', out);
    if (!split_command(cmd, &p)) {
        fprintf(out, "  %s\n", NL_NO_EXPLANATION);
        return;
    }
    for (size_t s = 0; s < p.nseg; s++) explain_segment(&x, &p, s);
}
