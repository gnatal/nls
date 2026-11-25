/*
 * jq_compound.c — jq: several conditions joined by "and" / "or".
 *
 *   nls jq "users.json where .age > 30 and .status is active"
 *   -> jq --arg v2 active 'map(select((.age > 30) and (.status == $v2)))' users.json
 *
 * The prompt is split at top-level conjunctions ("and", "or", "but"; never
 * inside quotes) into clauses holding one condition each. A piece with no
 * condition stays with its neighbour ("get .name and .email from ...",
 * "between 10 and 20"). Every clause is lexed and matched on its own,
 * against the single-condition jq_filter_* templates only. The select()
 * bodies of the winning templates are renumbered ({JSON_PATH:0} ->
 * {JSON_PATH:k}, $v -> $vk, ...) and joined into one synthetic template,
 * which the normal expander fills from a context holding every clause's
 * slots. Quoting and value checks are therefore the same as for a single
 * condition.
 */
#include "nl.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define MAX_CLAUSES 8

typedef struct {
    size_t start, end;   /* byte range in the prompt */
    char conj;           /* '&' or '|' joining it to the previous clause; 0 first */
} Span;

/* Is the word at s[0..n) a conjunction? Returns '&', '|' or 0. */
static char conj_of(const char *s, size_t n) {
    char w[8];
    while (n > 0 && strchr(",;", s[n - 1])) n--;
    if (n == 0 || n >= sizeof w) return 0;
    for (size_t i = 0; i < n; i++) w[i] = (char)tolower((unsigned char)s[i]);
    w[n] = '\0';
    if (strcmp(w, "and") == 0 || strcmp(w, "but") == 0) return '&';
    if (strcmp(w, "or") == 0) return '|';
    return 0;
}

/* Split `prompt` at unquoted conjunction words into pieces. */
static size_t split_pieces(const char *prompt, Span *pieces, size_t max) {
    size_t n = 0, piece_start = 0, i = 0, len = strlen(prompt);
    char pending = 0;
    while (i < len) {
        while (i < len && isspace((unsigned char)prompt[i])) i++;
        if (i >= len) break;
        size_t ws = i;
        if (prompt[i] == '\'' || prompt[i] == '"') {
            char q = prompt[i++];
            while (i < len && prompt[i] != q) i++;
            if (i < len) i++;
            continue;
        }
        while (i < len && !isspace((unsigned char)prompt[i])) i++;
        char c = conj_of(prompt + ws, i - ws);
        if (c && n + 1 < max) {
            pieces[n++] = (Span){ piece_start, ws, pending };
            pending = c;
            piece_start = i;
        }
    }
    pieces[n++] = (Span){ piece_start, len, pending };
    return n;
}

/* Does clause `s` hold an unquoted "either" ('|') or "both" ('&')? */
static char group_word(const char *prompt, Span s) {
    size_t i = s.start;
    while (i < s.end) {
        while (i < s.end && isspace((unsigned char)prompt[i])) i++;
        if (i < s.end && (prompt[i] == '\'' || prompt[i] == '"')) {
            char q = prompt[i++];
            while (i < s.end && prompt[i] != q) i++;
            if (i < s.end) i++;
            continue;
        }
        size_t ws = i;
        while (i < s.end && !isspace((unsigned char)prompt[i])) i++;
        if (i - ws == 6 && strncasecmp(prompt + ws, "either", 6) == 0) return '|';
        if (i - ws == 4 && strncasecmp(prompt + ws, "both", 4) == 0) return '&';
    }
    return 0;
}

static bool lex_span(ParseContext *ctx, const char *prompt, Span s, const JsonInfo *json) {
    char buf[NL_MAX_INPUT];
    size_t n = s.end - s.start;
    if (n >= sizeof buf) return false;
    memcpy(buf, prompt + s.start, n);
    buf[n] = '\0';
    if (!nl_lex(ctx, TOOL_JQ, buf, json)) return false;
    if (json) ctx->input_shape = json->shape;
    return true;
}

/* Copy the condition inside "select(...)" of a filter template to `o`,
 * renumbering its placeholders for clause k: {NUM:j} becomes
 * {NUM:num_idx+j}, where num_idx counts the numbers earlier clauses used. */
static bool append_condition(char *o, size_t size, const char *tpl, int k, int num_idx) {
    const char *p = strstr(tpl, "select(");
    if (!p) return false;
    p += 7;
    int depth = 1;
    const char *end = p;
    while (*end && depth > 0) {
        if (*end == '(') depth++;
        else if (*end == ')' && --depth == 0) break;
        end++;
    }
    if (depth != 0) return false;

    size_t len = strlen(o);
    while (p < end) {
        char piece[32];
        size_t skip = 1;
        if (strncmp(p, "{JSON_PATH:0}", 13) == 0) {
            snprintf(piece, sizeof piece, "{JSON_PATH:%d}", k);
            skip = 13;
        } else if (strncmp(p, "{NUM:", 5) == 0 && isdigit((unsigned char)p[5]) && p[6] == '}') {
            snprintf(piece, sizeof piece, "{NUM:%d}", num_idx + (p[5] - '0'));
            skip = 7;
        } else if (p[0] == '$' && p[1] == 'v' && !isalnum((unsigned char)p[2]) && p[2] != '_') {
            snprintf(piece, sizeof piece, "$v%d", k + 1);
            skip = 2;
        } else if (p[0] == '{' && isupper((unsigned char)p[1])) {
            return false;   /* a placeholder this module doesn't know how to renumber */
        } else {
            piece[0] = *p;
            piece[1] = '\0';
        }
        size_t pl = strlen(piece);
        if (len + pl + 1 > size) return false;
        memcpy(o + len, piece, pl + 1);
        len += pl;
        p += skip;
    }
    return true;
}

static bool append(char *o, size_t size, const char *s) {
    size_t len = strlen(o), n = strlen(s);
    if (len + n + 1 > size) return false;
    memcpy(o + len, s, n + 1);
    return true;
}

static void push_slot(ParseContext *ctx, SlotType type, const char *value) {
    if (ctx->slot_count >= NL_MAX_SLOTS) { ctx->overflow = true; return; }
    ctx->slots[ctx->slot_count].type = type;
    ctx->slots[ctx->slot_count].value = value;
    ctx->slot_count++;
}

bool nl_jq_compound(const char *prompt, const JsonInfo *json, bool verbose, bool quiet,
                    char *out, size_t out_size, char *err, size_t err_size) {
    static ParseContext pieces_ctx;
    static ParseContext clause_ctx[MAX_CLAUSES];
    static ParseContext synth;
    Span pieces[MAX_CLAUSES * 4];
    Span clauses[MAX_CLAUSES];
    size_t nclauses = 0;
    bool clause_has_cond = false;

    if (err_size) err[0] = '\0';

    /* 1. Group the pieces into clauses with one condition each. */
    size_t npieces = split_pieces(prompt, pieces, sizeof pieces / sizeof pieces[0]);
    if (npieces < 2) return false;   /* no conjunction: the caller explains */
    for (size_t i = 0; i < npieces; i++) {
        if (!lex_span(&pieces_ctx, prompt, pieces[i], json)) return false;
        uint8_t conds = pieces_ctx.cond_paths;
        if (conds > 1) return false;
        if (nclauses == 0 || (conds == 1 && clause_has_cond)) {
            if (nclauses == MAX_CLAUSES) {
                snprintf(err, err_size, "too many conditions (at most %d)", MAX_CLAUSES);
                return false;
            }
            clauses[nclauses++] = pieces[i];
            clause_has_cond = false;
        } else {
            clauses[nclauses - 1].end = pieces[i].end;   /* keep "and" inside the clause */
        }
        if (conds == 1) clause_has_cond = true;
    }
    if (nclauses < 2) return false;

    /* Grouping. "either a or b" / "both a and b" group explicitly: the run
     * of clauses joined by that conjunction becomes one unit. Between units,
     * "and" binds tighter than "or", as in jq: "a or b and c" is
     * a or (b and c), "either a or b and c" is (a or b) and c. */
    int opens[MAX_CLAUSES] = { 0 }, closes[MAX_CLAUSES] = { 0 };
    bool mixed = false, explicit_group = false;
    {
        size_t ustart[MAX_CLAUSES], uend[MAX_CLAUSES], nunits = 0;
        for (size_t k = 0; k < nclauses; ) {
            char g = group_word(prompt, clauses[k]);
            size_t j = k;
            if (g) while (j + 1 < nclauses && clauses[j + 1].conj == g) j++;
            if (j > k && (k > 0 || j + 1 < nclauses)) {
                opens[k]++;
                closes[j]++;
                explicit_group = true;
            }
            ustart[nunits] = k;
            uend[nunits++] = j;
            k = j + 1;
        }
        bool has_and = false, has_or = false;
        for (size_t u = 1; u < nunits; u++) {
            if (clauses[ustart[u]].conj == '|') has_or = true;
            else has_and = true;
        }
        mixed = has_and && has_or;
        for (size_t u = 0; mixed && u < nunits; ) {
            size_t v = u;
            while (v + 1 < nunits && clauses[ustart[v + 1]].conj == '&') v++;
            if (v > u) { opens[ustart[u]]++; closes[uend[v]]++; }
            u = v + 1;
        }
    }

    /* 2. Match each clause against the one-condition filters. */
    const CommandTemplate *tpls[MAX_CLAUSES];
    for (size_t k = 0; k < nclauses; k++) {
        ParseContext *c = &clause_ctx[k];
        int n = (int)(clauses[k].end - clauses[k].start);
        if (!lex_span(c, prompt, clauses[k], json) || c->cond_paths != 1) {
            snprintf(err, err_size, "couldn't find one condition in \"%.*s\"", n,
                     prompt + clauses[k].start);
            return false;
        }
        if (verbose) fprintf(stderr, "condition %zu: %.*s\n", k + 1, n, prompt + clauses[k].start);
        MatchResult m;
        nl_match_only(c, &m, verbose, "jq_filter_");
        if (!m.best && m.wrong_shape) {
            snprintf(err, err_size, "filtering needs a JSON array, but the input is an object");
            return false;
        }
        if (!m.best || (m.blocked && m.blocked_score > m.best_score)) {
            snprintf(err, err_size, "couldn't understand the condition \"%.*s\" "
                     "(e.g. .age > 30, .status is 'active', .email exists)", n,
                     prompt + clauses[k].start);
            return false;
        }
        if (verbose) fprintf(stderr, "condition %zu: %s\n", k + 1, m.best->id);
        tpls[k] = m.best;
    }

    /* 3. Build the synthetic template and the context that fills it. */
    memset(&synth, 0, sizeof synth);
    synth.tool = TOOL_JQ;
    synth.input_shape = json ? json->shape : 0;
    const char *file = NULL, *proj = NULL;
    for (size_t k = 0; k < nclauses; k++) {
        const ParseContext *c = &clause_ctx[k];
        if (c->wants_count) synth.wants_count = true;
        const char *f = nl_find_slot(c, SLOT_FILE, 0);
        if (f && !file) file = f;
        const char *p = nl_find_slot(c, SLOT_JSON_PATH, 1);
        if (p && proj && strcmp(p, proj) != 0) {
            snprintf(err, err_size, "not sure what to output: both %s and %s", proj, p);
            return false;
        }
        if (p) proj = p;
    }

    char program[NL_MAX_CMD_LEN] = "jq";
    char cond[NL_MAX_CMD_LEN] = "";
    int num_idx = 0;
    for (size_t k = 0; k < nclauses; k++) {
        const ParseContext *c = &clause_ctx[k];
        const char *t = tpls[k]->flag_template;
        push_slot(&synth, SLOT_JSON_PATH, nl_find_slot(c, SLOT_JSON_PATH, 0));
        if (strstr(t, "{QUOTED_STR:0}")) {
            char arg[48];
            snprintf(arg, sizeof arg, " --arg v%zu {QUOTED_STR:%d}", k + 1,
                     nl_count_slots(&synth, SLOT_QUOTED_STR));
            push_slot(&synth, SLOT_QUOTED_STR, nl_find_slot(c, SLOT_QUOTED_STR, 0));
            if (!append(program, sizeof program, arg)) return false;
        }
        int nums = 0;
        while (nums < 10) {
            char ph[16];
            snprintf(ph, sizeof ph, "{NUM:%d}", nums);
            if (!strstr(t, ph)) break;
            nums++;
        }
        const char *lead = k == 0 ? "" : clauses[k].conj == '&' ? " and " : " or ";
        bool ok = append(cond, sizeof cond, lead);
        for (int p = 0; ok && p < opens[k]; p++) ok = append(cond, sizeof cond, "(");
        ok = ok && append(cond, sizeof cond, "(") &&
             append_condition(cond, sizeof cond, t, (int)k, num_idx) &&
             append(cond, sizeof cond, ")");
        for (int p = 0; ok && p < closes[k]; p++) ok = append(cond, sizeof cond, ")");
        if (!ok) {
            snprintf(err, err_size, "can't combine %s with other conditions", tpls[k]->id);
            return false;
        }
        for (int j = 0; j < nums; j++)
            push_slot(&synth, SLOT_NUM, nl_find_slot(c, SLOT_NUM, j));
        num_idx += nums;
    }
    if ((mixed || explicit_group) && !quiet) {
        /* Say how it was read: the other grouping is a plausible intent. */
        fprintf(stderr, mixed ? "nls: reading \"and\" before \"or\": " : "nls: grouping as: ");
        for (size_t k = 0; k < nclauses; k++) {
            size_t a = clauses[k].start, b = clauses[k].end;
            if (k == 0) {
                /* drop the lead-in: "users.json where .a is x" -> ".a is x" */
                for (size_t i = a; i < b; i++) {
                    if (i > a && !isspace((unsigned char)prompt[i - 1])) continue;
                    size_t e = i;
                    while (e < b && !isspace((unsigned char)prompt[e])) e++;
                    if ((e - i == 5 && (strncasecmp(prompt + i, "where", 5) == 0 ||
                                        strncasecmp(prompt + i, "whose", 5) == 0)) ||
                        (e - i == 2 && strncasecmp(prompt + i, "if", 2) == 0)) {
                        a = e;
                        break;
                    }
                }
            }
            while (a < b && isspace((unsigned char)prompt[a])) a++;
            while (b > a && isspace((unsigned char)prompt[b - 1])) b--;
            fprintf(stderr, "%s%.*s%.*s%.*s", k == 0 ? "" : clauses[k].conj == '&' ? " and " : " or ",
                    opens[k], "((((((((", (int)(b - a), prompt + a, closes[k], "))))))))");
        }
        fprintf(stderr, "\n");
    }
    if (proj) push_slot(&synth, SLOT_JSON_PATH, proj);
    if (file) push_slot(&synth, SLOT_FILE, file);
    if (synth.overflow) { snprintf(err, err_size, "too many conditions"); return false; }

    char proj_ph[32] = "";
    if (proj) snprintf(proj_ph, sizeof proj_ph, " | {JSON_PATH:%zu}", nclauses);
    if (!append(program, sizeof program, " 'map(select(") ||
        !append(program, sizeof program, cond) ||
        !append(program, sizeof program, ")") ||
        !append(program, sizeof program, proj_ph) ||
        !append(program, sizeof program, "){COUNT}' {FILE:0|}")) {
        snprintf(err, err_size, "command too long");
        return false;
    }

    CommandTemplate tpl = { TOOL_JQ, "jq_filter_combined", "Keep items matching several conditions",
                            "users.json where .age > 30 and .status is 'active'",
                            { NULL }, { SLOT_NONE }, program, SHAPE_ARRAY };
    if (verbose) fprintf(stderr, "combined: %s\n", program);
    return nl_expand(&synth, &tpl, out, out_size, err, err_size);
}
