/*
 * fuzz_nls.c — fuzz the lexer, matcher, expanders and --explain.
 *
 * Input: byte 0 picks the tool, the rest is the request (up to the first
 * NUL, at most NL_MAX_INPUT - 1 bytes). For each input the harness does
 * what `nls <tool> "<request>"` does, minus running anything:
 *
 *   nl_lex -> nl_jq_compound (several jq conditions) or nl_match
 *          -> nl_expand for every template whose required slots are there
 *             (the match, its runners-up and the rest), plus its dry run
 *          -> nl_explain on each command
 *
 * Besides the sanitizers (crashes, overflows, UB), every command built is
 * checked by check_command(): its quotes must balance, it must hold no
 * control characters, and the shell must see no special character the
 * template didn't put there itself. A ';', '|', '$(' or backquote that came
 * from the request and survived unquoted would be a command injection.
 *
 * Two builds (see `make fuzz` / `make fuzz-check`):
 *   - with libFuzzer (clang -fsanitize=fuzzer): coverage-guided.
 *   - standalone (-DNL_FUZZ_MAIN, any compiler, e.g. Apple clang): replays
 *     the files given, then mutates them at random for -runs=N inputs.
 */
#define _POSIX_C_SOURCE 200809L   /* standalone driver: open, readdir, signal */
#include "../../src/nl.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The tool for byte 0: every tool, never TOOL_UNKNOWN. */
static ToolId pick_tool(uint8_t b) {
    return (ToolId)(1 + b % (TOOL_COUNT - 1));
}

/* ------------------------------------------------------------------ */
/* The oracle                                                         */
/* ------------------------------------------------------------------ */

/* Characters the shell treats specially when they are unquoted. '~' and
 * '=' only matter in places nls doesn't build, '#' only at a word start:
 * they're left out. */
#define SHELL_SPECIAL ";&|<>()$`\\\"'*?[]{}!\n"

static FILE *devnull;

static void fail(const char *why, const char *cmd, const CommandTemplate *tpl) {
    fprintf(stderr, "\nnls fuzz: %s\n  template: %s\n  command:  %s\n",
            why, tpl ? tpl->id : "(jq compound)", cmd);
    abort();
}

/* Unquoted special characters in a template's text, with its placeholder
 * syntax ({...} and << >>) taken out. A builder may emit one of these, the
 * request can't. */
static void template_specials(const char *t, bool allowed[256]) {
    if (!t) return;
    char q = 0;
    for (const char *p = t; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (q == '\'') { if (c == '\'') q = 0; continue; }
        if (c == '{' && p[1] >= 'A' && p[1] <= 'Z') {           /* placeholder */
            const char *e = strchr(p, '}');
            if (e) { p = e; continue; }
        }
        if ((c == '<' && p[1] == '<') || (c == '>' && p[1] == '>')) { p++; continue; }
        if (c == '\\' && p[1]) { allowed[(unsigned char)'\\'] = true; p++; continue; }
        if (c == '\'' && q == 0) { q = '\''; allowed[c] = true; continue; }
        if (c == '"') { q = q == '"' ? 0 : '"'; allowed[c] = true; continue; }
        if (q == '"' && c != '$' && c != '`') continue;
        if (strchr(SHELL_SPECIAL, c)) allowed[c] = true;
    }
}

/* Builder output that isn't in any template's text: what the per-tool
 * expanders and the shell quoting add on their own. Keep it short: each
 * character here is one the oracle no longer checks for that tool. */
static void builder_specials(ToolId tool, bool allowed[256]) {
    const char *extra = "'\\";                    /* nl_shell_quote: 'it'\''s' */
    switch (tool) {
    case TOOL_FIND:   extra = "'\\()*";  break;   /* \( -name X -o ... \), '*\/X' */
    case TOOL_MAGICK: extra = "'\\&*";   break;   /* mkdir -p d && ..., globs   */
    case TOOL_CURL:   extra = "'\\\"$";  break;   /* -H "Authorization: $TOKEN" */
    case TOOL_GIT:    extra = "'\\\"$()"; break;   /* --author="$(git config ...)" */
    default: break;
    }
    for (const char *p = extra; *p; p++) allowed[(unsigned char)*p] = true;
}

/* `cmd` as the shell would read it: quotes balance, no control characters
 * anywhere, and outside single quotes no special character the template
 * (or its tool's builders) can't produce. Inside double quotes only '$',
 * '`' and '\' are special; "$NAME" (a plain variable) is allowed where '$'
 * is, "$(" and "${" only where the template has them. */
static void check_command(const char *cmd, const CommandTemplate *tpl, ToolId tool) {
    bool allowed[256] = { false };
    if (tpl) {
        template_specials(tpl->flag_template, allowed);
        const Danger *d = nl_danger(tpl);
        if (d) template_specials(d->dry_run, allowed);
    } else {
        allowed[(unsigned char)'\''] = true;      /* jq compound: jq '...' file */
    }
    builder_specials(tool, allowed);

    /* The quoting context, innermost last: ' and " quote, ( is a "$(...)"
     * (inside which quoting starts over, even within double quotes). */
    char stack[64];
    int sp = 0;
    for (const char *p = cmd; *p; p++) {
        unsigned char c = (unsigned char)*p;
        char top = sp ? stack[sp - 1] : 0;
        if ((c < 0x20 && c != '\t') || c == 0x7f) fail("control character in the command", cmd, tpl);
        if (top == '\'') { if (c == '\'') sp--; continue; }
        if (top == '"') {
            if (c == '"') { sp--; continue; }
            if (c == '\\' && p[1]) { p++; continue; }
            if (c != '$' && c != '`') continue;
        } else if (c == '\'' || c == '"') {
            if (!allowed[c]) fail("a quote not from the template", cmd, tpl);
            if (sp == (int)sizeof stack) fail("quoting nested too deep", cmd, tpl);
            stack[sp++] = (char)c;
            continue;
        } else if (c == '\\' && p[1]) {
            if (!allowed['\\']) fail("unquoted backslash not from the template", cmd, tpl);
            p++;
            continue;
        } else if (c == ')' && top == '(') {
            sp--;
            continue;
        }
        if (!strchr(SHELL_SPECIAL, c)) continue;
        if (!allowed[c]) {
            char why[64];
            snprintf(why, sizeof why, "unquoted '%c' not from the template", c);
            fail(why, cmd, tpl);
        }
        if (c == '$' && p[1] == '{') fail("parameter expansion ${...} in the command", cmd, tpl);
        if (c == '$' && p[1] == '(') {
            if (!allowed['(']) fail("command substitution not from the template", cmd, tpl);
            if (sp == (int)sizeof stack) fail("quoting nested too deep", cmd, tpl);
            stack[sp++] = '(';
            p++;
        }
    }
    if (sp) fail("unbalanced quotes or $(...)", cmd, tpl);
}

/* ------------------------------------------------------------------ */
/* One input                                                          */
/* ------------------------------------------------------------------ */

/* Does ctx hold every slot tpl requires (a repeated type means "need N")? */
static bool has_required(const ParseContext *ctx, const CommandTemplate *tpl) {
    for (int i = 0; i < NL_MAX_REQUIRED && tpl->required_slots[i] != SLOT_NONE; i++) {
        SlotType t = tpl->required_slots[i];
        int need = 0;
        for (int k = 0; k < NL_MAX_REQUIRED && tpl->required_slots[k] != SLOT_NONE; k++)
            if (tpl->required_slots[k] == t) need++;
        if (nl_count_slots(ctx, t) < need) return false;
    }
    return true;
}

static void expand_one(const ParseContext *ctx, const CommandTemplate *tpl) {
    static char cmd[NL_MAX_CMD_LEN], err[256];
    if (!nl_check(ctx, tpl)) return;
    if (nl_expand(ctx, tpl, cmd, sizeof cmd, err, sizeof err)) {
        check_command(cmd, tpl, ctx->tool);
        nl_explain(cmd, devnull);
    }
    const Danger *d = nl_danger(tpl);
    if (d && d->dry_run) {
        CommandTemplate preview = *tpl;
        preview.flag_template = d->dry_run;
        if (nl_expand(ctx, &preview, cmd, sizeof cmd, err, sizeof err))
            check_command(cmd, tpl, ctx->tool);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static ParseContext ctx;
    static MatchResult m;
    static char prompt[NL_MAX_INPUT], cmd[NL_MAX_CMD_LEN], err[256];

    if (!devnull) devnull = fopen("/dev/null", "w");
    if (size < 2) return 0;
    ToolId tool = pick_tool(data[0]);
    size_t n = size - 1;
    if (n > sizeof prompt - 1) n = sizeof prompt - 1;
    memcpy(prompt, data + 1, n);
    prompt[n] = '\0';                        /* an embedded NUL ends it early */

    memset(&ctx, 0, sizeof ctx);
    if (!nl_lex(&ctx, tool, prompt, NULL)) return 0;

    if (tool == TOOL_JQ && ctx.cond_paths > 1) {
        if (nl_jq_compound(prompt, NULL, false, true, cmd, sizeof cmd, err, sizeof err)) {
            check_command(cmd, NULL, tool);
            nl_explain(cmd, devnull);
        }
        return 0;
    }

    memset(&m, 0, sizeof m);
    nl_match(&ctx, &m, false);
    for (size_t i = 0; i < NL_TEMPLATE_COUNT; i++) {
        const CommandTemplate *t = &NL_TEMPLATES[i];
        if (t->tool == tool && (t == m.best || has_required(&ctx, t)))
            expand_one(&ctx, t);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Standalone driver (no libFuzzer)                                   */
/* ------------------------------------------------------------------ */
#ifdef NL_FUZZ_MAIN
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_SEEDS 4096
#define MAX_LEN   512

static uint8_t *seeds[MAX_SEEDS];
static size_t seed_len[MAX_SEEDS];
static size_t nseeds;

static void add_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f || nseeds >= MAX_SEEDS) { if (f) fclose(f); return; }
    uint8_t *buf = malloc(NL_MAX_INPUT + 1);
    size_t n = fread(buf, 1, NL_MAX_INPUT + 1, f);
    fclose(f);
    seeds[nseeds] = buf;
    seed_len[nseeds++] = n;
    LLVMFuzzerTestOneInput(buf, n);
}

static void add_path(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) { perror(path); exit(2); }
    if (!S_ISDIR(st.st_mode)) { add_file(path); return; }
    DIR *d = opendir(path);
    struct dirent *e;
    char full[4096];
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        add_file(full);
    }
    if (d) closedir(d);
}

static uint64_t rng;
static uint32_t rnd(uint32_t n) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return n ? (uint32_t)(rng % n) : 0;
}

/* What the mutator splices in: shell syntax, quoting, the lexer's own
 * vocabulary and things it parses (times, sizes, paths, URLs). */
static const char *const TOKENS[] = {
    ";", "|", "&", "&&", "$(", ")", "`", "$", "${", "}", "'", "\"", "\\", "\n",
    "*", "?", "[", "]", "<", ">", "!", "#", "~", "--", "-", " ", "  ", "\t", "\x1b[2J",
    "'a b'", "\"a'b\"", "'$(id)'", "x;y.mp4", "a|b.json", "$(rm -rf ~).png",
    " and ", " or ", " not ", " where ", " from ", " to ", " into ", " in ", " of ",
    " between ", " by ", " with ", " as ", " is ", " at ", " for ", " last ", " first ",
    " delete ", " remove ", " all ", " files ", " ignoring ", " excluding ",
    " larger than ", " older than ", " days ", " ago ", " commits ", " branch ",
    "in.mp4", "users.json", "src/", "~/x", "backup.tar.gz", "photo.jpg", "a.png",
    "https://example.com/x?a=1&b=2", "http://localhost:3000", ".users[].email", ".a.b",
    "00:10", "1:30", "15s", "2min", "720p", "1920x1080", "128k", "30fps", "2x", "100MB",
    "10", "-3", "2.5", "true", "null", "HEAD~2", "a1b2c3d", "main", "feature-x",
    "$TOKEN", "token $TOKEN", "#ff8800", "50%", "800x", "x600", "TODO", "ERROR",
};
#define NTOKENS (sizeof TOKENS / sizeof TOKENS[0])

static size_t mutate(uint8_t *buf, size_t len) {
    int ops = 1 + (int)rnd(4);
    while (ops--) {
        size_t pos = len > 1 ? 1 + rnd((uint32_t)len) : 1;  /* keep byte 0 */
        if (pos > len) pos = len;
        switch (rnd(6)) {
        case 0: case 1: {                                      /* insert a token */
            const char *t = TOKENS[rnd(NTOKENS)];
            size_t tl = strlen(t);
            if (len + tl > MAX_LEN) break;
            memmove(buf + pos + tl, buf + pos, len - pos);
            memcpy(buf + pos, t, tl);
            len += tl;
            break;
        }
        case 2:                                                /* delete a run */
            if (pos < len) {
                size_t k = 1 + rnd((uint32_t)(len - pos < 8 ? len - pos : 8));
                memmove(buf + pos, buf + pos + k, len - pos - k);
                len -= k;
            }
            break;
        case 3:                                                /* random byte */
            if (pos < len) buf[pos] = (uint8_t)rnd(256);
            break;
        case 4: {                                              /* splice a seed */
            size_t s = rnd((uint32_t)nseeds);
            if (seed_len[s] < 2) break;
            size_t from = 1 + rnd((uint32_t)(seed_len[s] - 1));
            size_t k = 1 + rnd((uint32_t)(seed_len[s] - from));
            if (len + k > MAX_LEN) break;
            memmove(buf + pos + k, buf + pos, len - pos);
            memcpy(buf + pos, seeds[s] + from, k);
            len += k;
            break;
        }
        default:                                               /* another tool */
            buf[0] = (uint8_t)rnd(256);
            break;
        }
    }
    return len;
}

/* The input being run, saved to crash-input if it brings the process down
 * (the oracle aborts; with ASAN_OPTIONS=abort_on_error=1 so do the
 * sanitizers). Replay it with: build/fuzz-check crash-input */
static const uint8_t *cur;
static size_t cur_len;

static void on_crash(int sig) {
    if (cur) {
        int fd = open("crash-input", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ssize_t w = write(fd, cur, cur_len);
            (void)w;
            close(fd);
        }
        static const char msg[] = "nls fuzz: input saved to crash-input\n";
        ssize_t w = write(2, msg, sizeof msg - 1);
        (void)w;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

int main(int argc, char **argv) {
    unsigned long runs = 0;
    signal(SIGABRT, on_crash);
    signal(SIGSEGV, on_crash);
    signal(SIGBUS, on_crash);
    rng = (uint64_t)time(NULL) | 1;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-runs=", 6) == 0) runs = strtoul(argv[i] + 6, NULL, 10);
        else if (strncmp(argv[i], "-seed=", 6) == 0) rng = strtoull(argv[i] + 6, NULL, 10) * 2 + 1;
        else if (argv[i][0] == '-') continue;    /* libFuzzer flags: ignored */
        else add_path(argv[i]);
    }
    fprintf(stderr, "nls fuzz: replayed %zu inputs\n", nseeds);
    if (!runs || !nseeds) return 0;

    fprintf(stderr, "nls fuzz: %lu mutations (seed %llu)\n", runs, (unsigned long long)rng);
    static uint8_t buf[MAX_LEN + 1];
    for (unsigned long r = 0; r < runs; r++) {
        size_t s = rnd((uint32_t)nseeds);
        size_t len = seed_len[s] > MAX_LEN ? MAX_LEN : seed_len[s];
        memcpy(buf, seeds[s], len);
        len = mutate(buf, len);
        cur = buf;
        cur_len = len;
        LLVMFuzzerTestOneInput(buf, len);
    }
    cur = NULL;
    fprintf(stderr, "nls fuzz: no failures\n");
    return 0;
}
#endif
