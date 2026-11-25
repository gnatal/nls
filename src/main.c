/*
 * main.c — CLI parsing and dispatch.
 *
 *   nls [options] [tool] <prompt...>
 *
 * Prints the compiled command to stdout. With no tool, the request is
 * compiled for every tool and the best fit is used (infer_tool()). With -x
 * it shows the command on stderr, asks for confirmation (skip with -y) and
 * runs it via system(). A destructive command (see nl_danger()) is
 * explained, its dry run runs first, and only a typed "y" runs it; -y skips
 * that only with --force. --explain breaks the command down (explain.c).
 */
#define _POSIX_C_SOURCE 200809L   /* popen() for dry runs */
#include "nl.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

/* Lines of dry-run output shown before the rest is only counted. */
#define NL_DRY_RUN_LINES 20

#define NL_TOOL_LIST "ffmpeg, jq, grep, find, tar, git, curl, magick"

/* -x, -y, --force and --explain */
typedef struct {
    bool exec, yes, force, explain;
} RunMode;

static void usage(FILE *f) {
    fprintf(f,
        "usage: nls [options] [tool] \"<what you want>\"\n"
        "\n"
        "Compile a natural-language request into a shell command (offline).\n"
        "Supported tools: " NL_TOOL_LIST "\n"
        "Without a tool, nls picks the one that fits the request best.\n"
        "\n"
        "options:\n"
        "  -x, --exec        run the command (asks for confirmation)\n"
        "  -y, --yes         with -x: don't ask, unless the match is uncertain\n"
        "                    or the command is destructive\n"
        "      --force       with -x -y: don't ask for destructive commands either\n"
        "  -e, --explain     explain the command part by part\n"
        "  -v, --verbose     explain slots, tokens and template scores on stderr\n"
        "  -l, --list [tool] list available templates\n"
        "      --shell <zsh|bash>\n"
        "                    print a key binding that turns the typed line into\n"
        "                    a command: eval \"$(nls --shell zsh)\"\n"
        "  -h, --help        show this help\n"
        "  -V, --version     show version\n"
        "\n"
        "examples:\n"
        "  nls ffmpeg \"cut input.mp4 from 00:10 to 00:25 and scale to 720p\"\n"
        "  nls jq \"get .name from users.json where .age > 30\"\n"
        "  nls grep \"which python files contain TODO, ignoring case\"\n"
        "  nls find \"files larger than 100MB modified in the last 7 days\"\n"
        "  nls tar \"compress src/ into backup.tar.gz excluding .git\"\n"
        "  nls git \"undo last 2 commits keep changes staged\"\n"
        "  nls curl \"post data.json to https://api.example.com/users\"\n"
        "  nls magick \"resize hero.png to width 800 convert to webp quality 85\"\n"
        "  nls \"convert demo.mov to gif\"          (tool inferred: ffmpeg)\n"
        "  nls --explain git \"squash the last 3 commits\"\n"
        "  nls -x jq \"pretty print data.json\"\n");
}

static void list_templates(ToolId only) {
    for (size_t i = 0; i < NL_TEMPLATE_COUNT; i++) {
        const CommandTemplate *t = &NL_TEMPLATES[i];
        if (only != TOOL_UNKNOWN && t->tool != only) continue;
        printf("%-20s %s%s\n", t->id, t->description, nl_danger(t) ? " (destructive)" : "");
        printf("%-20s   nls %s \"%s\"\n", "", nl_tool_name(t->tool), t->example);
    }
}

/* Join argv[from..argc) into one prompt, re-quoting args that contain spaces
 * so the lexer still sees them as single tokens. */
static bool join_prompt(int argc, char **argv, int from, char *out, size_t size) {
    size_t len = 0;
    out[0] = '\0';
    for (int i = from; i < argc; i++) {
        const char *a = argv[i];
        bool spaced = argc - from > 1 && strpbrk(a, " \t") != NULL;
        char q = strchr(a, '\'') ? '"' : '\'';
        int n = snprintf(out + len, size - len, "%s%s%s%s",
                         len ? " " : "",
                         spaced ? (q == '"' ? "\"" : "'") : "",
                         a,
                         spaced ? (q == '"' ? "\"" : "'") : "");
        if (n < 0 || (size_t)n >= size - len) return false;
        len += (size_t)n;
    }
    return true;
}

static void print_context(const ParseContext *ctx) {
    fprintf(stderr, "slots:");
    if (ctx->slot_count == 0) fprintf(stderr, " (none)");
    for (size_t i = 0; i < ctx->slot_count; i++)
        fprintf(stderr, " %s=%s", nl_slot_name(ctx->slots[i].type), ctx->slots[i].value);
    fprintf(stderr, "\ntokens:");
    if (ctx->token_count == 0) fprintf(stderr, " (none)");
    for (size_t i = 0; i < ctx->token_count; i++)
        fprintf(stderr, " %s", ctx->intent_tokens[i]);
    fputc('\n', stderr);
}

/* Show `prompt` on stderr and read one line from the terminal (from stdin
 * when there is none, e.g. in a script). False at end of input. Only a
 * terminal echoes the answer, so otherwise end the prompt's line here. */
static bool ask(const char *prompt, char *line, size_t size) {
    FILE *tty = fopen("/dev/tty", "r");
    FILE *in = tty ? tty : stdin;
    fprintf(stderr, "%s", prompt);
    fflush(stderr);
    bool got = fgets(line, (int)size, in) != NULL;
    if (!got || !tty) fputc('\n', stderr);
    if (tty) fclose(tty);
    return got;
}

/* Enter runs, "n" aborts. */
static bool confirm(void) {
    char line[64];
    return ask("[Enter to run / Ctrl+C to abort] ", line, sizeof line) &&
           line[0] != 'n' && line[0] != 'N';
}

/* Destructive commands: only "y" or "yes" (any case) runs; Enter aborts. */
static bool confirm_destructive(void) {
    char line[64];
    if (!ask("[type y to run / Enter to abort] ", line, sizeof line)) return false;
    char *p = line + strspn(line, " \t");
    size_t n = strcspn(p, " \t\r\n");
    if (p[n + strspn(p + n, " \t\r\n")] != '\0') return false;   /* more than one word */
    for (size_t k = 0; k < n; k++) p[k] = (char)tolower((unsigned char)p[k]);
    return (n == 1 && p[0] == 'y') || (n == 3 && strncmp(p, "yes", 3) == 0);
}

/* Exit code from a system() / pclose() status. */
static int exit_status(int rc) {
#ifdef _WIN32
    return rc;
#else
    /* POSIX wait status without <sys/wait.h>: exit code in bits 8..15,
     * terminating signal in the low 7 bits. */
    if ((rc & 0x7f) != 0) return 128 + (rc & 0x7f);
    return (rc >> 8) & 0xff;
#endif
}

static int run_command(const char *cmd) {
    fflush(stdout);
    int rc = system(cmd);
    if (rc == -1) { perror("nls: system"); return 127; }
    return exit_status(rc);
}

/* Run a dry run with no input, showing its output (stdout and stderr) on
 * stderr, indented: stdout stays the real command's. Only the first
 * NL_DRY_RUN_LINES lines are shown, the rest counted. Control characters
 * are shown as '?', so a crafted file name can't redraw the preview.
 * Returns the dry run's exit code. */
static int run_dry_run(const char *dry) {
    char sh[NL_MAX_CMD_LEN + 32];
    int n = snprintf(sh, sizeof sh, "exec </dev/null 2>&1; %s", dry);
    if (n < 0 || (size_t)n >= sizeof sh) return 127;
    fflush(stdout);
    FILE *p = popen(sh, "r");
    if (!p) { perror("nls: popen"); return 127; }

    char buf[1024];
    unsigned long lines = 0;
    bool line_start = true;
    while (fgets(buf, sizeof buf, p)) {
        size_t len = strlen(buf);
        if (line_start) lines++;
        if (lines <= NL_DRY_RUN_LINES) {
            for (size_t k = 0; k < len; k++) {
                unsigned char c = (unsigned char)buf[k];
                if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7f) buf[k] = '?';
            }
            if (line_start) fputs("  ", stderr);
            fputs(buf, stderr);
        }
        line_start = len > 0 && buf[len - 1] == '\n';
    }
    if (!line_start && lines <= NL_DRY_RUN_LINES) fputc('\n', stderr);
    int rc = pclose(p);

    if (lines == 0)
        fprintf(stderr, "  (no output)\n");
    else if (lines > NL_DRY_RUN_LINES)
        fprintf(stderr, "  ... and %lu more line%s\n", lines - NL_DRY_RUN_LINES,
                lines - NL_DRY_RUN_LINES == 1 ? "" : "s");
    return rc == -1 ? 127 : exit_status(rc);
}

/* Print the words that did / didn't count toward the match. */
static void print_tokens(const ParseContext *ctx, uint32_t mask, bool matched) {
    bool first = true;
    for (size_t t = 0; t < ctx->token_count; t++) {
        if (((mask >> t) & 1u) != matched || ctx->intent_tokens[t][0] == '@') continue;
        fprintf(stderr, "%s\"%s\"", first ? "" : ", ", ctx->intent_tokens[t]);
        first = false;
    }
}

/* Is the match weak or a close call? With `report`, say why on stderr.
 * Returns true if the user should look before the command runs. */
static bool check_uncertain(const ParseContext *ctx, const MatchResult *m, const char *cmd,
                            bool report) {
    char alt_cmd[NL_MAX_ALTS][NL_MAX_CMD_LEN];
    const CommandTemplate *alts[NL_MAX_ALTS];
    int nalts = 0;
    for (uint8_t k = 0; k < m->alt_count; k++) {
        char err[64];
        if (!nl_expand(ctx, m->alts[k], alt_cmd[nalts], sizeof alt_cmd[nalts], err, sizeof err))
            continue;
        if (strcmp(alt_cmd[nalts], cmd) == 0) continue;
        bool dup = false;
        for (int j = 0; j < nalts; j++) if (strcmp(alt_cmd[j], alt_cmd[nalts]) == 0) dup = true;
        if (!dup) alts[nalts++] = m->alts[k];
    }
    if (!m->low_confidence && nalts == 0) return false;
    if (!report) return true;

    if (m->low_confidence) {
        if (m->best_tokens) {
            fprintf(stderr, "nls: low-confidence match: only ");
            print_tokens(ctx, m->best_tokens, true);
            fprintf(stderr, " matched; ignored ");
        } else {
            fprintf(stderr, "nls: low-confidence match: no keyword matched; ignored ");
        }
        print_tokens(ctx, m->best_tokens, false);
        fputc('\n', stderr);
    }
    if (nalts > 0) {
        fprintf(stderr, "nls: picked \"%s\" (%s); this request also fits:\n",
                m->best->description, m->best->id);
        for (int k = 0; k < nalts; k++)
            fprintf(stderr, "  %-22s %s%s\n  %-22s   %s\n", alts[k]->id, alts[k]->description,
                    nl_danger(alts[k]) ? " (destructive)" : "", "", alt_cmd[k]);
        fprintf(stderr, "nls: rephrase to choose (see 'nls -v' for scores)\n");
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Compiling one request for one tool                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    ParseContext ctx;
    JsonInfo json;
    MatchResult m;                  /* unused when `combined`             */
    bool combined;                  /* jq: several conditions, no template */
    char cmd[NL_MAX_CMD_LEN];
    char dry[NL_MAX_CMD_LEN];       /* "" if none                         */
    const Danger *danger;
} Compiled;

/* Messages for the user; nothing at all while probing tools (`quiet`). */
static void say(bool quiet, const char *fmt, ...) {
    if (quiet) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* Compile `prompt` for `tool` into c->cmd. Returns 0, or the exit code
 * after saying why not (unless `quiet`). */
static int compile(ToolId tool, const char *prompt, bool verbose, bool quiet, Compiled *c) {
    ParseContext *ctx = &c->ctx;
    MatchResult *m = &c->m;
    char err[256];
    memset(m, 0, sizeof *m);
    c->combined = false;
    c->cmd[0] = c->dry[0] = '\0';
    c->danger = NULL;
    if (quiet) verbose = false;

    if (!nl_lex(ctx, tool, prompt, NULL)) {
        say(quiet, "nls: prompt too long or has too many words\n");
        return 2;
    }
    /* jq: if the input file is readable, learn its shape and field names,
     * then lex again so bare words like "emails" can resolve to .email */
    bool have_json = false;
    if (tool == TOOL_JQ) {
        const char *file = nl_find_slot(ctx, SLOT_FILE, 0);
        const char *ext = file ? strrchr(file, '.') : NULL;
        bool lines = ext && (strcmp(ext, ".jsonl") == 0 || strcmp(ext, ".ndjson") == 0);
        if (file && !lines && nl_inspect_json(file, &c->json)) {
            char shape = c->json.shape;
            nl_lex(ctx, tool, prompt, &c->json);
            ctx->input_shape = shape;
            have_json = true;
        }
    }
    if (verbose) {
        print_context(ctx);
        if (ctx->input_shape)
            fprintf(stderr, "input: %s\n", ctx->input_shape == '[' ? "array" : "object");
    }

    /* grep: "search for TODO in src" with no src here */
    if (tool == TOOL_GREP && ctx->missing_path) {
        say(quiet, "nls: no file or folder named '%s' here.\n"
                   "Give the path as it is on disk (e.g. 'src/' or './%s').\n",
            ctx->missing_path, ctx->missing_path);
        return 1;
    }

    /* jq: "where .age around 30": refuse rather than guess the comparison */
    if (tool == TOOL_JQ && ctx->jq_bad_op) {
        say(quiet, "nls: don't know how to compare %s \"%s\" ...\n"
                   "Try: is, is not, >, <, at least, at most, between X and Y, "
                   "outside X and Y, contains, matches, exists\n",
            ctx->jq_bad_path, ctx->jq_bad_op);
        return 1;
    }

    /* jq: "where .a is x and .b > 3" */
    if (tool == TOOL_JQ && ctx->cond_paths > 1) {
        if (!nl_jq_compound(prompt, have_json ? &c->json : NULL, verbose, quiet, c->cmd,
                            sizeof c->cmd, err, sizeof err)) {
            if (err[0]) say(quiet, "nls: %s\n", err);
            else say(quiet, "nls: found several conditions; join them with \"and\" or \"or\" "
                            "(e.g. \"where .a is x and .b > 3\")\n");
            return 1;
        }
        c->combined = true;
        return 0;
    }

    if (verbose && ctx->flags[0]) fprintf(stderr, "modifiers: %s\n", ctx->flags);

    nl_match(ctx, m, verbose);

    if (!m->best || (m->blocked && m->blocked_score > m->best_score)) {
        if (m->wrong_shape && m->wrong_shape_score >= m->blocked_score) {
            say(quiet, "nls: \"%s\" needs a JSON %s, but %s is an %s.\n",
                m->wrong_shape->description,
                m->wrong_shape->shape == SHAPE_ARRAY ? "array" : "object",
                nl_find_slot(ctx, SLOT_FILE, 0),
                ctx->input_shape == '[' ? "array" : "object");
            say(quiet, "Run 'nls jq \"keys of %s\"' to see what it contains.\n",
                nl_find_slot(ctx, SLOT_FILE, 0));
        } else if (m->blocked) {
            say(quiet, "nls: this looks like \"%s\" (%s), but it needs:\n",
                m->blocked->description, m->blocked->id);
            for (uint8_t k = 0; k < m->missing_count; k++)
                say(quiet, "  - %s\n", nl_slot_hint(m->missing[k]));
            say(quiet, "example: nls %s \"%s\"\n", nl_tool_name(tool), m->blocked->example);
        } else {
            say(quiet, "nls: couldn't understand that request for %s.\n"
                       "Run 'nls --list %s' to see what's supported.\n",
                nl_tool_name(tool), nl_tool_name(tool));
        }
        return 1;
    }

    if (verbose) fprintf(stderr, "match: %s (score %d)\n", m->best->id, m->best_score);

    /* jq: a filter with several paths and none of them clearly the
     * condition: refuse rather than guess which one to test. */
    if (tool == TOOL_JQ && strncmp(m->best->id, "jq_filter_", 10) == 0 &&
        ctx->cond_paths == 0 && nl_count_slots(ctx, SLOT_JSON_PATH) > 1) {
        say(quiet, "nls: not sure which of %s and %s is the condition.\n"
                   "Put the condition after \"where\" (e.g. \"get .name where .age > 30\").\n",
            nl_find_slot(ctx, SLOT_JSON_PATH, 0), nl_find_slot(ctx, SLOT_JSON_PATH, 1));
        return 1;
    }

    if (!nl_expand(ctx, m->best, c->cmd, sizeof c->cmd, err, sizeof err)) {
        say(quiet, "nls: %s (%s)\n", err, m->best->id);
        return 1;
    }

    /* A destructive command's dry run comes from the same request: the
     * template with its preview text in place of the command. */
    c->danger = nl_danger(m->best);
    if (c->danger && c->danger->dry_run) {
        CommandTemplate preview = *m->best;
        preview.flag_template = c->danger->dry_run;
        if (!nl_expand(ctx, &preview, c->dry, sizeof c->dry, err, sizeof err)) {
            if (verbose) fprintf(stderr, "dry run: can't build it: %s\n", err);
            c->dry[0] = '\0';
        }
    }
    if (verbose && c->danger) {
        fprintf(stderr, "destructive: %s\n", c->danger->what);
        if (c->dry[0]) fprintf(stderr, "dry run: %s\n", c->dry);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Inferring the tool                                                 */
/* ------------------------------------------------------------------ */

/* Points a tool gets on top of its best template's keyword score for what
 * the prompt names: files of its kind, a URL, an archive, its own name. A
 * keyword match is worth 6-10, so one such file outweighs a stray word. */
#define EV_NAME     30   /* "with ffmpeg", "jq: ..."                      */
#define EV_URL      25   /* http(s)://, localhost:3000                    */
#define EV_ARCHIVE  20   /* backup.tar.gz                                 */
#define EV_FILE     15   /* in.mp4, users.json, hero.png                  */
#define EV_WORDS    10   /* commit, branch, stash, ...                    */
/* A runner-up within this many points that gives another command is
 * reported, and makes the guess uncertain. */
#define EV_CLOSE    5

static const char *const MEDIA_EXTS[] = {
    "mp4", "mkv", "mov", "avi", "webm", "flv", "wmv", "m4v", "mpg", "mpeg", "3gp",
    "mp3", "wav", "aac", "m4a", "flac", "ogg", "opus", "wma", "gif", NULL
};
static const char *const IMAGE_EXTS[] = {
    "png", "jpg", "jpeg", "webp", "bmp", "tif", "tiff", "heic", "heif", "avif", "ico",
    "svg", "psd", "gif", NULL
};
static const char *const JSON_FILE_EXTS[] = { "json", "jsonl", "ndjson", "geojson", "har", NULL };
static const char *const GIT_WORDS[] = {
    "commit", "commits", "committed", "branch", "branches", "stash", "stashed", "stashes",
    "rebase", "push", "pull", "pushed", "staged", "unstaged", "unstage", "upstream", "head",
    "reflog", "blame", "cherry-pick", "repo", "repository", "amend", "checkout", "origin",
    "untracked", "merge", "merged", "reset", "revert", "squash", "tag", "tags", "remote",
    "fetch", "conflict", "conflicts", "diff", "history", "restore", "discard", "track",
    "tracking", "tracked", "switch", "renames", "who", "changes", NULL
};
/* "convert all png files", "resize all jpgs": image batches name a format */
static const char *const IMAGE_WORDS[] = {
    "png", "pngs", "jpg", "jpgs", "jpeg", "jpegs", "webp", "heic", "avif", "image", "images",
    "photo", "photos", "picture", "pictures", NULL
};
/* Searching inside files rather than for them... */
static const char *const GREP_WORDS[] = {
    "contain", "contains", "containing", "mention", "mentions", "mentioning", "mentioned",
    "matching", "occurrence", "occurrences", "pattern", "regex", NULL
};
/* ... and selecting files by what they are. */
static const char *const FIND_WORDS[] = {
    "named", "called", "larger", "bigger", "smaller", "older", "newer", "modified",
    "accessed", "empty", "folders", "directories", "subdirectories", NULL
};
/* .git, .log, .env: names, not JSON paths */
static const char *const DOT_NAMES[] = {
    "git", "log", "env", "tmp", "bak", "swp", "cache", "ds_store", "gitignore", "txt", "md",
    "py", "c", "h", "js", "ts", "json", "yml", "yaml", "toml", "o", "so", "zip", "gz", "tar",
    "csv", "html", "css", "sh", "rs", "go", "java", "rb", "php", "xml", "lock", "svn", "idea",
    "vscode", "pyc", "class", "jar", "old", "orig", "conf", "cfg", "ini", "jpg", "png", "gif",
    "mp4", "mp3", "pdf", NULL
};
/* Tools that only count with evidence: without a file of their kind (or a
 * git word, a URL) their keywords are too generic ("compress src" is not
 * jq's "compact", "merge feature-x" is not jq's "slurp"). */
static const bool NEEDS_EVIDENCE[TOOL_COUNT] = {
    [TOOL_FFMPEG] = true, [TOOL_JQ] = true, [TOOL_GIT] = true, [TOOL_CURL] = true,
    [TOOL_MAGICK] = true,
};
/* Tools nls doesn't cover (yet): named first, they are refused rather
 * than read as part of a request for another tool. */
static const char *const UNSUPPORTED[] = {
    "openssl", "rsync", "sed", "awk", "gawk", "docker", "ssh", "scp", "sftp", "kubectl",
    "rg", "fd", "7z", "npm", "pip", "aws", "gh", "sox", "pandoc", "psql", "mysql", "sqlite3",
    "systemctl", "journalctl", "brew", "apt", "yq", "xargs", "chmod", "chown", "lsof", "cmake",
    NULL
};

static bool word_in(const char *w, const char *const *list) {
    for (size_t i = 0; list[i]; i++)
        if (strcmp(w, list[i]) == 0) return true;
    return false;
}

/* The lowercased extension of a name.ext word, "" if it has none. */
static void word_ext(const char *w, char *ext, size_t size) {
    ext[0] = '\0';
    const char *slash = strrchr(w, '/');
    const char *base = slash ? slash + 1 : w;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base || strlen(dot + 1) >= size) return;
    size_t k = 0;
    for (const char *p = dot + 1; *p; p++) ext[k++] = (char)tolower((unsigned char)*p);
    ext[k] = '\0';
}

/* Split the prompt into lowercased words (quotes and edge punctuation
 * dropped) and call `fn` on each. */
static void each_word(const char *prompt, void (*fn)(const char *w, bool quoted, void *arg),
                      void *arg) {
    char w[256];
    const char *p = prompt;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        size_t n = 0;
        bool quoted = *p == '\'' || *p == '"';
        if (quoted) {
            char q = *p++;
            while (*p && *p != q) { if (n + 1 < sizeof w) w[n++] = *p; p++; }
            if (*p) p++;
        } else {
            while (*p && !isspace((unsigned char)*p)) { if (n + 1 < sizeof w) w[n++] = *p; p++; }
        }
        while (n > 0 && strchr(",;!?):", w[n - 1])) n--;
        while (n > 1 && w[n - 1] == '.' && w[n - 2] != '.') n--;
        w[n] = '\0';
        char *s = w;
        if (*s == '(') s++;
        for (char *q = s; *q; q++) *q = (char)tolower((unsigned char)*q);
        if (*s) fn(s, quoted, arg);
    }
}

typedef struct {
    int points[TOOL_COUNT];
    const char *why[TOOL_COUNT];    /* the strongest evidence, for -v */
    char words[TOOL_COUNT][64];
    bool prev_quoted;
    bool searching;                 /* "search", "look", "grep", "scan" seen */
    bool prev_for;                  /* the previous word was "for"           */
} Evidence;

static void credit(Evidence *ev, ToolId t, int pts, const char *why, const char *w) {
    if (pts <= ev->points[t]) return;   /* the strongest clue counts, once */
    ev->points[t] = pts;
    ev->why[t] = why;
    snprintf(ev->words[t], sizeof ev->words[t], "%s", w);
}

static void evidence_word(const char *w, bool quoted, void *arg) {
    Evidence *ev = arg;
    bool after_quoted = ev->prev_quoted, after_for = ev->prev_for;
    ev->prev_quoted = quoted;
    ev->prev_for = !quoted && strcmp(w, "for") == 0;
    char ext[16];
    word_ext(w, ext, sizeof ext);
    if (strncmp(w, "http://", 7) == 0 || strncmp(w, "https://", 8) == 0 ||
        strncmp(w, "localhost:", 10) == 0)
        credit(ev, TOOL_CURL, EV_URL, "a URL", w);
    if (nl_tar_compression(w, true))
        credit(ev, TOOL_TAR, EV_ARCHIVE, "an archive", w);
    else if (ext[0]) {
        if (word_in(ext, MEDIA_EXTS)) credit(ev, TOOL_FFMPEG, EV_FILE, "a media file", w);
        if (word_in(ext, IMAGE_EXTS)) credit(ev, TOOL_MAGICK, EV_FILE, "an image", w);
        if (word_in(ext, JSON_FILE_EXTS)) credit(ev, TOOL_JQ, EV_FILE, "a JSON file", w);
    }
    if (quoted) return;   /* a pattern or a message: its words are no clue */
    if (word_in(w, GIT_WORDS)) credit(ev, TOOL_GIT, EV_WORDS, "a git word", w);
    if (word_in(w, IMAGE_WORDS)) credit(ev, TOOL_MAGICK, EV_WORDS, "an image word", w);
    if (word_in(w, GREP_WORDS)) credit(ev, TOOL_GREP, EV_WORDS, "a search word", w);
    if (word_in(w, FIND_WORDS)) credit(ev, TOOL_FIND, EV_WORDS, "a file test", w);
    /* "'TODO' in src/": a quoted pattern looked for in a place */
    if (after_quoted && strcmp(w, "in") == 0) credit(ev, TOOL_GREP, EV_WORDS, "a pattern", "in");
    /* "search python files for TODO": but "look for large files" is find */
    static const char *const SEARCH_VERBS[] = { "search", "look", "grep", "scan", NULL };
    static const char *const NOT_PATTERNS[] = {
        "large", "big", "huge", "small", "tiny", "file", "files", "old", "new", "recent",
        "empty", "hidden", "all", "any", "every", "the", "a", "an", "some", NULL
    };
    if (word_in(w, SEARCH_VERBS)) ev->searching = true;
    else if (after_for && ev->searching && !word_in(w, NOT_PATTERNS) && !word_in(w, FIND_WORDS))
        credit(ev, TOOL_GREP, EV_WORDS, "a pattern", w);
    /* v1.0, v2.3.1: a tag */
    if (w[0] == 'v' && isdigit((unsigned char)w[1]) && strspn(w + 1, "0123456789.") == strlen(w + 1))
        credit(ev, TOOL_GIT, EV_WORDS, "a tag", w);
    /* api.github.com/user: a host and a path */
    const char *slash = strchr(w, '/');
    if (slash && slash > w && w[0] != '.' && w[0] != '~' && !strchr(w, '*')) {
        const char *dot = NULL;
        for (const char *q = w; q < slash; q++) if (*q == '.') dot = q;
        const char *tld = dot ? dot + 1 : NULL;
        size_t tl = tld ? strspn(tld, "abcdefghijklmnopqrstuvwxyz") : 0;
        if (tld && tl >= 2 && tld + tl == slash && !strchr(tld, '.'))
            credit(ev, TOOL_CURL, EV_URL, "a URL", w);
    }
    /* .users[].email, .config.port, .[0]: a JSON path */
    if (w[0] == '.' && (w[1] == '[' || (isalpha((unsigned char)w[1]) &&
                                        (strchr(w + 1, '[') || strchr(w + 1, '.')))))
        credit(ev, TOOL_JQ, EV_FILE, "a JSON path", w);
    else if (w[0] == '.' && isalpha((unsigned char)w[1]) && !word_in(w + 1, DOT_NAMES) &&
             strspn(w + 1, "abcdefghijklmnopqrstuvwxyz0123456789_") == strlen(w + 1))
        credit(ev, TOOL_JQ, EV_WORDS, "a JSON field", w);
    if (strcmp(w, "json") == 0) credit(ev, TOOL_JQ, EV_FILE, "the word", w);
    /* a tool's own name, except words that are ordinary English */
    ToolId named = nl_tool_from_name(w);
    if (named != TOOL_UNKNOWN && strcmp(w, "find") != 0 && strcmp(w, "convert") != 0)
        credit(ev, named, EV_NAME, "its name", w);
}

typedef struct {
    ToolId tool;
    int score;          /* keyword score + evidence */
} Fit;

/* Compile the prompt for every tool and pick the one that fits best: its
 * best template's keyword score plus the evidence above. A tool whose
 * match rests on no keyword and no evidence (grep's plain search, curl's
 * plain GET) doesn't count. Returns TOOL_UNKNOWN if nothing fits, after
 * explaining why. `*close_call` is set when another tool came close. */
static ToolId infer_tool(const char *prompt, bool verbose, bool *close_call) {
    static Compiled probe;
    static char cmds[TOOL_COUNT][NL_MAX_CMD_LEN];
    Evidence ev;
    memset(&ev, 0, sizeof ev);
    each_word(prompt, evidence_word, &ev);

    Fit fits[TOOL_COUNT];
    int nfits = 0;
    ToolId blocked = TOOL_UNKNOWN;     /* best tool that knew what was meant */
    int blocked_score = 0;
    *close_call = false;

    if (verbose) fprintf(stderr, "tools:\n");
    for (int t = TOOL_UNKNOWN + 1; t < TOOL_COUNT; t++) {
        int rc = compile((ToolId)t, prompt, false, true, &probe);
        int kw = probe.combined ? NL_CONFIDENT_SCORE * 2 : probe.m.best_score;
        int score = kw + ev.points[t];
        /* a lone shared prefix (6: "compare" ~ "compress") is no reason */
        bool counts = rc == 0 && (kw > 6 || ev.points[t] > 0) &&
                      (!NEEDS_EVIDENCE[t] || ev.points[t] > 0);
        if (verbose) {
            fprintf(stderr, "  %-8s score=%-3d", nl_tool_name((ToolId)t), counts ? score : 0);
            if (ev.points[t]) fprintf(stderr, " (+%d: %s %s)", ev.points[t], ev.why[t], ev.words[t]);
            if (rc == 0 && counts)
                fprintf(stderr, " %s\n", probe.combined ? "jq_filter_combined" : probe.m.best->id);
            else if (rc == 0) fprintf(stderr, " %s\n", kw > 0 ? "no clue in the request" : "no keyword or clue");
            else fprintf(stderr, " %s\n", probe.m.blocked ? "blocked" : "no match");
        }
        if (!counts) {
            /* "cut video.mp4" names no times: ffmpeg should say what's missing */
            int bs = probe.m.blocked && ev.points[t] > 0 ? probe.m.blocked_score + ev.points[t] : 0;
            if (rc != 0 && bs > blocked_score) { blocked = (ToolId)t; blocked_score = bs; }
            continue;
        }
        snprintf(cmds[t], sizeof cmds[t], "%s", probe.cmd);
        Fit f = { (ToolId)t, score };
        int pos = nfits++;
        while (pos > 0 && fits[pos - 1].score < f.score) { fits[pos] = fits[pos - 1]; pos--; }
        fits[pos] = f;
    }

    if (nfits == 0 || (blocked != TOOL_UNKNOWN && blocked_score > fits[0].score)) {
        if (blocked != TOOL_UNKNOWN) {
            /* say what that tool was missing */
            compile(blocked, prompt, false, false, &probe);
            fprintf(stderr, "nls: (guessed the tool: %s; for another, name it: nls <tool> \"...\")\n",
                    nl_tool_name(blocked));
        } else {
            fprintf(stderr, "nls: couldn't tell which tool this is for.\n"
                            "Name it: nls <tool> \"...\" (" NL_TOOL_LIST "),\n"
                            "or run 'nls --list' to see what's supported.\n");
        }
        return TOOL_UNKNOWN;
    }

    ToolId best = fits[0].tool;
    for (int k = 1; k < nfits; k++) {
        if (fits[k].score + EV_CLOSE < fits[0].score) break;
        if (strcmp(cmds[fits[k].tool], cmds[best]) == 0) continue;
        if (!*close_call)
            fprintf(stderr, "nls: guessed the tool: %s; this could also be for:\n",
                    nl_tool_name(best));
        *close_call = true;
        fprintf(stderr, "  %-8s %s\n", nl_tool_name(fits[k].tool), cmds[fits[k].tool]);
    }
    if (*close_call)
        fprintf(stderr, "nls: name the tool to choose: nls %s \"...\"\n", nl_tool_name(best));
    if (verbose) fprintf(stderr, "tool: %s\n", nl_tool_name(best));
    return best;
}

/* ------------------------------------------------------------------ */
/* Output                                                             */
/* ------------------------------------------------------------------ */

/* Print the command, or show it and run it (-x). Running asks first unless
 * -y, and -y doesn't skip that for an uncertain match, nor for a
 * destructive command without --force. A destructive command is explained
 * and its dry run (`dry`, "" if none) runs before the question; only a
 * typed "y" runs it, and a failed dry run stops it. With --explain, the
 * breakdown follows the command (on stdout, or on stderr with -x). */
static int finish(const char *cmd, const Danger *danger, const char *dry, RunMode mode,
                  bool uncertain) {
    if (!mode.exec) {
        printf("%s\n", cmd);
        if (mode.explain) nl_explain(cmd, stdout);
        return 0;
    }
    fprintf(stderr, "\xe2\x86\x92 %s\n", cmd);
    if (mode.explain) nl_explain(cmd, stderr);
    bool ask_first = !mode.yes || uncertain || (danger && !mode.force);
    if (!ask_first) return run_command(cmd);

    if (danger) {
        fprintf(stderr, "nls: destructive: %s\n", danger->what);
        if (dry[0]) {
            fprintf(stderr, "nls: dry run: %s\n", dry);
            int rc = run_dry_run(dry);
            if (rc != 0) {
                fprintf(stderr, "nls: the dry run failed (exit %d), so the command was not run\n", rc);
                return 1;
            }
        }
    }
    if (mode.yes && uncertain)
        fprintf(stderr, "nls: not running an uncertain match without confirmation (-y ignored)\n");
    else if (mode.yes)
        fprintf(stderr, "nls: not running a destructive command without confirmation "
                        "(-y ignored; add --force to skip it)\n");
    if (danger ? !confirm_destructive() : !confirm()) {
        fprintf(stderr, "nls: aborted\n");
        return 130;
    }
    return run_command(cmd);
}

/* ------------------------------------------------------------------ */
/* Shell integration: --shell zsh|bash                                */
/* ------------------------------------------------------------------ */

/* The widget sends the typed line (minus a leading "nls ") to nls and puts
 * the command in its place, ready to edit or run. nls's messages are shown
 * under the line (zsh) or above it (bash); on an error the line stays. */
static const char ZSH_WIDGET[] =
    "# nls shell integration for zsh: eval \"$(nls --shell zsh)\"\n"
    "# Type a request, press Ctrl+G (NLS_KEY to change it, before the eval):\n"
    "# the line becomes the command. Ctrl+_ (undo) brings the request back.\n"
    "nls-widget() {\n"
    "  emulate -L zsh\n"
    "  setopt extended_glob\n"
    "  local req=${BUFFER##[[:space:]]#} out err rc errf\n"
    "  [[ $req == nls[[:space:]]* ]] && req=${req#nls}\n"
    "  req=${req##[[:space:]]#}\n"
    "  [[ -z $req ]] && return 0\n"
    "  errf=$(mktemp \"${TMPDIR:-/tmp}/nls.XXXXXX\") || return 1\n"
    "  out=$(command nls -- \"$req\" 2>\"$errf\" </dev/null)\n"
    "  rc=$?\n"
    "  err=$(<\"$errf\")\n"
    "  command rm -f -- \"$errf\"\n"
    "  if (( rc == 0 )) && [[ -n $out ]]; then\n"
    "    BUFFER=$out\n"
    "    CURSOR=${#BUFFER}\n"
    "  fi\n"
    "  [[ -n $err ]] && zle -M \"$err\"\n"
    "  return 0\n"
    "}\n"
    "zle -N nls-widget\n"
    "bindkey -- \"${NLS_KEY:-^G}\" nls-widget\n";

static const char BASH_WIDGET[] =
    "# nls shell integration for bash (4.0 or later): eval \"$(nls --shell bash)\"\n"
    "# Type a request, press Ctrl+G (NLS_KEY to change it, before the eval,\n"
    "# in readline syntax: NLS_KEY='\\C-x\\C-n'): the line becomes the command.\n"
    "if [ \"${BASH_VERSINFO[0]:-0}\" -lt 4 ]; then\n"
    "  echo \"nls: the shell integration needs bash 4.0 or later (this is $BASH_VERSION)\" >&2\n"
    "else\n"
    "  _nls_widget() {\n"
    "    local req out rc\n"
    "    req=${READLINE_LINE#\"${READLINE_LINE%%[![:space:]]*}\"}\n"
    "    case $req in nls[[:space:]]*) req=${req#nls} ;; esac\n"
    "    req=${req#\"${req%%[![:space:]]*}\"}\n"
    "    [ -z \"$req\" ] && return 0\n"
    "    out=$(command nls -- \"$req\" </dev/null)\n"
    "    rc=$?\n"
    "    if [ $rc -eq 0 ] && [ -n \"$out\" ]; then\n"
    "      READLINE_LINE=$out\n"
    "      READLINE_POINT=${#READLINE_LINE}\n"
    "    fi\n"
    "  }\n"
    "  bind -x \"\\\"${NLS_KEY:-\\\\C-g}\\\": _nls_widget\"\n"
    "fi\n";

static int print_shell(const char *shell) {
    if (!shell) {
        fprintf(stderr, "nls: --shell needs a shell: zsh or bash\n");
        return 2;
    }
    const char *base = strrchr(shell, '/');
    base = base ? base + 1 : shell;
    if (strcmp(base, "zsh") == 0) fputs(ZSH_WIDGET, stdout);
    else if (strcmp(base, "bash") == 0) fputs(BASH_WIDGET, stdout);
    else {
        fprintf(stderr, "nls: no shell integration for '%s' (supported: zsh, bash)\n", shell);
        return 2;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

/* "'compress my files/ into b.tar.gz'": a request quoted as a whole, as it
 * reaches nls from the shell widget (nls tar '...' typed on the line).
 * Drop the outer quotes so the words are words again. */
static void unwrap_quotes(char *p) {
    size_t lead = strspn(p, " \t");
    size_t n = strlen(p);
    while (n > lead && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    if (n - lead < 2) return;
    char q = p[lead];
    if ((q != '\'' && q != '"') || p[n - 1] != q || memchr(p + lead + 1, q, n - lead - 2))
        return;
    memmove(p, p + lead + 1, n - lead - 2);
    p[n - lead - 2] = '\0';
}

static int unsupported(const char *name) {
    fprintf(stderr, "nls: unsupported tool '%s' (supported in this version: " NL_TOOL_LIST ")\n",
            name);
    return 2;
}

int main(int argc, char **argv) {
    RunMode mode = { false, false, false, false };
    bool verbose = false;
    int i = 1;

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) { i++; break; }
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "-x") == 0 || strcmp(a, "--exec") == 0) mode.exec = true;
        else if (strcmp(a, "-y") == 0 || strcmp(a, "--yes") == 0) mode.yes = true;
        else if (strcmp(a, "--force") == 0) mode.force = true;
        else if (strcmp(a, "-e") == 0 || strcmp(a, "--explain") == 0) mode.explain = true;
        else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) verbose = true;
        else if (strcmp(a, "-xy") == 0 || strcmp(a, "-yx") == 0) mode.exec = mode.yes = true;
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(stdout); return 0; }
        else if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) { printf("nls %s\n", NL_VERSION_FULL); return 0; }
        else if (strcmp(a, "--shell") == 0) return print_shell(i + 1 < argc ? argv[i + 1] : NULL);
        else if (strncmp(a, "--shell=", 8) == 0) return print_shell(a + 8);
        else if (strcmp(a, "-l") == 0 || strcmp(a, "--list") == 0) {
            ToolId only = TOOL_UNKNOWN;
            if (i + 1 < argc) {
                only = nl_tool_from_name(argv[i + 1]);
                if (only == TOOL_UNKNOWN) {
                    fprintf(stderr, "nls: unknown tool '%s' (supported: " NL_TOOL_LIST ")\n", argv[i + 1]);
                    return 2;
                }
            }
            list_templates(only);
            return 0;
        } else {
            fprintf(stderr, "nls: unknown option '%s'\n", a);
            usage(stderr);
            return 2;
        }
    }

    if (argc - i < 1) { usage(stderr); return 2; }

    /* The tool is the first argument when it names one ("nls git undo"),
     * or the first word of a single-argument request ("nls 'git undo'",
     * as the shell widget sends it). Otherwise it is inferred. */
    ToolId tool = nl_tool_from_name(argv[i]);
    char prompt[NL_MAX_INPUT];
    if (tool != TOOL_UNKNOWN) {
        if (argc - i < 2) { usage(stderr); return 2; }
        if (!join_prompt(argc, argv, i + 1, prompt, sizeof prompt)) {
            fprintf(stderr, "nls: prompt too long (max %d bytes)\n", NL_MAX_INPUT - 1);
            return 2;
        }
    } else {
        if (word_in(argv[i], UNSUPPORTED)) return unsupported(argv[i]);
        if (!join_prompt(argc, argv, i, prompt, sizeof prompt)) {
            fprintf(stderr, "nls: prompt too long (max %d bytes)\n", NL_MAX_INPUT - 1);
            return 2;
        }
        const char *p = prompt + strspn(prompt, " \t");
        size_t n = strcspn(p, " \t");
        char first[32];
        if (n > 0 && n < sizeof first && p[n] != '\0') {
            memcpy(first, p, n);
            first[n] = '\0';
            if (word_in(first, UNSUPPORTED)) return unsupported(first);
            /* "convert" and "find" are verbs here ("find TODO in src" is a grep) */
            ToolId named = strcmp(first, "convert") == 0 || strcmp(first, "find") == 0
                         ? TOOL_UNKNOWN : nl_tool_from_name(first);
            if (named != TOOL_UNKNOWN && p[n + strspn(p + n, " \t")] != '\0') {
                tool = named;
                memmove(prompt, p + n + 1, strlen(p + n + 1) + 1);
            }
        }
    }

    unwrap_quotes(prompt);
    bool close_call = false;
    if (tool == TOOL_UNKNOWN) {
        tool = infer_tool(prompt, verbose, &close_call);
        if (tool == TOOL_UNKNOWN) return 1;
    }

    static Compiled c;
    int rc = compile(tool, prompt, verbose, false, &c);
    if (rc != 0) return rc;
    bool uncertain = close_call ||
                     (!c.combined && check_uncertain(&c.ctx, &c.m, c.cmd, true));
    return finish(c.cmd, c.danger, c.dry, mode, uncertain);
}
