/*
 * main.c — CLI parsing and dispatch.
 *
 *   nls [options] <tool> <prompt...>
 *
 * Prints the compiled command to stdout. With -x it shows the command on
 * stderr, asks for confirmation (skip with -y) and runs it via system().
 */
#include "nl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *f) {
    fprintf(f,
        "usage: nls [options] <tool> \"<what you want>\"\n"
        "\n"
        "Compile a natural-language request into a shell command (offline).\n"
        "Supported tools: ffmpeg, jq, grep, find, tar, git, curl, magick\n"
        "\n"
        "options:\n"
        "  -x, --exec       run the command (asks for confirmation)\n"
        "  -y, --yes        with -x: don't ask for confirmation\n"
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
        printf("%-20s %s\n", t->id, t->description);
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

static bool confirm(void) {
    FILE *tty = fopen("/dev/tty", "r");
    FILE *in = tty ? tty : stdin;
    char line[64];
    fprintf(stderr, "[Enter to run / Ctrl+C to abort] ");
    fflush(stderr);
    bool ok = fgets(line, sizeof line, in) != NULL && line[0] != 'n' && line[0] != 'N';
    if (tty) fclose(tty);
    return ok;
}

static int run_command(const char *cmd) {
    fflush(stdout);
    int rc = system(cmd);
    if (rc == -1) { perror("nls: system"); return 127; }
#ifdef _WIN32
    return rc;
#else
    /* POSIX wait status without <sys/wait.h>: exit code in bits 8..15,
     * terminating signal in the low 7 bits. */
    if ((rc & 0x7f) != 0) return 128 + (rc & 0x7f);
    return (rc >> 8) & 0xff;
#endif
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
            fprintf(stderr, "  %-22s %s\n  %-22s   %s\n", alts[k]->id, alts[k]->description, "",
                    alt_cmd[k]);
        fprintf(stderr, "nls: rephrase to choose (see 'nls -v' for scores)\n");
    }
    return true;
}

/* Print the command, or show it and run it (-x). With -y the prompt is
 * skipped, except for an uncertain match. */
static int finish(const char *cmd, bool exec, bool yes, bool uncertain) {
    if (!exec) {
        printf("%s\n", cmd);
        return 0;
    }
    fprintf(stderr, "\xe2\x86\x92 %s\n", cmd);
    if (yes && uncertain)
        fprintf(stderr, "nls: not running an uncertain match without confirmation (-y ignored)\n");
    if ((!yes || uncertain) && !confirm()) {
        fprintf(stderr, "nls: aborted\n");
        return 130;
    }
    return run_command(cmd);
}

int main(int argc, char **argv) {
    bool exec = false, yes = false, verbose = false;
    int i = 1;

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) { i++; break; }
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "-x") == 0 || strcmp(a, "--exec") == 0) exec = true;
        else if (strcmp(a, "-y") == 0 || strcmp(a, "--yes") == 0) yes = true;
        else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) verbose = true;
        else if (strcmp(a, "-xy") == 0 || strcmp(a, "-yx") == 0) exec = yes = true;
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
        return finish(cmd, exec, yes, false);
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

    bool uncertain = warn_uncertain(&ctx, &m, cmd);
    return finish(cmd, exec, yes, uncertain);
}
