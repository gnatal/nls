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
 *   << ... >>            optional group: emitted only if every placeholder
 *                        inside resolved to a user-supplied value
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

    /* Keep a leading "~/" unquoted so the shell still expands it. */
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

/* Raw-spliced values must stay inside this alphabet. */
static bool raw_value_ok(const char *v) {
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

        /* ---- {COUNT}: " | length" when the user asked for a count ---- */
        if (name_len == 5 && strncmp(name, "COUNT", 5) == 0 && *rest == '}') {
            if (ctx->wants_count) out_puts(o, " | length");
            p = close + 1;
            continue;
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
                if (!raw_value_ok(v)) {
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

    if (!expand_range(ctx, t, t + strlen(t), &o, false, &placeholders, &all_user, err, err_size))
        return false;
    if (o.overflow) { set_err(err, err_size, "command too long%s", ""); return false; }

    while (o.len > 0 && out[o.len - 1] == ' ') out[--o.len] = '\0';
    return true;
}
