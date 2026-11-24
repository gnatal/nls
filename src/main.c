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
    if (tool == TOOL_JQ) {
        const char *file = nl_find_slot(&ctx, SLOT_FILE, 0);
        const char *ext = file ? strrchr(file, '.') : NULL;
        bool lines = ext && (strcmp(ext, ".jsonl") == 0 || strcmp(ext, ".ndjson") == 0);
        static JsonInfo json;
        if (file && !lines && nl_inspect_json(file, &json)) {
            nl_lex(&ctx, tool, prompt, &json);
            ctx.input_shape = json.shape;
        }
    }
    if (verbose) {
        print_context(&ctx);
        if (ctx.input_shape)
            fprintf(stderr, "input: %s\n", ctx.input_shape == '[' ? "array" : "object");
    }

    if (tool == TOOL_JQ && ctx.cond_paths > 1) {
        fprintf(stderr, "nls: combining several conditions (e.g. \"where .a is x and .b > 3\") "
                        "isn't supported yet; use one condition per request\n");
        return 1;
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

    char cmd[NL_MAX_CMD_LEN];
    char err[256];
    if (!nl_expand(&ctx, m.best, cmd, sizeof cmd, err, sizeof err)) {
        fprintf(stderr, "nls: %s (%s)\n", err, m.best->id);
        return 1;
    }

    if (!exec) {
        printf("%s\n", cmd);
        return 0;
    }

    fprintf(stderr, "\xe2\x86\x92 %s\n", cmd);
    if (!yes && !confirm()) {
        fprintf(stderr, "nls: aborted\n");
        return 130;
    }
    return run_command(cmd);
}
