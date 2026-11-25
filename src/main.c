/*
 * main.c — CLI parsing and dispatch.
 *
 *   nls [options] <tool> <prompt...>
 *
 * Prints the compiled command to stdout. With -x it shows the command on
 * stderr, asks for confirmation (skip with -y) and runs it via system().
 * A destructive command (see nl_danger()) is explained, its dry run runs
 * first, and only a typed "y" runs it; -y skips that only with --force.
 */
#define _POSIX_C_SOURCE 200809L   /* popen() for dry runs */
#include "nl.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

/* Lines of dry-run output shown before the rest is only counted. */
#define NL_DRY_RUN_LINES 20

/* -x, -y and --force */
typedef struct {
    bool exec, yes, force;
} RunMode;

static void usage(FILE *f) {
    fprintf(f,
        "usage: nls [options] <tool> \"<what you want>\"\n"
        "\n"
        "Compile a natural-language request into a shell command (offline).\n"
        "Supported tools: ffmpeg, jq, grep, find, tar, git, curl, magick\n"
        "\n"
        "options:\n"
        "  -x, --exec       run the command (asks for confirmation)\n"
        "  -y, --yes        with -x: don't ask, unless the match is uncertain\n"
        "                   or the command is destructive\n"
        "      --force      with -x -y: don't ask for destructive commands either\n"
        "  -v, --verbose    explain slots, tokens and template scores on stderr\n"
        "  -l, --list [tool] list available templates\n"
        "  -h, --help       show this help\n"
        "  -V, --version    show version\n"
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

/* Warn on stderr when the pick was weak or a close call. Returns true if
 * the user should look before the command runs. */
static bool warn_uncertain(const ParseContext *ctx, const MatchResult *m, const char *cmd) {
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

/* Print the command, or show it and run it (-x). Running asks first unless
 * -y, and -y doesn't skip that for an uncertain match, nor for a
 * destructive command without --force. A destructive command is explained
 * and its dry run (`dry`, "" if none) runs before the question; only a
 * typed "y" runs it, and a failed dry run stops it. */
static int finish(const char *cmd, const Danger *danger, const char *dry, RunMode mode,
                  bool uncertain) {
    if (!mode.exec) {
        printf("%s\n", cmd);
        return 0;
    }
    fprintf(stderr, "\xe2\x86\x92 %s\n", cmd);
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

int main(int argc, char **argv) {
    RunMode mode = { false, false, false };
    bool verbose = false;
    int i = 1;

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) { i++; break; }
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "-x") == 0 || strcmp(a, "--exec") == 0) mode.exec = true;
        else if (strcmp(a, "-y") == 0 || strcmp(a, "--yes") == 0) mode.yes = true;
        else if (strcmp(a, "--force") == 0) mode.force = true;
        else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) verbose = true;
        else if (strcmp(a, "-xy") == 0 || strcmp(a, "-yx") == 0) mode.exec = mode.yes = true;
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(stdout); return 0; }
        else if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) { printf("nls %s\n", NL_VERSION); return 0; }
        else if (strcmp(a, "-l") == 0 || strcmp(a, "--list") == 0) {
            ToolId only = TOOL_UNKNOWN;
            if (i + 1 < argc) {
                only = nl_tool_from_name(argv[i + 1]);
                if (only == TOOL_UNKNOWN) {
                    fprintf(stderr, "nls: unknown tool '%s' (supported: ffmpeg, jq, grep, find, tar, git, curl, magick)\n", argv[i + 1]);
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

    if (argc - i < 2) { usage(stderr); return 2; }

    ToolId tool = nl_tool_from_name(argv[i]);
    if (tool == TOOL_UNKNOWN) {
        fprintf(stderr, "nls: unsupported tool '%s' (supported in this version: ffmpeg, jq, grep, find, tar, git, curl, magick)\n", argv[i]);
        return 2;
    }

    char prompt[NL_MAX_INPUT];
    if (!join_prompt(argc, argv, i + 1, prompt, sizeof prompt)) {
        fprintf(stderr, "nls: prompt too long (max %d bytes)\n", NL_MAX_INPUT - 1);
        return 2;
    }

    static ParseContext ctx;
    if (!nl_lex(&ctx, tool, prompt, NULL)) {
        fprintf(stderr, "nls: prompt too long or has too many words\n");
        return 2;
    }
    /* jq: if the input file is readable, learn its shape and field names,
     * then lex again so bare words like "emails" can resolve to .email */
    static JsonInfo json;
    bool have_json = false;
    if (tool == TOOL_JQ) {
        const char *file = nl_find_slot(&ctx, SLOT_FILE, 0);
        const char *ext = file ? strrchr(file, '.') : NULL;
        bool lines = ext && (strcmp(ext, ".jsonl") == 0 || strcmp(ext, ".ndjson") == 0);
        if (file && !lines && nl_inspect_json(file, &json)) {
            nl_lex(&ctx, tool, prompt, &json);
            ctx.input_shape = json.shape;
            have_json = true;
        }
    }
    if (verbose) {
        print_context(&ctx);
        if (ctx.input_shape)
            fprintf(stderr, "input: %s\n", ctx.input_shape == '[' ? "array" : "object");
    }

    char cmd[NL_MAX_CMD_LEN];
    char err[256];

    /* grep: "search for TODO in src" with no src here */
    if (tool == TOOL_GREP && ctx.missing_path) {
        fprintf(stderr, "nls: no file or folder named '%s' here.\n"
                        "Give the path as it is on disk (e.g. 'src/' or './%s').\n",
                ctx.missing_path, ctx.missing_path);
        return 1;
    }

    /* jq: "where .age around 30": refuse rather than guess the comparison */
    if (tool == TOOL_JQ && ctx.jq_bad_op) {
        fprintf(stderr, "nls: don't know how to compare %s \"%s\" ...\n"
                        "Try: is, is not, >, <, at least, at most, between X and Y, "
                        "outside X and Y, contains, matches, exists\n",
                ctx.jq_bad_path, ctx.jq_bad_op);
        return 1;
    }

    /* jq: "where .a is x and .b > 3" */
    if (tool == TOOL_JQ && ctx.cond_paths > 1) {
        if (!nl_jq_compound(prompt, have_json ? &json : NULL, verbose, cmd, sizeof cmd,
                            err, sizeof err)) {
            if (err[0]) fprintf(stderr, "nls: %s\n", err);
            else fprintf(stderr, "nls: found several conditions; join them with \"and\" or \"or\" "
                                 "(e.g. \"where .a is x and .b > 3\")\n");
            return 1;
        }
        return finish(cmd, NULL, "", mode, false);
    }

    if (verbose && ctx.flags[0]) fprintf(stderr, "modifiers: %s\n", ctx.flags);

    MatchResult m;
    nl_match(&ctx, &m, verbose);

    if (!m.best || (m.blocked && m.blocked_score > m.best_score)) {
        if (m.wrong_shape && m.wrong_shape_score >= m.blocked_score) {
            fprintf(stderr, "nls: \"%s\" needs a JSON %s, but %s is an %s.\n",
                    m.wrong_shape->description,
                    m.wrong_shape->shape == SHAPE_ARRAY ? "array" : "object",
                    nl_find_slot(&ctx, SLOT_FILE, 0),
                    ctx.input_shape == '[' ? "array" : "object");
            fprintf(stderr, "Run 'nls jq \"keys of %s\"' to see what it contains.\n",
                    nl_find_slot(&ctx, SLOT_FILE, 0));
        } else if (m.blocked) {
            fprintf(stderr, "nls: this looks like \"%s\" (%s), but it needs:\n",
                    m.blocked->description, m.blocked->id);
            for (uint8_t k = 0; k < m.missing_count; k++)
                fprintf(stderr, "  - %s\n", nl_slot_hint(m.missing[k]));
            fprintf(stderr, "example: nls %s \"%s\"\n", nl_tool_name(tool), m.blocked->example);
        } else {
            fprintf(stderr, "nls: couldn't understand that request for %s.\n"
                            "Run 'nls --list %s' to see what's supported.\n",
                    nl_tool_name(tool), nl_tool_name(tool));
        }
        return 1;
    }

    if (verbose) fprintf(stderr, "match: %s (score %d)\n", m.best->id, m.best_score);

    /* jq: a filter with several paths and none of them clearly the
     * condition: refuse rather than guess which one to test. */
    if (tool == TOOL_JQ && strncmp(m.best->id, "jq_filter_", 10) == 0 &&
        ctx.cond_paths == 0 && nl_count_slots(&ctx, SLOT_JSON_PATH) > 1) {
        fprintf(stderr, "nls: not sure which of %s and %s is the condition.\n"
                        "Put the condition after \"where\" (e.g. \"get .name where .age > 30\").\n",
                nl_find_slot(&ctx, SLOT_JSON_PATH, 0), nl_find_slot(&ctx, SLOT_JSON_PATH, 1));
        return 1;
    }

    if (!nl_expand(&ctx, m.best, cmd, sizeof cmd, err, sizeof err)) {
        fprintf(stderr, "nls: %s (%s)\n", err, m.best->id);
        return 1;
    }

    /* A destructive command's dry run comes from the same request: the
     * template with its preview text in place of the command. */
    const Danger *danger = nl_danger(m.best);
    char dry[NL_MAX_CMD_LEN] = "";
    if (danger && danger->dry_run) {
        CommandTemplate preview = *m.best;
        preview.flag_template = danger->dry_run;
        if (!nl_expand(&ctx, &preview, dry, sizeof dry, err, sizeof err)) {
            if (verbose) fprintf(stderr, "dry run: can't build it: %s\n", err);
            dry[0] = '\0';
        }
    }
    if (verbose && danger) {
        fprintf(stderr, "destructive: %s\n", danger->what);
        if (dry[0]) fprintf(stderr, "dry run: %s\n", dry);
    }

    bool uncertain = warn_uncertain(&ctx, &m, cmd);
    return finish(cmd, danger, dry, mode, uncertain);
}
