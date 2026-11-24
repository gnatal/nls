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
    return t == SLOT_FILE || t == SLOT_QUOTED_STR || t == SLOT_ARCHIVE || t == SLOT_DEST ||
           t == SLOT_AUTHOR || t == SLOT_COLOR;
}

/* git REV / BRANCH / EXCLUDE: ref-name characters plus ~ and ^ (HEAD~2),
 * none of which the shell treats specially mid-word; no leading '-' or
 * '~', no "..". */
static bool git_ref_ok(const char *v) {
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
static bool magick_geom_ok(const char *v) {
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
static bool raw_value_ok(SlotType type, const char *v) {
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

static void set_err(char *err, size_t err_size, const char *msg, const char *arg) {
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
        if (explicit_out && explicit_out[strlen(explicit_out) - 1] == '/') {
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
static bool looks_like_regular_file(const char *f) {
    size_t n = strlen(f);
    if (n == 0 || f[n - 1] == '/' || strcmp(f, ".") == 0 || strcmp(f, "..") == 0) return false;
    const char *slash = strrchr(f, '/');
    const char *base = slash ? slash + 1 : f;
    const char *dot = strrchr(base, '.');
    return dot && dot != base && dot[1] != '\0';
}

static bool all_digits_str(const char *s) {
    if (!*s) return false;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return false;
    return true;
}

/* Value of a short digit string (callers check all_digits_str first). */
static int atoi_safe(const char *s) {
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
    if (ctx->stray) {
        set_err(err, err_size, "not sure what '%s' refers to; try e.g. 'first 5', "
                "'larger than 5MB', 'older than 5 days' or 'less than 5 lines'", ctx->stray);
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

/* ------------------------------------------------------------------ */
/* tar                                                                */
/* ------------------------------------------------------------------ */

static const char *tar_comp_name(char c) {
    switch (c) {
    case 'z': return "gzip";
    case 'j': return "bzip2";
    case 'J': return "xz";
    case 'Z': return "zstd";
    default:  return "no compression";
    }
}

static const char *tar_comp_suffix(char c) {
    switch (c) {
    case 'z': return ".tar.gz";
    case 'j': return ".tar.bz2";
    case 'J': return ".tar.xz";
    case 'Z': return ".tar.zst";
    default:  return ".tar";
    }
}

/* The operation a tar template performs: 'c', 'x' or 't'. */
static char tar_mode(const CommandTemplate *tpl) {
    const char *m = strstr(tpl->flag_template, "{TAR_FLAGS|");
    return m ? m[11] : 0;
}

/* Compression to use: from the archive name, else the requested format,
 * else gzip when creating. 0 (with err set) if they disagree. */
static char tar_compression(const ParseContext *ctx, char *err, size_t err_size) {
    const char *archive = nl_find_slot(ctx, SLOT_ARCHIVE, 0);
    const char *ext = nl_find_slot(ctx, SLOT_EXT, 0);
    char a = archive ? nl_tar_compression(archive, true) : 0;
    char e = 0;
    if (ext) {
        char dotted[16];
        snprintf(dotted, sizeof dotted, ".%s", ext);
        e = nl_tar_compression(dotted, false);
    }
    if (a && e && a != e) {
        if (err && err_size)
            snprintf(err, err_size, "'%s' is a %s archive, but %s was asked for",
                     archive, tar_comp_name(a), tar_comp_name(e));
        return 0;
    }
    return a ? a : e ? e : 'z';
}

/* Does `name` end in a non-tar extension ("backup.zip")? */
static bool has_foreign_ext(const char *name) {
    const char *slash = strrchr(name, '/');
    const char *base = slash ? slash + 1 : name;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base || !dot[1]) return false;
    bool alpha = false;
    for (const char *p = dot + 1; *p; p++) {
        if (!isalnum((unsigned char)*p)) return false;
        if (isalpha((unsigned char)*p)) alpha = true;
    }
    return alpha && strlen(dot + 1) <= 5;
}

/* Archive name for "compress src/" with no name given: src.tar.gz; with
 * "into backup" or "into out/": backup.tar.gz, out/src.tar.gz. */
static bool tar_derive_archive(const ParseContext *ctx, char comp, char *dst, size_t size,
                               char *err, size_t err_size) {
    const char *dest = nl_find_slot(ctx, SLOT_DEST, 0);
    const char *dir = "";
    char stem[NL_MAX_INPUT];
    size_t dl = dest ? strlen(dest) : 0;
    if (dest && dest[dl - 1] != '/') {
        if (has_foreign_ext(dest)) {
            set_err(err, err_size, "'%s' isn't a tar archive name; use .tar, .tar.gz, .tgz, "
                    ".tar.bz2, .tar.xz or .tar.zst", dest);
            return false;
        }
        snprintf(stem, sizeof stem, "%s", dest);
    } else {
        if (dest) dir = dest;
        stem[0] = '\0';
        if (nl_count_slots(ctx, SLOT_FILE) == 1) {
            snprintf(stem, sizeof stem, "%s", nl_find_slot(ctx, SLOT_FILE, 0));
            size_t n = strlen(stem);
            while (n > 1 && stem[n - 1] == '/') stem[--n] = '\0';
            const char *slash = strrchr(stem, '/');
            if (slash) memmove(stem, slash + 1, strlen(slash + 1) + 1);
        }
        if (!stem[0] || strcmp(stem, ".") == 0 || strcmp(stem, "..") == 0 ||
            strcmp(stem, "~") == 0 || strcmp(stem, "/") == 0)
            snprintf(stem, sizeof stem, "archive");
    }
    int n = snprintf(dst, size, "%s%s%s", dir, stem, tar_comp_suffix(comp));
    return n >= 0 && (size_t)n < size;
}

/* Refuse requests the chosen template can't fully express. */
static bool tar_check(const ParseContext *ctx, const CommandTemplate *tpl,
                      char *err, size_t err_size) {
    char mode = tar_mode(tpl);
    if (ctx->stray) {
        set_err(err, err_size, "not sure what '%s' refers to; numbers are only used in "
                "e.g. 'strip 1 folder' when extracting", ctx->stray);
        return false;
    }
    if (nl_count_slots(ctx, SLOT_ARCHIVE) > 1) {
        set_err(err, err_size, "one archive at a time, please (got '%s' and another)",
                nl_find_slot(ctx, SLOT_ARCHIVE, 0));
        return false;
    }
    if (nl_count_slots(ctx, SLOT_DEST) > 1) {
        set_err(err, err_size, "more than one destination given ('%s' and another)",
                nl_find_slot(ctx, SLOT_DEST, 0));
        return false;
    }
    if (nl_count_slots(ctx, SLOT_EXT) > 1) {
        set_err(err, err_size, "pick one compression format%s", "");
        return false;
    }
    if (mode != 'x' && nl_count_slots(ctx, SLOT_NUM)) {
        set_err(err, err_size, "stripping leading folders only applies when extracting%s", "");
        return false;
    }
    if (mode == 'c' && nl_find_slot(ctx, SLOT_ARCHIVE, 0) && nl_find_slot(ctx, SLOT_DEST, 0)) {
        set_err(err, err_size, "got both an archive name and a destination ('%s'); "
                "give the full path instead, e.g. into out/backup.tar.gz",
                nl_find_slot(ctx, SLOT_DEST, 0));
        return false;
    }
    if (mode == 't' && nl_find_slot(ctx, SLOT_DEST, 0)) {
        set_err(err, err_size, "a destination ('%s') only applies when extracting",
                nl_find_slot(ctx, SLOT_DEST, 0));
        return false;
    }
    return tar_compression(ctx, err, err_size) != 0;
}

/* Single-quote `v` unconditionally (--exclude='.git' reads better than
 * --exclude=.git and keeps globs away from the shell). */
static void put_single_quoted(Out *o, const char *v) {
    out_putn(o, "'", 1);
    for (const char *p = v; *p; p++) {
        if (*p == '\'') out_puts(o, "'\\''");
        else out_putn(o, p, 1);
    }
    out_putn(o, "'", 1);
}

/* {EXCLUDES} {MEMBERS} {ARCHIVE} {TAR_FLAGS|mode}. Returns -1 if `name` is
 * not one of them, 0 on error, 1 when expanded. */
static int tar_placeholder(const ParseContext *ctx, const char *name, size_t len,
                           const char *rest, Out *o, char *err, size_t err_size) {
    char buf[NL_MAX_CMD_LEN];
    char quoted[NL_MAX_CMD_LEN];
#define IS(lit) (len == sizeof lit - 1 && strncmp(name, lit, len) == 0)
    if (IS("EXCLUDES") && *rest == '}') {
        for (int k = 0; k < nl_count_slots(ctx, SLOT_EXCLUDE); k++) {
            out_puts(o, " --exclude=");
            put_single_quoted(o, nl_find_slot(ctx, SLOT_EXCLUDE, k));
        }
        return 1;
    }
    if (IS("MEMBERS") && *rest == '}') {
        for (int k = 0; k < nl_count_slots(ctx, SLOT_FILE); k++) {
            const char *v = nl_find_slot(ctx, SLOT_FILE, k);
            /* a name starting with '-' must not be read as an option */
            if (v[0] == '-') { snprintf(buf, sizeof buf, "./%s", v); v = buf; }
            if (!nl_shell_quote(v, quoted, sizeof quoted)) { o->overflow = true; return 1; }
            out_puts(o, " ");
            out_puts(o, quoted);
        }
        return 1;
    }
    if (IS("ARCHIVE") && *rest == '}') {
        const char *a = nl_find_slot(ctx, SLOT_ARCHIVE, 0);
        if (!a) {
            if (!tar_derive_archive(ctx, tar_compression(ctx, err, err_size), buf, sizeof buf,
                                    err, err_size))
                return 0;
            a = buf;
        }
        if (a[0] == '-') {   /* keep "-rf.tar.gz" from looking like options */
            snprintf(quoted, sizeof quoted, "./%s", a);
            snprintf(buf, sizeof buf, "%s", quoted);
            a = buf;
        }
        if (!nl_shell_quote(a, quoted, sizeof quoted)) { o->overflow = true; return 1; }
        out_puts(o, quoted);
        return 1;
    }
    if (IS("TAR_FLAGS") && rest[0] == '|' && strchr("cxt", rest[1]) && rest[2] == '}') {
        char comp = tar_compression(ctx, err, err_size);
        if (!comp) return 0;
        if (comp == 'Z') out_puts(o, "--zstd ");
        out_putn(o, "-", 1);
        out_putn(o, rest + 1, 1);
        if (strchr("zjJ", comp)) out_putn(o, &comp, 1);
        if (!strchr(ctx->flags, 'q')) out_putn(o, "v", 1);
        out_putn(o, "f", 1);
        return 1;
    }
#undef IS
    return -1;
}

/* ------------------------------------------------------------------ */
/* git                                                                */
/* ------------------------------------------------------------------ */

static bool git_uses_span(const char *t) {
    return strstr(t, "{SPAN}") || strstr(t, "{BASE}") || strstr(t, "{COMMITS}") ||
           strstr(t, "{TARGET}");
}

/* Does a git template consume slot TYPE #idx, directly or via a builder? */
static bool git_template_uses(const char *t, SlotType type, int idx) {
    if (type == SLOT_FILE && strstr(t, "{PATHS")) return true;
    if (type == SLOT_AUTHOR && strstr(t, "{AUTHOR}")) return true;
    if (idx == 0 && (type == SLOT_NUM || type == SLOT_REV) && git_uses_span(t)) return true;
    if (idx == 0 && type == SLOT_EXCLUDE && strstr(t, "{SPAN}")) return true;
    return template_uses(t, type, idx);
}

/* Refuse templates that would silently drop or misread part of the request. */
static bool git_check(const ParseContext *ctx, const CommandTemplate *tpl,
                      char *err, size_t err_size) {
    const char *t = tpl->flag_template;
    if (ctx->stray) {
        if (strpbrk(ctx->stray, "$`;|&<>(){}*\\\"'!"))
            set_err(err, err_size, "not sure what '%s' is; quote names and messages, "
                    "e.g. branch 'feature-x' or message 'fix bug'", ctx->stray);
        else
            set_err(err, err_size, "not sure what '%s' refers to; numbers are used in e.g. "
                    "'last 3 commits', '3 commits ago', 'last 7 days' or 'lines 10 to 20'", ctx->stray);
        return false;
    }
    int seen[SLOT_TYPE_COUNT] = { 0 };
    for (size_t i = 0; i < ctx->slot_count; i++) {
        SlotType type = ctx->slots[i].type;
        if (git_template_uses(t, type, seen[type]++)) continue;
        static const char *const what[SLOT_TYPE_COUNT] = {
            [SLOT_FILE] = "a file path", [SLOT_TIME] = "a date filter",
            [SLOT_NUM] = "a number of commits", [SLOT_QUOTED_STR] = "a message or search text",
            [SLOT_EXCLUDE] = "a branch to leave out", [SLOT_LINES] = "a line range",
            [SLOT_REV] = "a revision", [SLOT_BRANCH] = "a branch name",
            [SLOT_AUTHOR] = "an author filter",
        };
        const char *v = ctx->slots[i].value;
        if (err && err_size)
            snprintf(err, err_size, "%s ('%s') isn't supported for this command%s",
                     what[type] ? what[type] : "that", v,
                     seen[type] > 1 ? " (or not that many of them)" : "");
        return false;
    }
    bool has_rev = nl_count_slots(ctx, SLOT_REV) || nl_count_slots(ctx, SLOT_EXCLUDE);
    if (git_uses_span(t) && nl_count_slots(ctx, SLOT_NUM) && has_rev) {
        set_err(err, err_size, "give either a number of commits or a revision ('%s'), not both",
                nl_find_slot(ctx, SLOT_REV, 0) ? nl_find_slot(ctx, SLOT_REV, 0)
                                               : nl_find_slot(ctx, SLOT_EXCLUDE, 0));
        return false;
    }
    const char *num = nl_find_slot(ctx, SLOT_NUM, 0);
    if (num) {
        long v = 0;
        for (const char *p = num; *p && v < 100000; p++) v = v * 10 + (*p - '0');
        if (v < 1 || v > 9999) {
            set_err(err, err_size, "'%s' isn't a usable number of commits", num);
            return false;
        }
        if (strcmp(tpl->id, "git_squash") == 0 && v < 2) {
            set_err(err, err_size, "squashing needs at least 2 commits (got %s)", num);
            return false;
        }
    }
    if (strstr(t, "--follow") && nl_count_slots(ctx, SLOT_FILE) > 1) {
        set_err(err, err_size, "following renames works on one file at a time%s", "");
        return false;
    }
    if (!strstr(t, "{PATHS")) {
        for (int k = 0; k < nl_count_slots(ctx, SLOT_FILE); k++)
            if (nl_find_slot(ctx, SLOT_FILE, k)[0] == '-') {
                set_err(err, err_size, "refusing file name '%s' (it would be read as an option); "
                        "write it as ./name", nl_find_slot(ctx, SLOT_FILE, k));
                return false;
            }
    }
    return true;
}

static bool git_put_ref(Out *o, const char *v, char *err, size_t err_size) {
    if (!git_ref_ok(v)) { set_err(err, err_size, "refusing unsafe value '%s'", v); return false; }
    out_puts(o, v);
    return true;
}

/* {PATHS} {PATHS|default} {AUTHOR} {SPAN} {BASE} {COMMITS}. Returns -1 if
 * `name` is not one of them, 0 on error, 1 when expanded. Like a {TYPE:i}
 * placeholder, a builder that emitted user input counts toward its << >>
 * group, and one that emitted nothing makes the group drop. */
static int git_placeholder(const ParseContext *ctx, const char *name, size_t len,
                           const char *rest, const char *close, Out *o,
                           int *placeholders, bool *all_user, char *err, size_t err_size) {
    char quoted[NL_MAX_CMD_LEN];
    char buf[64];
#define IS(lit) (len == sizeof lit - 1 && strncmp(name, lit, len) == 0)
    if (IS("PATHS") && (*rest == '}' || *rest == '|')) {
        int n = nl_count_slots(ctx, SLOT_FILE);
        if (n == 0) {
            *all_user = false;
            if (*rest == '|') { out_puts(o, " -- "); out_putn(o, rest + 1, (size_t)(close - rest - 1)); }
            return 1;
        }
        out_puts(o, " --");
        for (int k = 0; k < n; k++) {
            if (!nl_shell_quote(nl_find_slot(ctx, SLOT_FILE, k), quoted, sizeof quoted)) {
                o->overflow = true;
                return 1;
            }
            out_puts(o, " ");
            out_puts(o, quoted);
        }
        (*placeholders)++;
        return 1;
    }
    if (*rest != '}') return -1;
    if (IS("AUTHOR")) {
        int n = nl_count_slots(ctx, SLOT_AUTHOR);
        if (n == 0) *all_user = false;
        for (int k = 0; k < n; k++) {
            const char *a = nl_find_slot(ctx, SLOT_AUTHOR, k);
            if (strcmp(a, "me") == 0) out_puts(o, " --author=\"$(git config user.name)\"");
            else { out_puts(o, " --author="); put_single_quoted(o, a); }
            (*placeholders)++;
        }
        return 1;
    }
    const char *num = nl_find_slot(ctx, SLOT_NUM, 0);
    const char *rev = nl_find_slot(ctx, SLOT_REV, 0);
    if (num && !all_digits_str(num)) { set_err(err, err_size, "refusing unsafe value '%s'", num); return 0; }
    if (IS("SPAN")) {
        /* git diff: " HEAD~3 HEAD", " main feature", " main", or nothing */
        const char *excl = nl_find_slot(ctx, SLOT_EXCLUDE, 0);
        if (num || ctx->git_last) {
            snprintf(buf, sizeof buf, " HEAD~%s HEAD", num ? num : "1");
            out_puts(o, buf);
        } else if (excl) {
            out_puts(o, " ");
            if (!git_put_ref(o, excl, err, err_size)) return 0;
            out_puts(o, " ");
            if (!git_put_ref(o, rev ? rev : "HEAD", err, err_size)) return 0;
        } else if (rev) {
            out_puts(o, " ");
            if (!git_put_ref(o, rev, err, err_size)) return 0;
        } else {
            *all_user = false;
            return 1;
        }
        (*placeholders)++;
        return 1;
    }
    if (IS("BASE")) {
        /* git rebase -i: a base revision, or HEAD~N */
        if (rev) { if (!git_put_ref(o, rev, err, err_size)) return 0; }
        else if (num) { snprintf(buf, sizeof buf, "HEAD~%s", num); out_puts(o, buf); }
        else {
            set_err(err, err_size, "say how many commits (e.g. 'last 3 commits') or a base "
                    "(e.g. 'onto main')%s", "");
            return 0;
        }
        (*placeholders)++;
        return 1;
    }
    if (IS("TARGET")) {
        /* git reset: a revision, else HEAD~N (default HEAD~1) */
        if (rev) { if (!git_put_ref(o, rev, err, err_size)) return 0; }
        else { snprintf(buf, sizeof buf, "HEAD~%s", num ? num : "1"); out_puts(o, buf); }
        (*placeholders)++;
        return 1;
    }
    if (IS("COMMITS")) {
        /* git revert: a revision, "HEAD~N..HEAD" for the last N, else HEAD */
        if (rev) { if (!git_put_ref(o, rev, err, err_size)) return 0; }
        else if (num && strcmp(num, "1") != 0) { snprintf(buf, sizeof buf, "HEAD~%s..HEAD", num); out_puts(o, buf); }
        else out_puts(o, "HEAD");
        (*placeholders)++;
        return 1;
    }
#undef IS
    return -1;
}

/* ------------------------------------------------------------------ */
/* curl                                                               */
/* ------------------------------------------------------------------ */

static bool strncasecmp_ascii(const char *s, const char *prefix, size_t n) {
    for (size_t k = 0; k < n; k++)
        if (!s[k] || tolower((unsigned char)s[k]) != prefix[k]) return false;
    return true;
}

/* A header line: Name: value, no control characters. */
static bool curl_header_ok(const char *v) {
    size_t n = 0;
    while (isalnum((unsigned char)v[n]) || v[n] == '-' || v[n] == '_') n++;
    if (n == 0 || v[n] != ':') return false;
    for (const char *p = v; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == 0x7f) return false;
    return true;
}

/* "Authorization: Bearer $TOKEN": a header whose only shell-special part is
 * one trailing $NAME. Emitted in double quotes so the variable expands. */
static bool curl_header_env(const char *v) {
    const char *d = strchr(v, '$');
    if (!d || strchr(d + 1, '$')) return false;
    for (const char *p = v; p < d; p++)
        if (strchr("\"`\\!", *p)) return false;
    const char *q = d + 1;
    if (!(isalpha((unsigned char)*q) || *q == '_')) return false;
    for (; *q; q++)
        if (!(isalnum((unsigned char)*q) || *q == '_')) return false;
    return true;
}

static bool curl_form_body(const char *v) {
    return v[0] != '{' && v[0] != '[' && strchr(v, '=') != NULL;
}

/* Content-Type for a body file, by extension (NULL: let curl decide). */
static const char *curl_content_type(const char *file) {
    static const struct { const char *ext; const char *type; } types[] = {
        { "json", "application/json" }, { "xml", "application/xml" },
        { "yaml", "application/yaml" }, { "yml", "application/yaml" },
        { "csv", "text/csv" }, { "txt", "text/plain" }, { "html", "text/html" },
        { "ndjson", "application/x-ndjson" }, { "jsonl", "application/x-ndjson" },
        { "graphql", "application/graphql" },
    };
    const char *slash = strrchr(file, '/');
    const char *dot = strrchr(slash ? slash + 1 : file, '.');
    if (!dot) return NULL;
    char low[16];
    size_t n = strlen(dot + 1);
    if (n == 0 || n >= sizeof low) return NULL;
    for (size_t i = 0; i <= n; i++) low[i] = (char)tolower((unsigned char)dot[1 + i]);
    for (size_t k = 0; k < sizeof types / sizeof types[0]; k++)
        if (strcmp(low, types[k].ext) == 0) return types[k].type;
    return NULL;
}

/* With {OUTPUT|O} (downloads) a lone FILE is the output name:
 * "download report.pdf from https://...". */
static const char *curl_output(const ParseContext *ctx, bool download) {
    const char *d = nl_find_slot(ctx, SLOT_DEST, 0);
    if (d) return d;
    if (download) return nl_find_slot(ctx, SLOT_FILE, 0);
    return NULL;
}

/* The URL's last path segment, for -O ("" if there is none). */
static void curl_remote_name(const char *url, char *out, size_t size) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t end = strcspn(p, "?#");
    const char *slash = NULL;
    for (size_t k = 0; k < end; k++) if (p[k] == '/') slash = p + k;
    if (!slash) { out[0] = '\0'; return; }
    size_t n = (size_t)(p + end - (slash + 1));
    if (n >= size) n = size - 1;
    memcpy(out, slash + 1, n);
    out[n] = '\0';
}

/* Refuse requests the chosen template can't fully express. */
static bool curl_check(const ParseContext *ctx, const CommandTemplate *tpl,
                       char *err, size_t err_size) {
    const char *t = tpl->flag_template;
    bool has_body = strstr(t, "{BODY}") != NULL;
    bool has_upload = strstr(t, "{UPLOAD}") != NULL;
    bool has_output = strstr(t, "{OUTPUT") != NULL;
    bool download = strstr(t, "{OUTPUT|O}") != NULL;

    if (ctx->stray) {
        set_err(err, err_size, "not sure what '%s' refers to; numbers are used in e.g. "
                "'retry 3 times' or 'timeout 10s'", ctx->stray);
        return false;
    }
    if (nl_count_slots(ctx, SLOT_URL) > 1) {
        set_err(err, err_size, "one URL at a time, please (got '%s' and another)",
                nl_find_slot(ctx, SLOT_URL, 0));
        return false;
    }
    const char *url = nl_find_slot(ctx, SLOT_URL, 0);
    if (url && !nl_url_ok(url)) {
        set_err(err, err_size, "refusing URL '%s': only http(s) URLs made of plain URL "
                "characters are allowed (percent-encode spaces and quotes)", url);
        return false;
    }
    static const struct { SlotType type; const char *what; } once[] = {
        { SLOT_DEST, "output file" }, { SLOT_NUM, "retry count" }, { SLOT_TIME, "timeout" },
        { SLOT_AUTHOR, "user" },
    };
    for (size_t k = 0; k < sizeof once / sizeof once[0]; k++)
        if (nl_count_slots(ctx, once[k].type) > 1) {
            set_err(err, err_size, "more than one %s given", once[k].what);
            return false;
        }
    int files = nl_count_slots(ctx, SLOT_FILE);
    int strs = nl_count_slots(ctx, SLOT_QUOTED_STR);
    const char *dest = nl_find_slot(ctx, SLOT_DEST, 0);
    if (dest && !has_output) {
        set_err(err, err_size, "saving the response ('%s') doesn't fit this command; "
                "drop it or ask to download instead", dest);
        return false;
    }
    if (download && !dest && files > 1) {
        set_err(err, err_size, "which file is the output? say e.g. 'as %s'",
                nl_find_slot(ctx, SLOT_FILE, 0));
        return false;
    }
    int body_files = download && !dest ? 0 : files;
    if (has_upload) {
        if (strs) {
            set_err(err, err_size, "an upload sends files; '%s' would be dropped", nl_find_slot(ctx, SLOT_QUOTED_STR, 0));
            return false;
        }
        for (int k = 0; k < files; k++) {
            const char *f = nl_find_slot(ctx, SLOT_FILE, k);
            if (strpbrk(f, ";,\"")) {
                set_err(err, err_size, "can't upload '%s': curl -F treats ; , and \" in file "
                        "names specially; rename it first", f);
                return false;
            }
        }
        return true;
    }
    if (!has_body && (body_files || strs)) {
        const char *v = body_files ? nl_find_slot(ctx, SLOT_FILE, 0) : nl_find_slot(ctx, SLOT_QUOTED_STR, 0);
        set_err(err, err_size, "this request doesn't send a body ('%s'); did you mean post, "
                "put or patch?", v);
        return false;
    }
    if (has_body) {
        if (files > 1 || (files && strs)) {
            set_err(err, err_size, "send one body at a time (a file or inline data)%s", "");
            return false;
        }
        for (int k = 0; strs > 1 && k < strs; k++)
            if (!curl_form_body(nl_find_slot(ctx, SLOT_QUOTED_STR, k))) {
                set_err(err, err_size, "only form fields (name=value) can be combined; "
                        "'%s' isn't one", nl_find_slot(ctx, SLOT_QUOTED_STR, k));
                return false;
            }
    }
    for (int k = 0; k < nl_count_slots(ctx, SLOT_HEADER); k++)
        if (!curl_header_ok(nl_find_slot(ctx, SLOT_HEADER, k))) {
            set_err(err, err_size, "refusing header '%s' (expected Name: value)",
                    nl_find_slot(ctx, SLOT_HEADER, k));
            return false;
        }
    const char *out = curl_output(ctx, download);
    if (download && !out && url) {
        char name[256];
        curl_remote_name(url, name, sizeof name);
        if (!name[0]) {
            set_err(err, err_size, "'%s' has no file name to save as; add one, e.g. "
                    "'as page.html'", url);
            return false;
        }
    }
    return true;
}

/* {URL} {OPTS} {OPTS|letters} {HEADERS} {BODY} {UPLOAD} {OUTPUT} {OUTPUT|O}.
 * Returns -1 if `name` is not one of them, 0 on error, 1 when expanded. */
static int curl_placeholder(const ParseContext *ctx, const char *name, size_t len, const char *rest, const char *close,
                            Out *o, char *err, size_t err_size) {
    char buf[NL_MAX_CMD_LEN];
    char quoted[NL_MAX_CMD_LEN];
    const char *arg = *rest == '|' ? rest + 1 : NULL;
    size_t arg_len = arg ? (size_t)(close - arg) : 0;
#define IS(lit) (len == sizeof lit - 1 && strncmp(name, lit, len) == 0)
#define QUOTE(v) do { if (!nl_shell_quote((v), quoted, sizeof quoted)) { o->overflow = true; return 1; } \
                      out_puts(o, quoted); } while (0)
    if (IS("URL") && *rest == '}') {
        const char *u = nl_find_slot(ctx, SLOT_URL, 0);
        if (!u || !nl_url_ok(u)) { set_err(err, err_size, "missing or unsafe URL%s", ""); return 0; }
        put_single_quoted(o, u);
        return 1;
    }
    if (IS("OPTS")) {
        /* letters in `arg` are already part of the template's own flags */
        bool has_L = arg && memchr(arg, 'L', arg_len);
        const char *u = nl_find_slot(ctx, SLOT_URL, 0);
        if (strchr(ctx->flags, 'L') && !has_L) out_puts(o, " -L");
        if (strchr(ctx->flags, 'k')) out_puts(o, " -k");
        if (strchr(ctx->flags, 'c')) out_puts(o, " --compressed");
        if (u && strpbrk(u, "[]")) out_puts(o, " -g");   /* [ ] are curl globs */
        const char *num = nl_find_slot(ctx, SLOT_NUM, 0);
        if (num) {
            if (!raw_value_ok(SLOT_NUM, num)) { set_err(err, err_size, "refusing unsafe value '%s'", num); return 0; }
            out_puts(o, " --retry ");
            out_puts(o, num);
        }
        const char *secs = nl_find_slot(ctx, SLOT_TIME, 0);
        if (secs) {
            if (!raw_value_ok(SLOT_TIME, secs)) { set_err(err, err_size, "refusing unsafe value '%s'", secs); return 0; }
            out_puts(o, " --max-time ");
            out_puts(o, secs);
        }
        const char *user = nl_find_slot(ctx, SLOT_AUTHOR, 0);
        if (user) { out_puts(o, " -u "); QUOTE(user); }
        return 1;
    }
    if (IS("HEADERS") && *rest == '}') {
        for (int k = 0; k < nl_count_slots(ctx, SLOT_HEADER); k++) {
            const char *h = nl_find_slot(ctx, SLOT_HEADER, k);
            out_puts(o, " -H ");
            if (curl_header_env(h)) { out_puts(o, "\""); out_puts(o, h); out_puts(o, "\""); }
            else put_single_quoted(o, h);
        }
        return 1;
    }
    if (IS("BODY") && *rest == '}') {
        const char *f = nl_find_slot(ctx, SLOT_FILE, 0);
        const char *s0 = nl_find_slot(ctx, SLOT_QUOTED_STR, 0);
        bool user_type = false;
        for (int k = 0; k < nl_count_slots(ctx, SLOT_HEADER); k++)
            if (strncasecmp_ascii(nl_find_slot(ctx, SLOT_HEADER, k), "content-type:", 13))
                user_type = true;
        if (f) {
            const char *type = curl_content_type(f);
            if (type && !user_type) { out_puts(o, " -H 'Content-Type: "); out_puts(o, type); out_puts(o, "'"); }
            /* -d strips newlines, which is harmless for JSON but corrupts CSV & co. */
            bool json = type && strcmp(type, "application/json") == 0;
            out_puts(o, json ? " -d " : " --data-binary ");
            snprintf(buf, sizeof buf, "@%s", f);
            QUOTE(buf);
        } else if (s0) {
            if ((s0[0] == '{' || s0[0] == '[') && !user_type)
                out_puts(o, " -H 'Content-Type: application/json'");
            for (int k = 0; k < nl_count_slots(ctx, SLOT_QUOTED_STR); k++) {
                out_puts(o, " -d ");
                put_single_quoted(o, nl_find_slot(ctx, SLOT_QUOTED_STR, k));
            }
        }
        return 1;
    }
    if (IS("UPLOAD") && *rest == '}') {
        for (int k = 0; k < nl_count_slots(ctx, SLOT_FILE); k++) {
            snprintf(buf, sizeof buf, "file=@%s", nl_find_slot(ctx, SLOT_FILE, k));
            out_puts(o, " -F ");
            put_single_quoted(o, buf);
        }
        return 1;
    }
    if (IS("OUTPUT")) {
        bool download = arg && arg_len == 1 && arg[0] == 'O';
        const char *out = curl_output(ctx, download);
        if (out) {
            size_t n = strlen(out);
            if (out[n - 1] == '/') { out_puts(o, " --output-dir "); QUOTE(out); out_puts(o, " -O"); }
            else {
                out_puts(o, " -o ");
                if (out[0] == '-') { snprintf(buf, sizeof buf, "./%s", out); out = buf; }
                QUOTE(out);
            }
        } else if (download) {
            out_puts(o, " -O");
        }
        return 1;
    }
#undef QUOTE
#undef IS
    return -1;
}

/* ------------------------------------------------------------------ */
/* magick                                                             */
/* ------------------------------------------------------------------ */

static bool is_dir_path(const char *f) {
    size_t n = strlen(f);
    return n && (f[n - 1] == '/' || strcmp(f, ".") == 0 || strcmp(f, "..") == 0 || strcmp(f, "~") == 0);
}

static bool magick_has_glob(const char *f) { return strpbrk(f, "*?[") != NULL; }

/* The output's extension (lowercased), from DEST or the target EXT. */
static void magick_out_ext(const ParseContext *ctx, char *out, size_t size) {
    out[0] = '\0';
    const char *d = nl_find_slot(ctx, SLOT_DEST, 0);
    const char *e = NULL;
    if (d && !is_dir_path(d)) {
        const char *slash = strrchr(d, '/');
        e = strrchr(slash ? slash + 1 : d, '.');
        if (e) e++;
    } else {
        int n = nl_count_slots(ctx, SLOT_EXT);
        if (n) e = nl_find_slot(ctx, SLOT_EXT, n - 1);
    }
    if (!e) return;
    size_t k = 0;
    for (; e[k] && k + 1 < size; k++) out[k] = (char)tolower((unsigned char)e[k]);
    out[k] = '\0';
}

/* How many slots of `type` the template consumes (directly or via a builder). */
static int magick_capacity(const char *t, SlotType type, int nfiles) {
    bool out_file = strstr(t, "{OUT_FILE") != NULL;
    bool out_name = strstr(t, "{OUT_NAME") != NULL;
    switch (type) {
    case SLOT_FILE:   return strstr(t, "{INPUTS}") ? nfiles : (strstr(t, "{FILE:0") || strstr(t, "{GLOB}")) ? 1 : 0;
    case SLOT_EXT:    return (strstr(t, "{GLOB}") ? 1 : 0) + (strstr(t, "{EXT:1") ? 1 : 0) +
                             ((out_file && !strstr(t, "{OUT_FILE|!")) || out_name ? 1 : 0);
    case SLOT_DEST:   return out_file || out_name || strstr(t, "{DEST:0") ? 1 : 0;
    case SLOT_RES:    return strstr(t, "{RESIZE}") || strstr(t, "{CROP}") || strstr(t, "{RES:0") ? 1 : 0;
    case SLOT_NUM:    return strstr(t, "{QUALITY}") || strstr(t, "{NUM:0") ? 1 : 0;
    case SLOT_FACTOR: return strstr(t, "{FACTOR:0") ? 1 : 0;
    case SLOT_COLOR:  return strstr(t, "{COLOR:0") ? 1 : 0;
    case SLOT_FPS:    return strstr(t, "{DELAY}") ? 1 : 0;
    default:          return 0;
    }
}

/* A path magick would read as something other than a plain file name. */
static bool magick_path_refused(const char *f, char *err, size_t err_size) {
    if (f[0] == '-') {
        set_err(err, err_size, "refusing file name '%s' (it would be read as an option); "
                "write it as ./name", f);
        return true;
    }
    if (f[0] == '@') {
        set_err(err, err_size, "refusing '%s': magick reads @file as a list of file names", f);
        return true;
    }
    /* "png:out", "msl:x", "https://...": magick format / coder prefixes */
    size_t n = 0;
    while (isalnum((unsigned char)f[n])) n++;
    if (n > 0 && f[n] == ':') {
        set_err(err, err_size, "refusing '%s': magick reads 'xyz:' as a format prefix; "
                "write it as ./name", f);
        return true;
    }
    if (strchr(f, '\n') || strchr(f, '\r')) {
        set_err(err, err_size, "refusing file name with a line break%s", "");
        return true;
    }
    return false;
}

/* Refuse requests the chosen template can't fully express. */
static bool magick_check(const ParseContext *ctx, const CommandTemplate *tpl,
                         char *err, size_t err_size) {
    const char *t = tpl->flag_template;
    char ext[16];

    if (ctx->stray) {
        set_err(err, err_size, "not sure what '%s' refers to; numbers are used in e.g. "
                "'width 800', '50%%', 'quality 85', '90 degrees', 'fuzz 10%%' or '10px border'",
                ctx->stray);
        return false;
    }
    if (nl_count_slots(ctx, SLOT_QUOTED_STR)) {
        set_err(err, err_size, "not sure what '%s' is; only file names and colours are "
                "understood in quotes", nl_find_slot(ctx, SLOT_QUOTED_STR, 0));
        return false;
    }
    static const struct { SlotType type; const char *what; } once[] = {
        { SLOT_RES, "size" }, { SLOT_NUM, "quality" }, { SLOT_FACTOR, "amount" },
        { SLOT_COLOR, "colour" }, { SLOT_DEST, "output" }, { SLOT_FPS, "frame rate" },
    };
    for (size_t k = 0; k < sizeof once / sizeof once[0]; k++)
        if (nl_count_slots(ctx, once[k].type) > 1) {
            set_err(err, err_size, "more than one %s given", once[k].what);
            return false;
        }

    int nfiles = nl_count_slots(ctx, SLOT_FILE);
    static const SlotType kinds[] = {
        SLOT_FILE, SLOT_EXT, SLOT_DEST, SLOT_RES, SLOT_NUM, SLOT_FACTOR, SLOT_COLOR, SLOT_FPS,
    };
    for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        int have = nl_count_slots(ctx, kinds[k]);
        int cap = magick_capacity(t, kinds[k], nfiles);
        if (have <= cap) continue;
        const char *v = nl_find_slot(ctx, kinds[k], cap);
        if (kinds[k] == SLOT_FILE && cap == 1) {
            set_err(err, err_size, "this works on one image at a time (got '%s' too); for a "
                    "folder say e.g. 'all png files in dir/'", v);
            return false;
        }
        if (kinds[k] == SLOT_EXT && cap >= 1) {
            set_err(err, err_size, "pick one output format (got '%s' too)", v);
            return false;
        }
        static const char *const what[SLOT_TYPE_COUNT] = {
            [SLOT_FILE] = "an input image", [SLOT_EXT] = "an output format",
            [SLOT_DEST] = "an output name", [SLOT_RES] = "a size", [SLOT_NUM] = "a quality",
            [SLOT_FACTOR] = "an amount", [SLOT_COLOR] = "a colour", [SLOT_FPS] = "a frame rate",
        };
        if (err && err_size)
            snprintf(err, err_size, "%s ('%s') isn't supported for this command",
                     what[kinds[k]], v);
        return false;
    }

    if ((strchr(ctx->flags, '!') || strchr(ctx->flags, '>')) && !strstr(t, "{RESIZE}")) {
        set_err(err, err_size, "'exactly' and 'only shrink' only apply when resizing%s", "");
        return false;
    }
    if (strchr(ctx->flags, '!') && strchr(ctx->flags, '>')) {
        set_err(err, err_size, "pick either 'exactly' or 'only shrink'%s", "");
        return false;
    }

    const char *num = nl_find_slot(ctx, SLOT_NUM, 0);
    if (num && (!all_digits_str(num) || strlen(num) > 3 || atoi_safe(num) < 1 || atoi_safe(num) > 100)) {
        set_err(err, err_size, "quality must be between 1 and 100 (got %s)", num);
        return false;
    }
    const char *amt = nl_find_slot(ctx, SLOT_FACTOR, 0);
    if (amt) {
        int v = all_digits_str(amt) && strlen(amt) <= 5 ? atoi_safe(amt) : -1;
        int max = strstr(t, "-fuzz") ? 100 : strstr(t, "-rotate") ? 360 : 1000;
        if (v < 0 || v > max) {
            if (err && err_size) snprintf(err, err_size, "'%s' is out of range here (0-%d)", amt, max);
            return false;
        }
    }
    const char *fps = nl_find_slot(ctx, SLOT_FPS, 0);
    if (fps && (!all_digits_str(fps) || strlen(fps) > 3 || atoi_safe(fps) < 1 || atoi_safe(fps) > 100)) {
        set_err(err, err_size, "frame rate must be between 1 and 100 fps (got %s)", fps);
        return false;
    }
    const char *color = nl_find_slot(ctx, SLOT_COLOR, 0);
    if (color) {
        for (const char *p = color; *p; p++)
            if (!(isalnum((unsigned char)*p) || (*p == '#' && p == color))) {
                set_err(err, err_size, "refusing colour '%s'", color);
                return false;
            }
    }

    for (int k = 0; k < nfiles; k++)
        if (magick_path_refused(nl_find_slot(ctx, SLOT_FILE, k), err, err_size)) return false;
    const char *dest = nl_find_slot(ctx, SLOT_DEST, 0);
    if (dest && magick_path_refused(dest, err, err_size)) return false;

    bool multi = strstr(t, "{INPUTS}") != NULL;
    bool glob = strstr(t, "{GLOB}") != NULL;
    const char *in = nl_find_slot(ctx, SLOT_FILE, 0);
    if (!multi && !glob && in) {
        if (is_dir_path(in)) {
            if (err && err_size)
                snprintf(err, err_size, "'%s' is a folder; for every image in it say e.g. "
                         "'all png files in %s'", in, in);
            return false;
        }
        if (magick_has_glob(in)) {
            set_err(err, err_size, "'%s' matches several files; for a batch say e.g. "
                    "'all png files in dir/'", in);
            return false;
        }
    }
    if (multi) {
        bool any_glob = false;
        for (int k = 0; k < nfiles; k++)
            if (magick_has_glob(nl_find_slot(ctx, SLOT_FILE, k))) any_glob = true;
        if (nfiles < 2 && !any_glob) {
            set_err(err, err_size, "this needs several images (e.g. a.png b.png, or "
                    "'frames/*.png'), got only '%s'", in);
            return false;
        }
    }
    if (glob) {
        if (in && !is_dir_path(in) && (looks_like_regular_file(in) || magick_has_glob(in))) {
            set_err(err, err_size, "'%s' isn't a folder; say e.g. 'all png files in photos/'", in);
            return false;
        }
        const char *src = nl_find_slot(ctx, SLOT_EXT, 0);
        const char *dst = nl_find_slot(ctx, SLOT_EXT, 1);
        if (src && dst && strcmp(src, dst) == 0) {
            set_err(err, err_size, "those images are already %s", src);
            return false;
        }
        if (strstr(t, "{DEST:0") && dest && !is_dir_path(dest)) {
            if (looks_like_regular_file(dest)) {
                set_err(err, err_size, "'%s' should be a folder (e.g. into small/)", dest);
                return false;
            }
        }
    }
    if (!glob && strstr(t, "{DEST:0") == NULL && dest && is_dir_path(dest) && !strstr(t, "{OUT_")) {
        set_err(err, err_size, "an output folder ('%s') doesn't fit this command", dest);
        return false;
    }

    /* formats that can't hold what the command makes */
    magick_out_ext(ctx, ext, sizeof ext);
    if (strstr(t, "-transparent") || strstr(t, "-background none")) {
        static const char *const opaque[] = { "jpg", "jpeg", "bmp", "ppm", NULL };
        for (size_t k = 0; opaque[k]; k++)
            if (strcmp(ext, opaque[k]) == 0) {
                set_err(err, err_size, "%s can't store transparency; use png or webp", ext);
                return false;
            }
    }
    if (strstr(t, "{CROP}") || strstr(t, "-extent")) {
        const char *r = nl_find_slot(ctx, SLOT_RES, 0);
        const char *x = r ? strchr(r, 'x') : NULL;
        bool both = x && x != r && x[1];
        if (r && !both && !(strstr(t, "{CROP}") && strchr(r, '%'))) {
            set_err(err, err_size, "this needs a width and a height (e.g. 800x600), got '%s'", r);
            return false;
        }
    }
    return true;
}

/* {RESIZE} {CROP} {QUALITY} {INPUTS} {GLOB} {DELAY} {OUT_NAME|stem|ext}. Returns -1
 * if `name` is not one of them, 0 on error, 1 when expanded. */
static int magick_placeholder(const ParseContext *ctx, const char *name, size_t len,
                              const char *rest, const char *close, Out *o,
                              char *err, size_t err_size) {
    char buf[NL_MAX_CMD_LEN];
    char quoted[NL_MAX_CMD_LEN];
#define IS(lit) (len == sizeof lit - 1 && strncmp(name, lit, len) == 0)
#define QUOTE(v) do { if (!nl_shell_quote((v), quoted, sizeof quoted)) { o->overflow = true; return 1; } \
                      out_puts(o, quoted); } while (0)
    if (IS("RESIZE") && *rest == '}') {
        const char *r = nl_find_slot(ctx, SLOT_RES, 0);
        if (!r) return 1;
        if (!magick_geom_ok(r)) { set_err(err, err_size, "refusing unsafe value '%s'", r); return 0; }
        out_puts(o, " -resize ");
        char mod = strchr(ctx->flags, '!') ? '!' : strchr(ctx->flags, '>') ? '>' : 0;
        if (mod) {
            snprintf(buf, sizeof buf, "%s%c", r, mod);
            put_single_quoted(o, buf);
        } else {
            out_puts(o, r);
        }
        return 1;
    }
    if (IS("CROP") && *rest == '}') {
        const char *r = nl_find_slot(ctx, SLOT_RES, 0);
        if (!r || !magick_geom_ok(r)) { set_err(err, err_size, "missing or unsafe size%s", ""); return 0; }
        if (strchr(r, '%')) snprintf(buf, sizeof buf, "%sx%s+0+0", r, r);
        else snprintf(buf, sizeof buf, "%s+0+0", r);
        out_puts(o, buf);
        return 1;
    }
    if (IS("QUALITY") && *rest == '}') {
        const char *q = nl_find_slot(ctx, SLOT_NUM, 0);
        if (!q) return 1;
        if (!all_digits_str(q)) { set_err(err, err_size, "refusing unsafe value '%s'", q); return 0; }
        out_puts(o, " -quality ");
        out_puts(o, q);
        return 1;
    }
    if (IS("INPUTS") && *rest == '}') {
        for (int k = 0; k < nl_count_slots(ctx, SLOT_FILE); k++) {
            out_puts(o, " ");
            QUOTE(nl_find_slot(ctx, SLOT_FILE, k));
        }
        return 1;
    }
    if (IS("DELAY") && *rest == '}') {
        const char *f = nl_find_slot(ctx, SLOT_FPS, 0);
        int fps = f && all_digits_str(f) ? atoi_safe(f) : 10;
        if (fps < 1) fps = 1;
        int delay = (100 + fps / 2) / fps;
        snprintf(buf, sizeof buf, " -delay %d", delay < 1 ? 1 : delay);
        out_puts(o, buf);
        return 1;
    }
    if (IS("GLOB") && *rest == '}') {
        const char *e = nl_find_slot(ctx, SLOT_EXT, 0);
        const char *d = nl_find_slot(ctx, SLOT_FILE, 0);
        if (!e) { set_err(err, err_size, "say which images, e.g. 'all png files'%s", ""); return 0; }
        for (const char *p = e; *p; p++)
            if (!isalnum((unsigned char)*p)) { set_err(err, err_size, "refusing unsafe value '%s'", e); return 0; }
        /* "~/photos/": keep ~/ outside the quotes so the shell expands it */
        if (d && d[0] == '~' && (d[1] == '/' || d[1] == '\0')) { out_puts(o, "~/"); d += d[1] ? 2 : 1; }
        size_t dl = d ? strlen(d) : 0;
        snprintf(buf, sizeof buf, "%s%s*.%s", d ? d : "", dl && d[dl - 1] != '/' ? "/" : "", e);
        put_single_quoted(o, buf);
        return 1;
    }
    if (IS("OUT_NAME") && *rest == '|') {
        const char *stem = rest + 1;
        const char *bar = memchr(stem, '|', (size_t)(close - stem));
        if (!bar) return -1;
        const char *def_ext = bar + 1;
        const char *d = nl_find_slot(ctx, SLOT_DEST, 0);
        int n_ext = nl_count_slots(ctx, SLOT_EXT);
        const char *e = n_ext ? nl_find_slot(ctx, SLOT_EXT, n_ext - 1) : NULL;
        if (d && !is_dir_path(d)) {
            snprintf(buf, sizeof buf, "%s", d);
        } else if (e) {
            snprintf(buf, sizeof buf, "%s%.*s.%s", d ? d : "", (int)(bar - stem), stem, e);
        } else {
            snprintf(buf, sizeof buf, "%s%.*s.%.*s", d ? d : "", (int)(bar - stem), stem,
                     (int)(close - def_ext), def_ext);
        }
        QUOTE(buf);
        return 1;
    }
#undef QUOTE
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

        /* ---- tar builders ---- */
        if (ctx->tool == TOOL_TAR) {
            int r = tar_placeholder(ctx, name, name_len, rest, o, err, err_size);
            if (r == 0) return false;
            if (r == 1) { p = close + 1; continue; }
        }

        /* ---- git builders ---- */
        if (ctx->tool == TOOL_GIT) {
            int r = git_placeholder(ctx, name, name_len, rest, close, o, placeholders, all_user,
                                    err, err_size);
            if (r == 0) return false;
            if (r == 1) { p = close + 1; continue; }
        }

        /* ---- curl builders ---- */
        if (ctx->tool == TOOL_CURL) {
            int r = curl_placeholder(ctx, name, name_len, rest, close, o, err, err_size);
            if (r == 0) return false;
            if (r == 1) { p = close + 1; continue; }
        }

        /* ---- magick builders ---- */
        if (ctx->tool == TOOL_MAGICK) {
            int r = magick_placeholder(ctx, name, name_len, rest, close, o, err, err_size);
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

bool nl_expand(const ParseContext *ctx, const CommandTemplate *tpl,
               char *out, size_t out_size, char *err, size_t err_size) {
    Out o = { out, out_size, 0, false };
    if (out_size) out[0] = '\0';
    if (err && err_size) err[0] = '\0';

    int placeholders = 0;
    bool all_user = true;
    const char *t = tpl->flag_template;

    if (ctx->tool == TOOL_FIND && !find_check(ctx, tpl, err, err_size)) return false;
    if (ctx->tool == TOOL_TAR && !tar_check(ctx, tpl, err, err_size)) return false;
    if (ctx->tool == TOOL_GIT && !git_check(ctx, tpl, err, err_size)) return false;
    if (ctx->tool == TOOL_CURL && !curl_check(ctx, tpl, err, err_size)) return false;
    if (ctx->tool == TOOL_MAGICK && !magick_check(ctx, tpl, err, err_size)) return false;

    /* magick won't create an output folder ("into thumbs/") */
    if (ctx->tool == TOOL_MAGICK && (strstr(t, "{OUT_FILE") || strstr(t, "{OUT_NAME"))) {
        const char *d = nl_find_slot(ctx, SLOT_DEST, 0);
        if (d && is_dir_path(d) && strcmp(d, ".") != 0 && strcmp(d, "./") != 0) {
            char quoted[NL_MAX_CMD_LEN];
            if (!nl_shell_quote(d, quoted, sizeof quoted)) { set_err(err, err_size, "command too long%s", ""); return false; }
            out_puts(&o, "mkdir -p ");
            out_puts(&o, quoted);
            out_puts(&o, " && ");
        }
    }

    if (!expand_range(ctx, t, t + strlen(t), &o, false, &placeholders, &all_user, err, err_size))
        return false;
    if (o.overflow) { set_err(err, err_size, "command too long%s", ""); return false; }

    while (o.len > 0 && out[o.len - 1] == ' ') out[--o.len] = '\0';
    return true;
}
