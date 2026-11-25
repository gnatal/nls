/*
 * lexer_magick.c — magick: ImageMagick: resize, convert, crop, effects, animations.
 */
#include "lexer_int.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    bool resizing;      /* resize / scale / thumbnail ...: "to 800" is a width   */
    bool amount;        /* rotate / blur / sharpen / border: a bare number is its amount */
    int target_ext;     /* slot index of an EXT given after "to"/"as", or -1     */
} MagickState;

/* Image format words. Plurals ("pngs") are accepted by magick_format(). */
static const char *const MAGICK_EXTS[] = {
    "png", "jpg", "jpeg", "webp", "avif", "gif", "bmp", "tif", "tiff", "ico", "heic", "heif",
    "svg", "pdf", "jxl", "psd", "tga", "ppm", NULL
};

static const char *const MAGICK_COLORS[] = {
    "white", "black", "red", "green", "blue", "yellow", "orange", "purple", "pink", "magenta",
    "cyan", "brown", "navy", "teal", "lime", "maroon", "olive", "silver", "gold", "beige", NULL
};

static const char *const MAGICK_RESIZE_VERBS[] = {
    "resize", "resized", "resizing", "scale", "scaled", "scaling", "shrink", "downscale",
    "upscale", "enlarge", "thumbnail", "thumbnails", "thumb", "crop", "pad", "fit", "smaller",
    "bigger", "larger", NULL
};

static const char *const MAGICK_AMOUNT_VERBS[] = {
    "rotate", "rotated", "rotating", "turn", "blur", "blurred", "blurry", "sharpen", "sharper",
    "border", "frame", NULL
};

/* "to 800", "at 640": a bare number after these is a width while resizing */
static const char *const MAGICK_RES_LEADERS[] = {
    "to", "at", "resize", "resized", "scale", "scaled", "max", "maximum", "within", "fit",
    "thumbnail", "downscale", "upscale", "shrink", "crop", "pad", NULL
};

static const char *const MAGICK_DEST_LEADERS[] = {
    "into", "as", "named", "called", "-o", NULL
};

static const char *const MAGICK_AMOUNT_UNITS[] = {
    "degrees", "degree", "deg", "\xc2\xb0", "px", "pixels", "pixel", NULL
};

static const char *const MAGICK_FILLER[] = {
    "image", "images", "picture", "pictures", "photo", "photos", "pic", "pics", "img",
    "px", "pixels", "keep", "keeping", "aspect", "ratio", "proportions", "magick", NULL
};

/* "png", "JPEG", "pngs" -> canonical extension, or NULL. */
static const char *magick_format(const char *lw) {
    static char buf[8];
    size_t n = strlen(lw);
    if (n == 0 || n >= sizeof buf) return NULL;
    memcpy(buf, lw, n + 1);
    if (!in_list(buf, MAGICK_EXTS) && n > 3 && buf[n - 1] == 's') buf[n - 1] = '\0';
    if (!in_list(buf, MAGICK_EXTS)) return NULL;
    return strcmp(buf, "jpeg") == 0 ? "jpg" : buf;
}

/* #rgb, #rgba, #rrggbb, #rrggbbaa */
static bool magick_hex_color(const char *s) {
    if (s[0] != '#') return false;
    size_t n = 0;
    while (isxdigit((unsigned char)s[1 + n])) n++;
    return s[1 + n] == '\0' && (n == 3 || n == 4 || n == 6 || n == 8);
}

/* "800x600", "800x", "x600" (not "2x": that's a scale factor) */
static bool magick_wxh(const char *lw) {
    const char *x = strchr(lw, 'x');
    if (!x || strchr(x + 1, 'x')) return false;
    size_t a = (size_t)(x - lw), b = strlen(x + 1);
    if (a > 5 || b > 5 || (a == 0 && b == 0)) return false;
    if (!all_digits(lw, a) && a) return false;
    if (!all_digits(x + 1, b) && b) return false;
    if (b == 0 && atoi(lw) < 16) return false;
    return true;
}

/* A whole number 1..99999 at `s`, optionally followed by `suffix` chars
 * (e.g. "%", "px"). Writes the digits to `out`. */
static bool magick_int(const char *s, const char *const *suffixes, char *out, size_t size) {
    size_t n = number_prefix(s, false);
    if (n == 0 || n > 7 || n >= size) return false;
    const char *rest = s + n;
    if (*rest && !(suffixes && in_list(rest, suffixes))) return false;
    memcpy(out, s, n);
    out[n] = '\0';
    return true;
}

static void magick_add_path(ParseContext *ctx, const RawTok *toks, size_t i, MagickState *st) {
    (void)st;
    size_t at = 0;
    const char *pw = prev_word(toks, i, &at);
    if (pw && in_list(pw, FIND_DIR_NOUNS)) pw = prev_word(toks, at, &at);
    /* "convert in.png to out.webp": "to" names the output once there is an input;
     * "add a border to photo.jpg" is the input itself */
    bool dest = pw && (in_list(pw, MAGICK_DEST_LEADERS) ||
                       (strcmp(pw, "to") == 0 && nl_count_slots(ctx, SLOT_FILE) > 0));
    add_slot(ctx, dest ? SLOT_DEST : SLOT_FILE, toks[i].text);
    /* "combine a.png b.png into anim.gif": several images into one .gif is an
     * animation, not a strip (unless "side by side" etc. outweighs it) */
    const char *dext = dest ? extension_of(toks[i].text) : NULL;
    if (dext && strcasecmp(dext, "gif") == 0 && nl_count_slots(ctx, SLOT_FILE) >= 2) {
        add_intent(ctx, "gif");
        add_intent(ctx, "animated");
    }
    /* A glob ("frames/\*.png") is several images: favours animations and strips */
    if (!dest && strpbrk(toks[i].text, "*?")) add_intent(ctx, "frames");
}

/* Classify toks[i] in magick mode. Returns the number of tokens consumed
 * (>= 1), or 0 if the token should fall through to intent handling. */
static size_t classify_magick(ParseContext *ctx, const RawTok *toks, size_t count, size_t i,
                              MagickState *st) {
    const char *t = toks[i].text;
    const char *lw = toks[i].lower;
    const char *next = i + 1 < count && !toks[i + 1].quoted ? toks[i + 1].lower : NULL;
    const char *next2 = i + 2 < count && !toks[i + 2].quoted ? toks[i + 2].lower : NULL;
    const char *prev = i > 0 && !toks[i - 1].quoted ? toks[i - 1].lower : NULL;
    static const char *const pct[] = { "%", NULL };
    static const char *const px[] = { "px", NULL };
    char num[16], buf[32];

    /* 1. Quoted: a path, a colour, or text nls has no use for */
    if (toks[i].quoted) {
        if (looks_like_file(t, TOOL_GREP)) magick_add_path(ctx, toks, i, st);
        else if (magick_hex_color(t) || in_list(lw, MAGICK_COLORS)) add_slot(ctx, SLOT_COLOR, lw);
        else add_slot(ctx, SLOT_QUOTED_STR, t);
        return 1;
    }

    /* 2. "black and white", "b&w" */
    if (strcmp(lw, "black") == 0 && next && next2 && (strcmp(next, "and") == 0 || strcmp(next, "&") == 0) &&
        strcmp(next2, "white") == 0) {
        add_intent(ctx, "grayscale");
        return 3;
    }
    if (strcmp(lw, "b&w") == 0 || strcmp(lw, "bw") == 0) { add_intent(ctx, "grayscale"); return 1; }
    /* "remove the transparency", "no alpha channel": flattening, not keying out */
    {
        static const char *const neg[] = { "remove", "drop", "strip", "without", "no", "kill", NULL };
        size_t k = i + 1;
        if (in_list(lw, neg)) {
            if (k < count && strcmp(toks[k].lower, "the") == 0) k++;
            if (k < count && !toks[k].quoted &&
                (strcmp(toks[k].lower, "transparency") == 0 || strcmp(toks[k].lower, "alpha") == 0)) {
                add_intent(ctx, "flatten");
                k++;
                if (k < count && strcmp(toks[k].lower, "channel") == 0) k++;
                return k - i;
            }
        }
    }

    /* 3. Formats: ".webp", "webp", "pngs" (the target if after "to"/"as") */
    {
        const char *f = lw[0] == '.' ? magick_format(lw + 1) : magick_format(lw);
        if (f && (lw[0] == '.' || !strchr(lw, '.'))) {
            /* "... to gif" names the output format; only "make a gif" is an intent */
            bool target = prev && (strcmp(prev, "to") == 0 || strcmp(prev, "as") == 0 ||
                                   strcmp(prev, "into") == 0);
            if (target) st->target_ext = ctx->slot_count;
            char ext[8];
            snprintf(ext, sizeof ext, "%s", f);
            add_slot(ctx, SLOT_EXT, ext);
            if (!target) add_intent(ctx, ext);
            return 1;
        }
    }

    /* 4. Colours: "white", "#ff8800", "gray background" */
    if (magick_hex_color(lw) || in_list(lw, MAGICK_COLORS) ||
        ((strcmp(lw, "gray") == 0 || strcmp(lw, "grey") == 0) && next &&
         (strcmp(next, "background") == 0 || strcmp(next, "border") == 0 || strcmp(next, "bg") == 0))) {
        add_slot(ctx, SLOT_COLOR, lw);
        return 1;
    }

    /* 5. Quality: "quality 85", "quality of 85%", "85% quality", "-quality 85" */
    if (strcmp(lw, "quality") == 0 || strcmp(lw, "-quality") == 0) {
        size_t k = i + 1;
        if (k < count && (strcmp(toks[k].lower, "of") == 0 || strcmp(toks[k].lower, "=") == 0 ||
                          strcmp(toks[k].lower, "at") == 0))
            k++;
        if (k < count && !toks[k].quoted && magick_int(toks[k].lower, pct, num, sizeof num)) {
            add_slot(ctx, SLOT_NUM, num);
            return k + 1 - i;
        }
    }
    if (next && strcmp(next, "quality") == 0 && magick_int(lw, pct, num, sizeof num)) {
        add_slot(ctx, SLOT_NUM, num);
        return 2;
    }

    /* 6. Fuzz: "fuzz 10%", "tolerance of 10%", "10% fuzz" */
    if (strcmp(lw, "fuzz") == 0 || strcmp(lw, "-fuzz") == 0 || strcmp(lw, "tolerance") == 0 ||
        strcmp(lw, "fuzziness") == 0) {
        size_t k = i + 1;
        if (k < count && strcmp(toks[k].lower, "of") == 0) k++;
        if (k < count && !toks[k].quoted && magick_int(toks[k].lower, pct, num, sizeof num)) {
            add_slot(ctx, SLOT_FACTOR, num);
            return k + 1 - i;
        }
    }
    if (next && (strcmp(next, "fuzz") == 0 || strcmp(next, "tolerance") == 0) &&
        magick_int(lw, pct, num, sizeof num)) {
        add_slot(ctx, SLOT_FACTOR, num);
        return 2;
    }

    /* 7. Frame rate (animations): "10fps", "10 fps", "fps 10" */
    if (strcmp(lw, "fps") == 0 && next && magick_int(next, NULL, num, sizeof num)) {
        add_slot(ctx, SLOT_FPS, num);
        return 2;
    }
    {
        static const char *const fps[] = { "fps", NULL };
        if (magick_int(lw, fps, num, sizeof num) && strstr(lw, "fps")) { add_slot(ctx, SLOT_FPS, num); return 1; }
        if (next && strcmp(next, "fps") == 0 && magick_int(lw, NULL, num, sizeof num)) {
            add_slot(ctx, SLOT_FPS, num);
            return 2;
        }
    }

    /* 8. Geometry */
    if (magick_wxh(lw)) { add_slot(ctx, SLOT_RES, lw); return 1; }
    /* "width 800", "width of 800px", "height 600" */
    if (strcmp(lw, "width") == 0 || strcmp(lw, "height") == 0) {
        size_t k = i + 1;
        if (k < count && (strcmp(toks[k].lower, "of") == 0 || strcmp(toks[k].lower, "=") == 0)) k++;
        if (k < count && !toks[k].quoted && magick_int(toks[k].lower, px, num, sizeof num)) {
            snprintf(buf, sizeof buf, lw[0] == 'w' ? "%sx" : "x%s", num);
            add_slot(ctx, SLOT_RES, buf);
            return k + 1 - i;
        }
    }
    /* "800 wide", "800px wide", "600 pixels tall" */
    if (magick_int(lw, px, num, sizeof num)) {
        size_t k = i + 1;
        if (k < count && (strcmp(toks[k].lower, "px") == 0 || strcmp(toks[k].lower, "pixels") == 0)) k++;
        if (k < count && !toks[k].quoted) {
            const char *w = toks[k].lower;
            bool wide = strcmp(w, "wide") == 0 || strcmp(w, "width") == 0;
            bool tall = strcmp(w, "tall") == 0 || strcmp(w, "high") == 0 || strcmp(w, "height") == 0;
            if (wide || tall) {
                snprintf(buf, sizeof buf, wide ? "%sx" : "x%s", num);
                add_slot(ctx, SLOT_RES, buf);
                return k + 1 - i;
            }
        }
    }
    /* "50%", "50 percent" (fuzz and quality were handled above) */
    if (magick_int(lw, pct, num, sizeof num) &&
        (strchr(lw, '%') || (next && (strcmp(next, "percent") == 0 || strcmp(next, "pct") == 0)))) {
        snprintf(buf, sizeof buf, "%s%%", num);
        add_slot(ctx, SLOT_RES, buf);
        return strchr(lw, '%') ? 1 : 2;
    }
    /* "2x", "1.5x", "0.5x": a scale factor */
    {
        size_t n = number_prefix(lw, false);
        if (n && strcmp(lw + n, "x") == 0) {
            double f = atof(lw);
            if (f > 0 && f <= 16) {
                snprintf(buf, sizeof buf, "%.10g%%", f * 100);
                add_slot(ctx, SLOT_RES, buf);
                return 1;
            }
        }
    }
    if (st->resizing && (strcmp(lw, "half") == 0 || strcmp(lw, "quarter") == 0)) {
        add_slot(ctx, SLOT_RES, lw[0] == 'h' ? "50%" : "25%");
        return next && strcmp(next, "size") == 0 ? 2 : 1;
    }
    /* "resize to 800", "scale to 640px": a width */
    if (st->resizing && prev && in_list(prev, MAGICK_RES_LEADERS) &&
        magick_int(lw, px, num, sizeof num) && !(next && in_list(next, MAGICK_AMOUNT_UNITS) &&
                                                 strcmp(next, "px") != 0 && strcmp(next, "pixels") != 0)) {
        add_slot(ctx, SLOT_RES, num);
        return next && (strcmp(next, "px") == 0 || strcmp(next, "pixels") == 0) ? 2 : 1;
    }

    /* 9. Resize modifiers: "exactly", "ignoring aspect ratio", "only shrink" */
    if (strcmp(lw, "exactly") == 0 || strcmp(lw, "exact") == 0 || strcmp(lw, "stretch") == 0 ||
        strcmp(lw, "stretched") == 0 || strcmp(lw, "force") == 0 || strcmp(lw, "forced") == 0) {
        add_flag(ctx, '!');
        return 1;
    }
    if ((strcmp(lw, "ignore") == 0 || strcmp(lw, "ignoring") == 0 || strcmp(lw, "without") == 0 ||
         strcmp(lw, "disregard") == 0) && next) {
        size_t k = i + 1;
        if (strcmp(toks[k].lower, "keeping") == 0 || strcmp(toks[k].lower, "the") == 0) k++;
        if (k < count && strcmp(toks[k].lower, "aspect") == 0) {
            add_flag(ctx, '!');
            return k + 1 < count && strcmp(toks[k + 1].lower, "ratio") == 0 ? k + 2 - i : k + 1 - i;
        }
    }
    if (strcmp(lw, "only") == 0 && next &&
        (strcmp(next, "shrink") == 0 || strcmp(next, "downscale") == 0 || strcmp(next, "larger") == 0 ||
         strcmp(next, "bigger") == 0 || (strcmp(next, "if") == 0 && next2 &&
                                         (strcmp(next2, "larger") == 0 || strcmp(next2, "bigger") == 0)))) {
        add_flag(ctx, '>');
        add_intent(ctx, "shrink");
        return strcmp(next, "if") == 0 ? 3 : 2;
    }
    if ((strcmp(lw, "never") == 0 || strcmp(lw, "don't") == 0 || strcmp(lw, "dont") == 0 ||
         strcmp(lw, "no") == 0 || strcmp(lw, "without") == 0) && next &&
        (strncmp(next, "enlarg", 6) == 0 || strncmp(next, "upscal", 6) == 0)) {
        add_flag(ctx, '>');
        return 2;
    }

    /* 10. Amounts: "90 degrees", "10px border", "by 5", or a bare number
     *     after rotate / blur / border */
    {
        static const char *const units[] = { "\xc2\xb0", "deg", "px", NULL };
        if (magick_int(lw, units, num, sizeof num)) {
            bool unit = next && in_list(next, MAGICK_AMOUNT_UNITS);
            if (unit || st->amount || lw[strlen(num)]) {
                add_slot(ctx, SLOT_FACTOR, num);
                return unit ? 2 : 1;
            }
        }
    }

    /* 11. Numbers mean nothing else */
    if (is_number(lw, true)) {
        if (!ctx->stray) ctx->stray = arena_dup(ctx, t);
        return 1;
    }

    /* 12. Paths, file names and globs ("frames/\*.png") */
    if (strchr(t, '/') || looks_like_file(t, TOOL_GREP)) {
        magick_add_path(ctx, toks, i, st);
        return 1;
    }

    /* 13. Glue words carry no intent */
    if (in_list(lw, MAGICK_FILLER) && !is_keyword(TOOL_MAGICK, lw)) return 1;
    return 0;
}

/* Put the target format ("... to webp") after the source format ("all pngs")
 * so templates can use {EXT:0} as the source and the last EXT as the target. */
static void magick_order_exts(ParseContext *ctx, int target) {
    if (target < 0) return;
    int last = -1;
    for (size_t k = 0; k < ctx->slot_count; k++)
        if (ctx->slots[k].type == SLOT_EXT) last = (int)k;
    if (last <= target) return;
    const char *v = ctx->slots[target].value;
    for (size_t k = (size_t)target + 1; k <= (size_t)last; k++) {
        if (ctx->slots[k].type != SLOT_EXT) continue;
        ctx->slots[target].value = ctx->slots[k].value;
        target = (int)k;
    }
    ctx->slots[last].value = v;
}

void nl_lex_magick(ParseContext *ctx, const RawTok *toks, size_t count, const JsonInfo *json) {
    (void)json;
    MagickState mst = { false, false, -1 };
    for (size_t i = 0; i < count; i++) {
        if (toks[i].quoted) continue;
        if (in_list(toks[i].lower, MAGICK_RESIZE_VERBS)) mst.resizing = true;
        if (in_list(toks[i].lower, MAGICK_AMOUNT_VERBS)) mst.amount = true;
    }

    for (size_t i = 0; i < count && !ctx->overflow; i++) {
        size_t used = classify_magick(ctx, toks, count, i, &mst);
        if (used) { i += used - 1; continue; }
        lex_intent(ctx, toks[i].lower);
    }
    magick_order_exts(ctx, mst.target_ext);
}
