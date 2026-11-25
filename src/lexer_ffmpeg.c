/*
 * lexer_ffmpeg.c — ffmpeg: times, resolutions, bitrates, frame rates and formats.
 */
#include "lexer_int.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const SEC_UNITS[]  = { "s", "sec", "secs", "second", "seconds", NULL };

static const char *const MIN_UNITS[]  = { "min", "mins", "minute", "minutes", NULL };

static const char *const HOUR_UNITS[] = { "h", "hr", "hrs", "hour", "hours", NULL };

static const char *const STD_HEIGHTS[] = {
    "144", "240", "360", "480", "540", "720", "1080", "1440", "2160", NULL
};

/* [H]H:MM[:SS][.frac] */
static bool is_timestamp(const char *s) {
    size_t i = 0, groups = 0;
    while (s[i]) {
        size_t start = i;
        while (isdigit((unsigned char)s[i])) i++;
        size_t len = i - start;
        if (len == 0) return false;
        if (groups > 0 && len != 2) return false;
        if (groups == 0 && len > 3) return false;
        groups++;
        if (s[i] == ':') { i++; continue; }
        if (s[i] == '.' && groups >= 2) {
            i++;
            if (!isdigit((unsigned char)s[i])) return false;
            while (isdigit((unsigned char)s[i])) i++;
            return s[i] == '\0' && groups <= 3;
        }
        return s[i] == '\0' && groups >= 2 && groups <= 3;
    }
    return false;
}

/* ffmpeg-only literal forms with fused units. Returns true if handled. */
static bool classify_ffmpeg_literal(ParseContext *ctx, const char *t, const char *lw) {
    char buf[64];

    if (is_timestamp(t)) { add_slot(ctx, SLOT_TIME, t); return true; }

    if (strcmp(lw, "4k") == 0) { add_slot(ctx, SLOT_RES, "-2:2160"); return true; }
    if (strcmp(lw, "2k") == 0) { add_slot(ctx, SLOT_RES, "-2:1440"); return true; }
    if (strcmp(lw, "8k") == 0) { add_slot(ctx, SLOT_RES, "-2:4320"); return true; }

    size_t n = number_prefix(lw, false);
    if (n == 0 || n >= 32) return false;
    char num[32];
    memcpy(num, lw, n);
    num[n] = '\0';
    const char *suffix = lw + n;

    if (*suffix == '\0') return false; /* plain number: handled by caller */

    if (in_list(suffix, SEC_UNITS)) { add_slot(ctx, SLOT_TIME, num); return true; }
    if (in_list(suffix, MIN_UNITS)) {
        format_seconds(buf, sizeof buf, atof(num) * 60.0);
        add_slot(ctx, SLOT_TIME, buf);
        return true;
    }
    if (in_list(suffix, HOUR_UNITS)) {
        format_seconds(buf, sizeof buf, atof(num) * 3600.0);
        add_slot(ctx, SLOT_TIME, buf);
        return true;
    }
    if (strcmp(suffix, "p") == 0 && all_digits(num, n)) {
        snprintf(buf, sizeof buf, "-2:%s", num);
        add_slot(ctx, SLOT_RES, buf);
        return true;
    }
    if (suffix[0] == 'x' && all_digits(num, n) && all_digits(suffix + 1, strlen(suffix + 1))
        && strlen(suffix + 1) < 8) {
        snprintf(buf, sizeof buf, "%s:%s", num, suffix + 1);
        add_slot(ctx, SLOT_RES, buf);
        return true;
    }
    if (strcmp(suffix, "x") == 0) { add_slot(ctx, SLOT_FACTOR, num); return true; }
    if (strcmp(suffix, "fps") == 0) {
        add_slot(ctx, SLOT_FPS, num);
        add_intent(ctx, "fps");
        return true;
    }
    if ((suffix[0] == 'k' || suffix[0] == 'm') &&
        (suffix[1] == '\0' || strcmp(suffix + 1, "bps") == 0 || strcmp(suffix + 1, "b") == 0)) {
        snprintf(buf, sizeof buf, "%s%c", num, suffix[0] == 'k' ? 'k' : 'M');
        add_slot(ctx, SLOT_BITRATE, buf);
        return true;
    }
    return false;
}

/* ffmpeg plain number: uses one token of lookahead / two of lookbehind.
 * Returns number of extra tokens consumed (0 or 1). */
static int classify_ffmpeg_number(ParseContext *ctx, const char *num,
                                  const char *prev, const char *prev2, const char *next) {
    char buf[64];
    if (next) {
        if (in_list(next, SEC_UNITS)) { add_slot(ctx, SLOT_TIME, num); return 1; }
        if (in_list(next, MIN_UNITS)) {
            format_seconds(buf, sizeof buf, atof(num) * 60.0);
            add_slot(ctx, SLOT_TIME, buf);
            return 1;
        }
        if (in_list(next, HOUR_UNITS)) {
            format_seconds(buf, sizeof buf, atof(num) * 3600.0);
            add_slot(ctx, SLOT_TIME, buf);
            return 1;
        }
        if (strcmp(next, "fps") == 0) {
            add_slot(ctx, SLOT_FPS, num);
            add_intent(ctx, "fps");
            return 1;
        }
        if (strcmp(next, "kbps") == 0) {
            snprintf(buf, sizeof buf, "%sk", num);
            add_slot(ctx, SLOT_BITRATE, buf);
            return 1;
        }
    }
    bool integer = all_digits(num, strlen(num));
    if (prev && integer) {
        if (strcmp(prev, "width") == 0 || strcmp(prev, "wide") == 0) {
            snprintf(buf, sizeof buf, "%s:-2", num);
            add_slot(ctx, SLOT_RES, buf);
            return 0;
        }
        if (strcmp(prev, "height") == 0 || strcmp(prev, "tall") == 0) {
            snprintf(buf, sizeof buf, "-2:%s", num);
            add_slot(ctx, SLOT_RES, buf);
            return 0;
        }
        static const char *const res_lead[] = {
            "to", "at", "scale", "resize", "scaled", "resized", "downscale", "upscale", NULL
        };
        if (in_list(prev, res_lead) && in_list(num, STD_HEIGHTS)) {
            snprintf(buf, sizeof buf, "-2:%s", num);
            add_slot(ctx, SLOT_RES, buf);
            return 0;
        }
    }
    if (prev && (strcmp(prev, "fps") == 0 || strcmp(prev, "framerate") == 0 ||
                 (strcmp(prev, "rate") == 0 && prev2 && strcmp(prev2, "frame") == 0))) {
        add_slot(ctx, SLOT_FPS, num);
        return 0;
    }
    add_slot(ctx, SLOT_NUM, num);
    return 0;
}

/* Seconds multiplier if `sep second [unit]` closes a time range whose unit
 * should also apply to the number before `sep` ("1 to 3 seconds",
 * "2 and 5s", "1-2 minutes"); 0 otherwise. */
static double range_unit(const char *sep, const char *second, const char *after) {
    static const char *const seps[] = { "to", "and", "until", "through", "-", NULL };
    if (!in_list(sep, seps)) return 0;
    size_t n = number_prefix(second, false);
    if (n == 0) return 0;
    const char *unit = second[n] ? second + n : after;
    if (!unit) return 0;
    if (in_list(unit, SEC_UNITS)) return 1;
    if (in_list(unit, MIN_UNITS)) return 60;
    if (in_list(unit, HOUR_UNITS)) return 3600;
    return 0;
}

void nl_lex_ffmpeg(ParseContext *ctx, const RawTok *toks, size_t count, const JsonInfo *json) {
    (void)json;
    /* "turn down the volume", "turn the sound up": with a sound word around,
     * "down" / "up" mean quieter / louder, not a vertical flip. "speed up the
     * audio" is still a speed-up. */
    static const char *const sound[] = { "volume", "sound", "audio", NULL };
    static const char *const pace[] = { "speed", "sped", "speeding", "slow", "slowing", NULL };
    bool has_sound = false;
    for (size_t i = 0; i < count; i++)
        if (!toks[i].quoted && in_list(toks[i].lower, sound)) has_sound = true;

    for (size_t i = 0; i < count && !ctx->overflow; i++) {
        const char *t = toks[i].text;
        const char *lw = toks[i].lower;
        const char *prev = i > 0 ? toks[i - 1].lower : NULL;
        const char *prev2 = i > 1 ? toks[i - 2].lower : NULL;
        const char *next = i + 1 < count && !toks[i + 1].quoted ? toks[i + 1].lower : NULL;

        /* 1. Quoted strings */
        if (toks[i].quoted) {
            add_slot(ctx, looks_like_file(t, TOOL_FFMPEG) ? SLOT_FILE : SLOT_QUOTED_STR, t);
            continue;
        }
        /* 2. Dotted extension: ".mp4" */
        if (lw[0] == '.' && ext_in(lw + 1, FFMPEG_EXTS)) {
            add_slot(ctx, SLOT_EXT, lw + 1);
            add_intent(ctx, lw + 1);
            continue;
        }
        /* 3. Timestamps, resolutions, bitrates, fps, factors */
        if (classify_ffmpeg_literal(ctx, t, lw)) continue;
        /* 4. Plain numbers (with unit lookahead) */
        if (is_number(t, false)) {
            /* "from 1 to 3 seconds": the range's trailing unit also applies
             * to its first number */
            double mult = next && i + 2 < count
                ? range_unit(next, toks[i + 2].lower, i + 3 < count ? toks[i + 3].lower : NULL)
                : 0;
            if (mult > 0) {
                char buf[64];
                format_seconds(buf, sizeof buf, atof(t) * mult);
                add_slot(ctx, SLOT_TIME, buf);
                continue;
            }
            i += (size_t)classify_ffmpeg_number(ctx, t, prev, prev2, next);
            continue;
        }
        /* 5. Files */
        if (looks_like_file(t, TOOL_FFMPEG)) { add_slot(ctx, SLOT_FILE, t); continue; }
        /* 6. Standalone format words: also count as intent */
        if (ext_in(lw, FFMPEG_EXTS)) {
            add_slot(ctx, SLOT_EXT, strcmp(lw, "jpeg") == 0 ? "jpg" : lw);
            add_intent(ctx, lw);
            continue;
        }
        /* "from 00:10 to 00:14" names a segment, like "between 00:10 and
         * 00:14": one word for it, so it doesn't count twice for the cut
         * templates that list both */
        if (strcmp(lw, "from") == 0 && i + 3 < count && is_timestamp(toks[i + 1].lower) &&
            strcmp(toks[i + 2].lower, "to") == 0 && is_timestamp(toks[i + 3].lower)) {
            add_intent(ctx, "between");
            continue;
        }
        if (has_sound && (strcmp(lw, "down") == 0 || strcmp(lw, "up") == 0) &&
            !(prev && in_list(prev, pace))) {
            add_intent(ctx, lw[0] == 'd' ? "quieter" : "louder");
            continue;
        }
        lex_intent(ctx, lw);
    }
}
