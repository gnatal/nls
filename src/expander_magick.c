/*
 * expander_magick.c — magick: refusals, output folders and the {RESIZE} {CROP} {QUALITY} {INPUTS} {GLOB} {DELAY} {OUT_NAME} builders.
 */
#include "expander_int.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

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

/* magick won't create an output folder ("into thumbs/") */
static bool magick_prologue(const ParseContext *ctx, const CommandTemplate *tpl, Out *o,
                            char *err, size_t err_size) {
    const char *t = tpl->flag_template;
    if (!strstr(t, "{OUT_FILE") && !strstr(t, "{OUT_NAME")) return true;
    const char *d = nl_find_slot(ctx, SLOT_DEST, 0);
    if (d && is_dir_path(d) && strcmp(d, ".") != 0 && strcmp(d, "./") != 0) {
        char quoted[NL_MAX_CMD_LEN];
        if (!nl_shell_quote(d, quoted, sizeof quoted)) { set_err(err, err_size, "command too long%s", ""); return false; }
        out_puts(o, "mkdir -p ");
        out_puts(o, quoted);
        out_puts(o, " && ");
    }
    return true;
}

static int magick_ph(const ParseContext *ctx, const Placeholder *ph, Out *o,
                     char *err, size_t err_size) {
    return magick_placeholder(ctx, ph->name, ph->len, ph->rest, ph->close, o, err, err_size);
}

const ToolExpander nl_expander_magick = { magick_check, magick_prologue, magick_ph };
