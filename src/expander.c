/*
 * expander.c — template substitution.
 *
 * Template grammar:
 *   {TYPE:i}             i-th slot of TYPE (TYPE = FILE, TIME, RES, ...)
 *   {TYPE:i|default}     same, with a literal fallback (may be empty)
 *   {OUT_FILE}           output path derived from the input file
 *   {OUT_FILE|ext}       ... defaulting to extension `ext` when the user gave
 *                        no target format; "!ext" forces it
 *   {OUT_FILE|ext|sfx}   ... and inserts suffix `sfx` before the extension
 *   {COUNT}              " | length" if the prompt asked for a count (jq)
 *   {FLAGS|base|allowed} "-<base letters + modifiers>" (grep), see
 *                        build_flags(); empty (and eats one space) if none
 *   << ... >>            optional group: emitted only if every placeholder
 *                        inside resolved to a user-supplied value
 * find only (each expands to "" or to text with a leading space):
 *   {KIND}               " -type f|d|l" (nothing for "files and directories")
 *   {NAMES}              " -name X" / " \( -name X -o -name Y \)" from EXT and
 *                        QUOTED_STR slots; -iname when ignoring case
 *   {DEPTH}              " -maxdepth N"
 *   {PRUNE}              " -path '*\/X' -prune -o" per EXCLUDE slot
 *   {PRINT}              " -print" when {PRUNE} emitted anything or a
 *                        line-count test (an -exec) is present
 * For find, every slot the user gave must be used by the template: a
 * dropped "older than 30 days" would make a delete far broader.
 *
 * FILE, QUOTED_STR and OUT_FILE values are POSIX shell-quoted. All other
 * slot values are produced by the lexer from a restricted alphabet and are
 * re-validated here before being spliced in raw (they may appear inside
 * single-quoted program text such as a jq filter).
 */
#include "nl.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t size;
    size_t len;
    bool overflow;
} Out;

static void out_putn(Out *o, const char *s, size_t n) {
    if (o->len + n + 1 > o->size) { o->overflow = true; return; }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

static void out_puts(Out *o, const char *s) { out_putn(o, s, strlen(s)); }

static bool is_shell_safe_char(unsigned char c) {
    return isalnum(c) || strchr("_-./:@%+=,", c) != NULL;
}

bool nl_shell_quote(const char *in, char *out, size_t out_size) {
    Out o = { out, out_size, 0, false };
    if (out_size) out[0] = '\0';

    /* Keep a leading "~/" (or a bare "~") unquoted so the shell still expands it. */
    if (in[0] == '~' && in[1] == '\0') { out_putn(&o, in, 1); return !o.overflow; }
    if (in[0] == '~' && in[1] == '/') { out_putn(&o, in, 2); in += 2; }

    bool safe = in[0] != '\0';
    for (const char *p = in; *p; p++)
        if (!is_shell_safe_char((unsigned char)*p)) { safe = false; break; }
    if (safe || (in[0] == '\0' && o.len > 0)) {
        out_puts(&o, in);
        return !o.overflow;
    }

    out_putn(&o, "'", 1);
    for (const char *p = in; *p; p++) {
        if (*p == '\'') out_puts(&o, "'\\''");
        else out_putn(&o, p, 1);
    }
    out_putn(&o, "'", 1);
    return !o.overflow;
}

static bool needs_quoting(SlotType t) {
    return t == SLOT_FILE || t == SLOT_QUOTED_STR;
}

/* find SIZE: [+-]digits[ckMG] */
static bool find_size_ok(const char *v) {
    if (*v == '+' || *v == '-') v++;
    if (!isdigit((unsigned char)*v)) return false;
    while (isdigit((unsigned char)*v)) v++;
    return v[0] && strchr("ckMG", v[0]) && v[1] == '\0';
}

/* find TIME: -[am](time|min) [+-]digits */
static bool find_time_ok(const char *v) {
    if (v[0] != '-' || (v[1] != 'a' && v[1] != 'm')) return false;
    v += 2;
    if (strncmp(v, "time ", 5) == 0 || strncmp(v, "min ", 4) == 0) v = strchr(v, ' ') + 1;
    else return false;
    if (*v != '+' && *v != '-') return false;
    v++;
    if (!isdigit((unsigned char)*v)) return false;
    while (isdigit((unsigned char)*v)) v++;
    return *v == '\0';
}

/* find LINES: (<|>|<=|>=|==|!=) digits */
static bool find_lines_ok(const char *v) {
    static const char *const ops[] = { "<= ", ">= ", "== ", "!= ", "< ", "> " };
    size_t k = 0;
    while (k < sizeof ops / sizeof ops[0] && strncmp(v, ops[k], strlen(ops[k])) != 0) k++;
    if (k == sizeof ops / sizeof ops[0]) return false;
    v += strlen(ops[k]);
    if (!isdigit((unsigned char)*v)) return false;
    while (isdigit((unsigned char)*v)) v++;
    return *v == '\0';
}

/* Raw-spliced values must stay inside this alphabet. */
static bool raw_value_ok(SlotType type, const char *v) {
    if (type == SLOT_SIZE) return find_size_ok(v);
    if (type == SLOT_LINES) return find_lines_ok(v);
    if (type == SLOT_TIME && strchr(v, ' ')) return find_time_ok(v);
    for (const char *p = v; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || strchr("._:-[]?", c))) return false;
    }
    return true;
}

static SlotType slot_from_name(const char *name, size_t len) {
    for (int t = 1; t < SLOT_TYPE_COUNT; t++) {
        const char *n = nl_slot_name((SlotType)t);
        if (strlen(n) == len && strncmp(n, name, len) == 0) return (SlotType)t;
    }
    return SLOT_NONE;
}

static void set_err(char *err, size_t err_size, const char *msg, const char *arg) {
    if (err && err_size) snprintf(err, err_size, msg, arg);
}

/* Build the output filename for {OUT_FILE|ext|suffix}. */
static bool synth_out_file(const ParseContext *ctx, const char *ext_spec, size_t ext_len,
                           const char *suffix, size_t suffix_len,
                           char *dst, size_t dst_size, char *err, size_t err_size) {
    const char *explicit_out = nl_find_slot(ctx, SLOT_FILE, 1);
    if (explicit_out) {
        if (strlen(explicit_out) >= dst_size) return false;
        strcpy(dst, explicit_out);
        return true;
    }
    const char *in = nl_find_slot(ctx, SLOT_FILE, 0);
    if (!in) { set_err(err, err_size, "no input file to derive the output name from%s", ""); return false; }

    const char *slash = strrchr(in, '/');
    const char *base = slash ? slash + 1 : in;
    const char *dot = strrchr(base, '.');
    if (dot == base) dot = NULL;
    size_t stem_len = dot ? (size_t)(dot - in) : strlen(in);
    const char *in_ext = dot ? dot + 1 : "";

    char ext[32];
    const char *user_ext = nl_find_slot(ctx, SLOT_EXT, 0);
    if (ext_len > 0 && ext_spec[0] == '!') {
        snprintf(ext, sizeof ext, "%.*s", (int)(ext_len - 1), ext_spec + 1);
    } else if (user_ext) {
        snprintf(ext, sizeof ext, "%s", user_ext);
    } else if (ext_len > 0) {
        snprintf(ext, sizeof ext, "%.*s", (int)ext_len, ext_spec);
    } else {
        snprintf(ext, sizeof ext, "%s", in_ext[0] ? in_ext : "out");
    }

    int n = snprintf(dst, dst_size, "%.*s%.*s.%s", (int)stem_len, in,
                     (int)suffix_len, suffix, ext);
    if (n < 0 || (size_t)n >= dst_size) return false;
    if (strcmp(dst, in) == 0) {
        n = snprintf(dst, dst_size, "%.*s_out.%s", (int)stem_len, in, ext);
        if (n < 0 || (size_t)n >= dst_size) return false;
    }
    return true;
}

/* Does a FILE value name a regular file (rather than a directory)? */
static bool looks_like_regular_file(const char *f) {
    size_t n = strlen(f);
    if (n == 0 || f[n - 1] == '/' || strcmp(f, ".") == 0 || strcmp(f, "..") == 0) return false;
    const char *slash = strrchr(f, '/');
    const char *base = slash ? slash + 1 : f;
    const char *dot = strrchr(base, '.');
    return dot && dot != base && dot[1] != '\0';
}

static void drop_char(char *s, char c) {
    char *p = strchr(s, c);
    if (p) memmove(p, p + 1, strlen(p));
}

/*
 * {FLAGS|base|allowed}: "-" + base letters + the user's modifier letters
 * (ctx->flags) that are in `allowed`; empty if there are none.
 *  - 'r' is dropped when every FILE is a regular file (grep -n x app.log)
 *  - 'F' (fixed string) replaces 'E'
 *  - 'v' with 'l' becomes 'L' (files that don't match); 'v' cancels 'o'
 * A modifier the template can't express is an error, not silently dropped.
 */
static bool build_flags(const ParseContext *ctx, const char *rest, const char *close,
                        char *dst, size_t dst_size, char *err, size_t err_size) {
    char letters[32] = "";
    const char *allowed = "";
    size_t allowed_len = 0;
    if (*rest == '|') {
        const char *base = rest + 1;
        const char *bar = memchr(base, '|', (size_t)(close - base));
        size_t base_len = (size_t)((bar ? bar : close) - base);
        if (base_len >= sizeof letters) base_len = sizeof letters - 1;
        memcpy(letters, base, base_len);
        letters[base_len] = '\0';
        if (bar) { allowed = bar + 1; allowed_len = (size_t)(close - allowed); }
    }

    int nfiles = nl_count_slots(ctx, SLOT_FILE);
    bool all_regular = nfiles > 0;
    for (int k = 0; k < nfiles; k++)
        if (!looks_like_regular_file(nl_find_slot(ctx, SLOT_FILE, k))) all_regular = false;
    if (all_regular) drop_char(letters, 'r');

    for (const char *f = ctx->flags; *f; f++) {
        if (!memchr(allowed, *f, allowed_len)) {
            static const struct { char f; const char *what; } names[] = {
                { 'i', "ignoring case" }, { 'w', "whole-word matching" },
                { 'v', "inverted matching" }, { 'F', "fixed-string matching" },
            };
            const char *what = "that option";
            for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
                if (names[k].f == *f) what = names[k].what;
            set_err(err, err_size, "%s isn't supported for this command", what);
            return false;
        }
        if (*f == 'v' && strchr(letters, 'l')) { *strchr(letters, 'l') = 'L'; continue; }
        if (*f == 'v' && strchr(letters, 'o')) { drop_char(letters, 'o'); }
        if (*f == 'F') drop_char(letters, 'E');
        if (!strchr(letters, *f) && strlen(letters) + 1 < sizeof letters) {
            size_t n = strlen(letters);
            letters[n] = *f;
            letters[n + 1] = '\0';
        }
    }

    int n = snprintf(dst, dst_size, letters[0] ? "-%s" : "%s", letters);
    return n >= 0 && (size_t)n < dst_size;
}

/* ------------------------------------------------------------------ */
/* find                                                               */
/* ------------------------------------------------------------------ */

/* Does the template reference slot {NAME:idx} (directly, or through one of
 * the find builders)? */
static bool template_uses(const char *tpl, SlotType type, int idx) {
    if ((type == SLOT_EXT || type == SLOT_QUOTED_STR) && strstr(tpl, "{NAMES}")) return true;
    if (type == SLOT_EXCLUDE && strstr(tpl, "{PRUNE}")) return true;
    char key[32];
    int n = snprintf(key, sizeof key, "{%s:%d", nl_slot_name(type), idx);
    for (const char *p = tpl; (p = strstr(p, key)) != NULL; p += n)
        if (p[n] == '}' || p[n] == '|') return true;
    return false;
}

static bool find_has_filter(const ParseContext *ctx) {
    return nl_count_slots(ctx, SLOT_EXT) || nl_count_slots(ctx, SLOT_QUOTED_STR) ||
           nl_count_slots(ctx, SLOT_SIZE) || nl_count_slots(ctx, SLOT_TIME) ||
           nl_count_slots(ctx, SLOT_LINES);
}

/* Refuse templates that would silently drop part of the request. */
static bool find_check(const ParseContext *ctx, const CommandTemplate *tpl,
                       char *err, size_t err_size) {
    const char *t = tpl->flag_template;
    if (ctx->find_stray) {
        set_err(err, err_size, "not sure what '%s' refers to; try e.g. 'first 5', "
                "'larger than 5MB', 'older than 5 days' or 'less than 5 lines'", ctx->find_stray);
        return false;
    }
    int seen[SLOT_TYPE_COUNT] = { 0 };
    for (size_t i = 0; i < ctx->slot_count; i++) {
        SlotType type = ctx->slots[i].type;
        if (template_uses(t, type, seen[type]++)) continue;
        if (type == SLOT_EXCLUDE && strstr(t, "-delete")) {
            set_err(err, err_size, "skipping directories can't be combined with deleting "
                    "(find's -delete turns off -prune); narrow the search path instead%s", "");
            return false;
        }
        static const char *const what[SLOT_TYPE_COUNT] = {
            [SLOT_FILE] = "searching that many directories", [SLOT_EXT] = "a name filter",
            [SLOT_QUOTED_STR] = "a name filter", [SLOT_TIME] = "an age filter",
            [SLOT_NUM] = "a count", [SLOT_SIZE] = "a size filter",
            [SLOT_EXCLUDE] = "excluding directories", [SLOT_LINES] = "a line-count filter",
        };
        set_err(err, err_size, "%s isn't supported for this command",
                what[type] ? what[type] : "that");
        return false;
    }
    if (ctx->find_kind_set && !strstr(t, "{KIND}")) {
        const char *ty = strstr(t, "-type ");
        char want = ty ? ty[6] : 0;
        if (ctx->find_kind != want) {
            set_err(err, err_size, "this command only works on %s",
                    want == 'd' ? "directories" : "regular files");
            return false;
        }
    }
    if (nl_count_slots(ctx, SLOT_LINES)) {
        const char *ty = strstr(t, "-type ");
        char type = strstr(t, "{KIND}") ? (ctx->find_kind ? ctx->find_kind : 'f') : ty ? ty[6] : 0;
        if (type != 'f') {
            set_err(err, err_size, "line counts only apply to regular files%s", "");
            return false;
        }
    }
    if (strchr(ctx->flags, 'i') &&
        (!strstr(t, "{NAMES}") || !(nl_count_slots(ctx, SLOT_EXT) || nl_count_slots(ctx, SLOT_QUOTED_STR)))) {
        set_err(err, err_size, "ignoring case only applies to a name pattern (e.g. named 'readme*')%s", "");
        return false;
    }
    if (ctx->find_depth[0] && !strstr(t, "{DEPTH}")) {
        set_err(err, err_size, "a depth limit isn't supported for this command%s", "");
        return false;
    }
    if ((strstr(t, "-delete") || strstr(t, "rm -")) && !strstr(t, "-empty") && !find_has_filter(ctx)) {
        set_err(err, err_size, "refusing to delete everything; say which files "
                "(e.g. '*.log', named 'core', older than 30 days)%s", "");
        return false;
    }
    return true;
}

/* {KIND} {NAMES} {DEPTH} {PRUNE} {PRINT}. Returns -1 if `name` is not one of
 * them, 0 on error, 1 when expanded. */
static int find_placeholder(const ParseContext *ctx, const char *name, size_t len, Out *o,
                            char *err, size_t err_size) {
    char quoted[NL_MAX_CMD_LEN];
#define IS(lit) (len == sizeof lit - 1 && strncmp(name, lit, len) == 0)
    if (IS("KIND")) {
        char k = ctx->find_kind ? ctx->find_kind : 'f';
        if (k != 'a') { out_puts(o, " -type "); out_putn(o, &k, 1); }
        return 1;
    }
    if (IS("DEPTH")) {
        if (ctx->find_depth[0]) { out_puts(o, " -maxdepth "); out_puts(o, ctx->find_depth); }
        return 1;
    }
    if (IS("PRINT")) {
        /* pruning, or the awk line-count test (-exec is an action), turn
         * off find's implicit -print */
        if (nl_count_slots(ctx, SLOT_EXCLUDE) || nl_count_slots(ctx, SLOT_LINES))
            out_puts(o, " -print");
        return 1;
    }
    if (IS("PRUNE")) {
        for (int k = 0; k < nl_count_slots(ctx, SLOT_EXCLUDE); k++) {
            const char *v = nl_find_slot(ctx, SLOT_EXCLUDE, k);
            char pat[NL_MAX_INPUT + 4];
            snprintf(pat, sizeof pat, "*/%s", v);
            if (!nl_shell_quote(pat, quoted, sizeof quoted)) { o->overflow = true; return 1; }
            out_puts(o, " -path ");
            out_puts(o, quoted);
            out_puts(o, " -prune -o");
        }
        return 1;
    }
    if (IS("NAMES")) {
        const char *test = strchr(ctx->flags, 'i') ? "-iname " : "-name ";
        int next = nl_count_slots(ctx, SLOT_EXT), nstr = nl_count_slots(ctx, SLOT_QUOTED_STR);
        if (next + nstr == 0) return 1;
        bool group = next + nstr > 1;
        if (group) out_puts(o, " \\(");
        for (int k = 0; k < next + nstr; k++) {
            out_puts(o, k ? " -o " : " ");
            out_puts(o, test);
            if (k < next) {
                const char *e = nl_find_slot(ctx, SLOT_EXT, k);
                for (const char *p = e; *p; p++)
                    if (!isalnum((unsigned char)*p)) {
                        set_err(err, err_size, "refusing unsafe value '%s'", e);
                        return 0;
                    }
                out_puts(o, "'*.");
                out_puts(o, e);
                out_puts(o, "'");
            } else {
                if (!nl_shell_quote(nl_find_slot(ctx, SLOT_QUOTED_STR, k - next), quoted, sizeof quoted)) {
                    o->overflow = true;
                    return 1;
                }
                out_puts(o, quoted);
            }
        }
        if (group) out_puts(o, " \\)");
        return 1;
    }
#undef IS
    return -1;
}

/*
 * Expand template text [p, end) into `o`.
 * *placeholders / *all_user track whether an optional group may be emitted.
 */
static bool expand_range(const ParseContext *ctx, const char *p, const char *end, Out *o,
                         bool in_group, int *placeholders, bool *all_user,
                         char *err, size_t err_size) {
    char value[NL_MAX_CMD_LEN];
    char quoted[NL_MAX_CMD_LEN];

    while (p < end) {
        /* Optional group */
        if (p + 1 < end && p[0] == '<' && p[1] == '<') {
            const char *close = strstr(p + 2, ">>");
            if (!close || close > end) { set_err(err, err_size, "unterminated << in template%s", ""); return false; }
            char gbuf[NL_MAX_CMD_LEN];
            Out g = { gbuf, sizeof gbuf, 0, false };
            gbuf[0] = '\0';
            int gph = 0;
            bool guser = true;
            if (!expand_range(ctx, p + 2, close, &g, true, &gph, &guser, err, err_size)) return false;
            if (g.overflow) { o->overflow = true; return true; }
            if (gph > 0 && guser) out_puts(o, gbuf);
            p = close + 2;
            continue;
        }

        if (*p != '{' || p + 1 >= end || !isupper((unsigned char)p[1])) {
            out_putn(o, p, 1);
            p++;
            continue;
        }

        const char *close = memchr(p, '}', (size_t)(end - p));
        if (!close) { out_putn(o, p, 1); p++; continue; }

        const char *name = p + 1;
        size_t name_len = 0;
        while (name + name_len < close && (isupper((unsigned char)name[name_len]) || name[name_len] == '_'))
            name_len++;
        const char *rest = name + name_len; /* ':', '|' or '}' */

        /* ---- {OUT_FILE...} ---- */
        if (name_len == 8 && strncmp(name, "OUT_FILE", 8) == 0 && (*rest == '|' || *rest == '}')) {
            const char *ext_spec = "", *suffix = "";
            size_t ext_len = 0, suffix_len = 0;
            if (*rest == '|') {
                ext_spec = rest + 1;
                const char *bar = memchr(ext_spec, '|', (size_t)(close - ext_spec));
                ext_len = (size_t)((bar ? bar : close) - ext_spec);
                if (bar) { suffix = bar + 1; suffix_len = (size_t)(close - suffix); }
            }
            if (!synth_out_file(ctx, ext_spec, ext_len, suffix, suffix_len,
                                value, sizeof value, err, err_size))
                return false;
            if (!nl_shell_quote(value, quoted, sizeof quoted)) { o->overflow = true; return true; }
            out_puts(o, quoted);
            (*placeholders)++;
            p = close + 1;
            continue;
        }

        /* ---- {FLAGS|base|allowed}: grep-style option cluster ---- */
        if (name_len == 5 && strncmp(name, "FLAGS", 5) == 0 && (*rest == '|' || *rest == '}')) {
            if (!build_flags(ctx, rest, close, value, sizeof value, err, err_size)) return false;
            if (value[0]) out_puts(o, value);
            else if (close + 1 < end && close[1] == ' ') close++; /* no flags: drop the space */
            p = close + 1;
            continue;
        }

        /* ---- {COUNT}: " | length" when the user asked for a count ---- */
        if (name_len == 5 && strncmp(name, "COUNT", 5) == 0 && *rest == '}') {
            if (ctx->wants_count) out_puts(o, " | length");
            p = close + 1;
            continue;
        }

        /* ---- find builders ---- */
        if (ctx->tool == TOOL_FIND && *rest == '}') {
            int r = find_placeholder(ctx, name, name_len, o, err, err_size);
            if (r == 0) return false;
            if (r == 1) { p = close + 1; continue; }
        }

        /* ---- {TYPE:i|default} ---- */
        SlotType type = slot_from_name(name, name_len);
        if (type == SLOT_NONE || *rest != ':') { out_putn(o, p, 1); p++; continue; }

        const char *idx_p = rest + 1;
        int idx = 0;
        while (idx_p < close && isdigit((unsigned char)*idx_p)) idx = idx * 10 + (*idx_p++ - '0');

        const char *def = NULL;
        size_t def_len = 0;
        if (idx_p < close && *idx_p == '|') { def = idx_p + 1; def_len = (size_t)(close - def); }

        const char *v = nl_find_slot(ctx, type, idx);
        (*placeholders)++;
        if (v) {
            if (needs_quoting(type)) {
                if (!nl_shell_quote(v, quoted, sizeof quoted)) { o->overflow = true; return true; }
                out_puts(o, quoted);
            } else {
                if (!raw_value_ok(type, v)) {
                    set_err(err, err_size, "refusing unsafe value '%s'", v);
                    return false;
                }
                out_puts(o, v);
            }
        } else if (def) {
            *all_user = false;
            out_putn(o, def, def_len);
        } else if (in_group) {
            *all_user = false; /* group will be dropped */
        } else {
            if (err && err_size)
                snprintf(err, err_size, "missing %s #%d: expected %s",
                         nl_slot_name(type), idx + 1, nl_slot_hint(type));
            return false;
        }
        p = close + 1;
    }
    return true;
}

bool nl_expand(const ParseContext *ctx, const CommandTemplate *tpl,
               char *out, size_t out_size, char *err, size_t err_size) {
    Out o = { out, out_size, 0, false };
    if (out_size) out[0] = '\0';
    if (err && err_size) err[0] = '\0';

    int placeholders = 0;
    bool all_user = true;
    const char *t = tpl->flag_template;

    if (ctx->tool == TOOL_FIND && !find_check(ctx, tpl, err, err_size)) return false;

    if (!expand_range(ctx, t, t + strlen(t), &o, false, &placeholders, &all_user, err, err_size))
        return false;
    if (o.overflow) { set_err(err, err_size, "command too long%s", ""); return false; }

    while (o.len > 0 && out[o.len - 1] == ' ') out[--o.len] = '\0';
    return true;
}
