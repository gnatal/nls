/*
 * expander.c — template substitution (tool-specific builders live in
 * expander_<tool>.c).
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
 * tar only (see tar_check() for what is refused):
 *   {TAR_FLAGS|c|x|t}    "-czvf": mode + compression letter (from the archive
 *                        suffix or a format word, gzip by default) + v
 *                        (unless "quietly") + f; zstd adds "--zstd "
 *   {ARCHIVE}            the ARCHIVE slot, or when creating a name derived
 *                        from DEST / the single source ("src/" -> src.tar.gz)
 *   {EXCLUDES}           " --exclude='X'" per EXCLUDE slot
 *   {MEMBERS}            " path" per FILE slot (sources or members)
 * git only (see git_check() for what is refused):
 *   {PATHS} {PATHS|dflt} " -- f1 f2" from FILE slots (or " -- dflt")
 *   {AUTHOR}             " --author='x'" per AUTHOR slot ("me": git config user.name)
 *   {SPAN}               diff range: " HEAD~N HEAD", " A B" (between), " REV", or ""
 *   {BASE}               rebase base: REV or HEAD~N (one is required)
 *   {COMMITS}            revert targets: REV, HEAD~N..HEAD, or HEAD
 *   {TARGET}             reset target: REV, or HEAD~N (default HEAD~1)
 * curl only (see curl_check() for what is refused):
 *   {URL}                the URL, re-checked against the allowlist, single-quoted
 *   {OPTS} {OPTS|L}      " -L -k --compressed -g --retry N --max-time S -u 'user'"
 *                        from modifiers; letters after | are already in the template
 *   {HEADERS}            " -H '...'" per HEADER (double quotes for a trailing $VAR)
 *   {BODY}               " -H 'Content-Type: ...' -d @file" / " -d '{...}'" / form fields
 *   {UPLOAD}             " -F 'file=@path'" per FILE
 *   {OUTPUT} {OUTPUT|O}  " -o name" / " --output-dir d/ -O"; |O: " -O" by default and
 *                        a lone FILE is the output name (downloads)
 * magick only (see magick_check() for what is refused):
 *   {RESIZE}             " -resize GEOM" ('GEOM!' exact, 'GEOM>' only shrink), or ""
 *   {CROP}               "WxH+0+0"; "N%" becomes "N%xN%+0+0" (magick ignores "N%+0+0")
 *   {QUALITY}            " -quality N", or ""
 *   {INPUTS}             " in1 in2 ..." every FILE (globs stay quoted: magick expands them)
 *   {GLOB}               'dir/\*.ext' from FILE:0 (a folder, optional) and EXT:0
 *   {DELAY}              " -delay N" in 1/100 s from FPS (default 10 fps)
 *   {OUT_NAME|stem|ext}  DEST, or DEST-folder/stem.ext, or stem.(target EXT or ext)
 *   {OUT_FILE...}        uses DEST (not FILE:1) as the explicit output; a folder
 *                        DEST keeps the derived name. A suffix written "?_sfx" is
 *                        only added when the name would otherwise be the input's.
 * git QUOTED_STR (messages, search text) is always single-quoted; REV,
 * BRANCH and EXCLUDE are spliced raw after git_ref_ok().
 *
 * FILE, QUOTED_STR and OUT_FILE values are POSIX shell-quoted. All other
 * slot values are produced by the lexer from a restricted alphabet and are
 * re-validated here before being spliced in raw (they may appear inside
 * single-quoted program text such as a jq filter).
 */
#include "nl.h"
#include "expander_int.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void out_putn(Out *o, const char *s, size_t n) {
    if (o->len + n + 1 > o->size) { o->overflow = true; return; }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

void out_puts(Out *o, const char *s) { out_putn(o, s, strlen(s)); }

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
    return t == SLOT_FILE || t == SLOT_QUOTED_STR || t == SLOT_ARCHIVE || t == SLOT_DEST ||
           t == SLOT_AUTHOR || t == SLOT_COLOR;
}

/* git REV / BRANCH / EXCLUDE: ref-name characters plus ~ and ^ (HEAD~2),
 * none of which the shell treats specially mid-word; no leading '-' or
 * '~', no "..". */
bool git_ref_ok(const char *v) {
    if (!v[0] || v[0] == '-' || v[0] == '~' || v[0] == '^' || strstr(v, "..")) return false;
    for (const char *p = v; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || strchr("._/-~^", c))) return false;
    }
    return true;
}

/* git LINES: digits,digits */
static bool git_lines_ok(const char *v) {
    if (!isdigit((unsigned char)*v)) return false;
    while (isdigit((unsigned char)*v)) v++;
    if (*v++ != ',' || !isdigit((unsigned char)*v)) return false;
    while (isdigit((unsigned char)*v)) v++;
    return *v == '\0';
}

/* git TIME: --since='...' / --until='...' with [A-Za-z0-9 -] inside */
static bool git_time_ok(const char *v) {
    if (strncmp(v, "--since='", 9) != 0 && strncmp(v, "--until='", 9) != 0) return false;
    v += 9;
    if (!*v || *v == '\'') return false;
    while (*v && *v != '\'') {
        unsigned char c = (unsigned char)*v++;
        if (!(isalnum(c) || c == ' ' || c == '-')) return false;
    }
    return v[0] == '\'' && v[1] == '\0';
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

/* magick geometry: WxH, Wx, xH, W, or N% (N may have a fraction) */
bool magick_geom_ok(const char *v) {
    const char *p = v;
    size_t a = 0, b = 0;
    while (isdigit((unsigned char)p[a])) a++;
    if (p[a] == '%' || p[a] == '.') {
        if (a == 0) return false;
        p += a;
        if (*p == '.') { p++; if (!isdigit((unsigned char)*p)) return false; while (isdigit((unsigned char)*p)) p++; }
        return p[0] == '%' && p[1] == '\0';
    }
    if (p[a] == '\0') return a > 0 && a <= 5;
    if (p[a] != 'x') return false;
    p += a + 1;
    while (isdigit((unsigned char)p[b])) b++;
    return p[b] == '\0' && (a || b) && a <= 5 && b <= 5;
}

/* Raw-spliced values must stay inside this alphabet. */
bool raw_value_ok(SlotType type, const char *v) {
    if (type == SLOT_SIZE) return find_size_ok(v);
    if (type == SLOT_RES && strchr(v, '%')) return magick_geom_ok(v);
    if (type == SLOT_REV || type == SLOT_BRANCH || type == SLOT_EXCLUDE) return git_ref_ok(v);
    if (type == SLOT_LINES) return strchr(v, ',') ? git_lines_ok(v) : find_lines_ok(v);
    if (type == SLOT_TIME && strncmp(v, "--", 2) == 0) return git_time_ok(v);
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

void set_err(char *err, size_t err_size, const char *msg, const char *arg) {
    if (err && err_size) snprintf(err, err_size, msg, arg);
}

/* Build the output filename for {OUT_FILE|ext|suffix}. */
static bool synth_out_file(const ParseContext *ctx, const char *ext_spec, size_t ext_len,
                           const char *suffix, size_t suffix_len,
                           char *dst, size_t dst_size, char *err, size_t err_size) {
    const char *explicit_out = nl_find_slot(ctx, SLOT_FILE, 1);
    const char *dir = "";
    bool cond = suffix_len > 0 && suffix[0] == '?';   /* "?_sfx": only to avoid overwriting */
    if (cond) { suffix++; suffix_len--; }
    if (ctx->tool == TOOL_MAGICK) {
        /* the output is DEST ("to out.webp"); a folder ("into small/") keeps the name */
        explicit_out = nl_find_slot(ctx, SLOT_DEST, 0);
        if (explicit_out && explicit_out[0] && explicit_out[strlen(explicit_out) - 1] == '/') {
            dir = explicit_out;
            explicit_out = NULL;
        }
    }
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
    if (ctx->tool == TOOL_MAGICK) {   /* "... to webp" is the last EXT */
        int n_ext = nl_count_slots(ctx, SLOT_EXT);
        user_ext = n_ext ? nl_find_slot(ctx, SLOT_EXT, n_ext - 1) : NULL;
    }
    if (ext_len > 0 && ext_spec[0] == '!') {
        snprintf(ext, sizeof ext, "%.*s", (int)(ext_len - 1), ext_spec + 1);
    } else if (user_ext) {
        snprintf(ext, sizeof ext, "%s", user_ext);
    } else if (ext_len > 0) {
        snprintf(ext, sizeof ext, "%.*s", (int)ext_len, ext_spec);
    } else {
        snprintf(ext, sizeof ext, "%s", in_ext[0] ? in_ext : "out");
    }

    /* into a folder: keep only the base name */
    const char *stem = in;
    if (dir[0]) { stem = base; stem_len = dot ? (size_t)(dot - base) : strlen(base); }

    int n = snprintf(dst, dst_size, "%s%.*s%.*s.%s", dir, (int)stem_len, stem,
                     cond ? 0 : (int)suffix_len, suffix, ext);
    if (n < 0 || (size_t)n >= dst_size) return false;
    if (cond && strcmp(dst, in) == 0)
        n = snprintf(dst, dst_size, "%s%.*s%.*s.%s", dir, (int)stem_len, stem,
                     (int)suffix_len, suffix, ext);
    if (n < 0 || (size_t)n >= dst_size) return false;
    if (strcmp(dst, in) == 0) {
        n = snprintf(dst, dst_size, "%s%.*s_out.%s", dir, (int)stem_len, stem, ext);
        if (n < 0 || (size_t)n >= dst_size) return false;
    }
    return true;
}

/* Does a FILE value name a regular file (rather than a directory)? */
bool looks_like_regular_file(const char *f) {
    size_t n = strlen(f);
    if (n == 0 || f[n - 1] == '/' || strcmp(f, ".") == 0 || strcmp(f, "..") == 0) return false;
    const char *slash = strrchr(f, '/');
    const char *base = slash ? slash + 1 : f;
    const char *dot = strrchr(base, '.');
    return dot && dot != base && dot[1] != '\0';
}

bool all_digits_str(const char *s) {
    if (!*s) return false;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return false;
    return true;
}

/* Value of a short digit string (callers check all_digits_str first). */
int atoi_safe(const char *s) {
    long v = 0;
    for (; isdigit((unsigned char)*s) && v < 1000000; s++) v = v * 10 + (*s - '0');
    return (int)v;
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

/* Does the template reference slot {NAME:idx} (directly, or through one of
 * the find builders)? */
bool template_uses(const char *tpl, SlotType type, int idx) {
    if ((type == SLOT_EXT || type == SLOT_QUOTED_STR) && strstr(tpl, "{NAMES}")) return true;
    if (type == SLOT_EXCLUDE && strstr(tpl, "{PRUNE}")) return true;
    char key[32];
    int n = snprintf(key, sizeof key, "{%s:%d", nl_slot_name(type), idx);
    for (const char *p = tpl; (p = strstr(p, key)) != NULL; p += n)
        if (p[n] == '}' || p[n] == '|') return true;
    return false;
}

/* Single-quote `v` unconditionally (--exclude='.git' reads better than
 * --exclude=.git and keeps globs away from the shell). */
void put_single_quoted(Out *o, const char *v) {
    out_putn(o, "'", 1);
    for (const char *p = v; *p; p++) {
        if (*p == '\'') out_puts(o, "'\\''");
        else out_putn(o, p, 1);
    }
    out_putn(o, "'", 1);
}

/* ------------------------------------------------------------------ */
/* Tool registry                                                      */
/* ------------------------------------------------------------------ */

static const ToolExpander *const EXPANDERS[TOOL_COUNT] = {
    [TOOL_FIND]   = &nl_expander_find,
    [TOOL_TAR]    = &nl_expander_tar,
    [TOOL_GIT]    = &nl_expander_git,
    [TOOL_CURL]   = &nl_expander_curl,
    [TOOL_MAGICK] = &nl_expander_magick,
};

static const ToolExpander *expander_for(ToolId tool) {
    return tool > TOOL_UNKNOWN && tool < TOOL_COUNT ? EXPANDERS[tool] : NULL;
}

/* ------------------------------------------------------------------ */
/* Template expansion                                                 */
/* ------------------------------------------------------------------ */

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

        /* ---- tool builders ---- */
        const ToolExpander *tx = expander_for(ctx->tool);
        if (tx && tx->placeholder) {
            Placeholder ph = { name, name_len, rest, close, placeholders, all_user };
            int r = tx->placeholder(ctx, &ph, o, err, err_size);
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
            if (ctx->tool == TOOL_GIT && type == SLOT_QUOTED_STR) {
                put_single_quoted(o, v);   /* messages & search text: always '...' */
            } else if (needs_quoting(type)) {
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

bool nl_check(const ParseContext *ctx, const CommandTemplate *tpl) {
    const ToolExpander *tx = expander_for(ctx->tool);
    char err[16];
    return !(tx && tx->check) || tx->check(ctx, tpl, err, sizeof err);
}

bool nl_expand(const ParseContext *ctx, const CommandTemplate *tpl,
               char *out, size_t out_size, char *err, size_t err_size) {
    Out o = { out, out_size, 0, false };
    if (out_size) out[0] = '\0';
    if (err && err_size) err[0] = '\0';

    int placeholders = 0;
    bool all_user = true;
    const char *t = tpl->flag_template;

    const ToolExpander *tx = expander_for(ctx->tool);
    if (tx && tx->check && !tx->check(ctx, tpl, err, err_size)) return false;
    if (tx && tx->prologue && !tx->prologue(ctx, tpl, &o, err, err_size)) return false;

    if (!expand_range(ctx, t, t + strlen(t), &o, false, &placeholders, &all_user, err, err_size))
        return false;
    if (o.overflow) { set_err(err, err_size, "command too long%s", ""); return false; }

    while (o.len > 0 && out[o.len - 1] == ' ') out[--o.len] = '\0';
    return true;
}
