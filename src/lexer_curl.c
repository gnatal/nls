/*
 * lexer_curl.c — curl: HTTP requests: methods, bodies, headers, auth, downloads.
 */
#include "lexer_int.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Does `s` start with `prefix` (ASCII, case-insensitive)? */
static bool strncasecmp_ascii(const char *s, const char *prefix, size_t n) {
    for (size_t k = 0; k < n; k++)
        if (!s[k] || tolower((unsigned char)s[k]) != prefix[k]) return false;
    return true;
}

typedef struct {
    bool saving;        /* a save verb is present: "to out.json" is where to save */
} CurlState;

/* The URL allowlist: RFC 3986 unreserved + reserved characters and '%'.
 * No spaces, quotes other than ', backslashes, backticks, braces or '<>|^'. */
static bool url_char_ok(unsigned char c) {
    return isalnum(c) || strchr("._~:/?#[]@!$&'()*+,;=%-", c) != NULL;
}

static size_t url_scheme_len(const char *s) {
    if (strncasecmp_ascii(s, "https://", 8)) return 8;
    if (strncasecmp_ascii(s, "http://", 7)) return 7;
    return 0;
}

bool nl_url_ok(const char *s) {
    size_t n = url_scheme_len(s);
    if (!n || !s[n] || s[n] == '/') return false;   /* needs a host */
    for (const char *p = s; *p; p++)
        if (!url_char_ok((unsigned char)*p)) return false;
    return strlen(s) < 2000;
}

/* Top-level domains that turn a bare "api.example.com/users" into a URL.
 * File-extension lookalikes (.sh, .md, .py, ...) are deliberately absent. */
static const char *const CURL_TLDS[] = {
    "com", "net", "org", "io", "dev", "app", "co", "ai", "gov", "edu", "info", "biz",
    "xyz", "cloud", "tech", "site", "online", "local", "internal", "localhost", NULL
};

/* "localhost:3000/api", "127.0.0.1:8080", "api.example.com/users": a host
 * with no scheme. Writes "http(s)://..." into `out` and returns true. */
static bool curl_bare_host(const char *t, char *out, size_t size) {
    size_t host = strcspn(t, ":/?#");
    if (host == 0 || host >= 254) return false;
    char h[256];
    for (size_t k = 0; k < host; k++) {
        unsigned char c = (unsigned char)t[k];
        if (!(isalnum(c) || c == '.' || c == '-')) return false;
        h[k] = (char)tolower(c);
    }
    h[host] = '\0';
    /* a port must be digits */
    if (t[host] == ':') {
        size_t d = 0;
        while (isdigit((unsigned char)t[host + 1 + d])) d++;
        if (d == 0 || d > 5 || (t[host + 1 + d] && !strchr("/?#", t[host + 1 + d]))) return false;
    }
    bool local = strcmp(h, "localhost") == 0 || strcmp(h, "127.0.0.1") == 0 ||
                 strcmp(h, "0.0.0.0") == 0;
    if (!local) {
        const char *dot = strrchr(h, '.');
        if (!dot || dot == h || !in_list(dot + 1, CURL_TLDS)) return false;
        /* "data.json"-style names have no second dot, a path or a port and
         * end in a TLD only by accident ("notes.info" is still a host) */
    }
    int n = snprintf(out, size, "%s://%s", local ? "http" : "https", t);
    return n > 0 && (size_t)n < size;
}

static const char *const CURL_SAVE_VERBS[] = {
    "download", "downloading", "save", "saving", "write", "store", "dump", "output", NULL
};

/* "as out.zip", "into downloads/": where to save the response */
static const char *const CURL_DEST_LEADERS[] = {
    "as", "into", "named", "called", "-o", "--output", NULL
};

static const char *const CURL_TLS_WORDS[] = {
    "ssl", "tls", "cert", "certs", "certificate", "certificates", "https", NULL
};

static const char *const CURL_TLS_SKIP[] = {
    "ignore", "ignoring", "skip", "skipping", "no", "without", "disable", "disabling",
    "bypass", "bypassing", "accept", "allow", NULL
};

static const char *const CURL_TLS_TAIL[] = {
    "error", "errors", "verification", "verify", "verifying", "check", "checks", "checking",
    "validation", "warnings", NULL
};

static const char *const CURL_SEC_UNITS[] = { "s", "sec", "secs", "second", "seconds", NULL };

static const char *const CURL_MIN_UNITS[] = { "m", "min", "mins", "minute", "minutes", NULL };

static const char *const CURL_TIMEOUT_WORDS[] = {
    "timeout", "time-out", "--max-time", "-m", "--connect-timeout", NULL
};

static const char *const CURL_FILLER[] = {
    "url", "endpoint", "api", "request", "requests", "server", "website", "site", "page",
    "via", "over", "http", "https", "body", "payload", "data", "-ss", "-s", "--silent", NULL
};

/* JSON or form body given inline: {"a":1}, [1,2], name=bob */
static bool curl_inline_body(const char *t) {
    if (t[0] == '{' || t[0] == '[') return true;
    if (!(isalpha((unsigned char)t[0]) || t[0] == '_')) return false;
    for (const char *p = t; *p; p++) {
        if (*p == '=') return p[1] != '=';
        if (!(isalnum((unsigned char)*p) || strchr("_.-[]", *p))) return false;
    }
    return false;
}

/* "X-Api-Key: abc", "Accept:application/json": a header line */
static bool curl_header_like(const char *t) {
    size_t n = 0;
    while (isalnum((unsigned char)t[n]) || t[n] == '-' || t[n] == '_') n++;
    return n > 0 && t[n] == ':' && t[n + 1] != '/';  /* not "http://" */
}

static bool env_name_ok(const char *s, bool upper_only) {
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    bool alpha = false;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '_')) return false;
        if (upper_only && islower(c)) return false;
        if (isalpha(c)) alpha = true;
    }
    return alpha && strlen(s) < 64;
}

/* A number of seconds at toks[k] ("10", "10s", "10 seconds", "2 minutes").
 * Writes the seconds into `out`; returns tokens consumed or 0. */
static size_t curl_seconds(const RawTok *toks, size_t count, size_t k, char *out, size_t size) {
    if (k >= count || toks[k].quoted) return 0;
    const char *w = toks[k].lower;
    size_t nl = number_prefix(w, false);
    if (!nl) return 0;
    double mult = 1;
    size_t used = 1;
    const char *unit = w + nl;
    if (*unit == '\0' && k + 1 < count && !toks[k + 1].quoted &&
        (in_list(toks[k + 1].lower, CURL_SEC_UNITS) || in_list(toks[k + 1].lower, CURL_MIN_UNITS))) {
        unit = toks[k + 1].lower;
        used = 2;
    }
    if (*unit) {
        if (in_list(unit, CURL_MIN_UNITS)) mult = 60;
        else if (!in_list(unit, CURL_SEC_UNITS)) return 0;
    }
    char num[32];
    if (nl >= sizeof num) return 0;
    memcpy(num, w, nl);
    num[nl] = '\0';
    double secs = atof(num) * mult;
    if (secs <= 0 || secs > 86400) return 0;
    format_seconds(out, size, secs);
    return used;
}

static void curl_add_path(ParseContext *ctx, const RawTok *toks, size_t count, size_t i,
                          const CurlState *st) {
    size_t at = 0;
    const char *pw = prev_word(toks, i, &at);
    /* "save the response to out.json", "download it into the folder dl/" */
    if (pw && in_list(pw, FIND_DIR_NOUNS)) pw = prev_word(toks, at, &at);
    bool dest = pw && (in_list(pw, CURL_DEST_LEADERS) ||
                       (st->saving && (strcmp(pw, "to") == 0 || strcmp(pw, "in") == 0)));
    /* "get data.json from https://...": the name to save under */
    if (!dest && i + 2 < count && !toks[i + 1].quoted && strcmp(toks[i + 1].lower, "from") == 0 &&
        url_scheme_len(toks[i + 2].text))
        dest = true;
    add_slot(ctx, dest ? SLOT_DEST : SLOT_FILE, toks[i].text);
}

/* Classify toks[i] in curl mode. Returns the number of tokens consumed
 * (>= 1), or 0 if the token should fall through to intent handling. */
static size_t classify_curl(ParseContext *ctx, const RawTok *toks, size_t count, size_t i,
                            const CurlState *st) {
    const char *t = toks[i].text;
    const char *lw = toks[i].lower;
    const char *next = i + 1 < count && !toks[i + 1].quoted ? toks[i + 1].lower : NULL;
    const char *prev = i > 0 && !toks[i - 1].quoted ? toks[i - 1].lower : NULL;
    char buf[NL_MAX_INPUT + 32];

    /* 1. URLs, with or without a scheme */
    if (url_scheme_len(t)) { add_slot(ctx, SLOT_URL, t); return 1; }
    if (!toks[i].quoted && curl_bare_host(t, buf, sizeof buf)) {
        add_slot(ctx, SLOT_URL, buf);
        return 1;
    }

    /* 2. Headers: "header 'X-Api-Key: abc'", "-H Accept:application/json",
     *    or any quoted "Name: value" */
    bool header_word = strcmp(lw, "header") == 0 || strcmp(lw, "-h") == 0 ||
                       strcmp(lw, "--header") == 0;
    if (header_word && i + 1 < count && curl_header_like(toks[i + 1].text)) {
        add_slot(ctx, SLOT_HEADER, toks[i + 1].text);
        return 2;
    }
    if (toks[i].quoted && curl_header_like(t)) { add_slot(ctx, SLOT_HEADER, t); return 1; }

    /* 3. Bearer tokens: "token $TOKEN", "bearer GITHUB_TOKEN", "auth token 'abc'" */
    if (!toks[i].quoted && (strcmp(lw, "token") == 0 || strcmp(lw, "bearer") == 0 ||
                            strcmp(lw, "--oauth2-bearer") == 0)) {
        size_t k = i + 1;
        if (strcmp(lw, "bearer") == 0 && k < count && strcmp(toks[k].lower, "token") == 0) k++;
        static const char *const glue[] = {
            "from", "in", "env", "environment", "variable", "var", "is", "=", NULL
        };
        while (k < count && !toks[k].quoted && in_list(toks[k].lower, glue)) k++;
        if (k < count) {
            const char *v = toks[k].text;
            if (toks[k].quoted && v[0] && !strpbrk(v, "\r\n")) {
                snprintf(buf, sizeof buf, "Authorization: Bearer %s", v);
            } else if (v[0] == '$' && env_name_ok(v + 1, false)) {
                snprintf(buf, sizeof buf, "Authorization: Bearer %s", v);
            } else if (env_name_ok(v, true) && strlen(v) >= 2) {
                snprintf(buf, sizeof buf, "Authorization: Bearer $%s", v);
            } else {
                buf[0] = '\0';
            }
            if (buf[0]) { add_slot(ctx, SLOT_HEADER, buf); return k + 1 - i; }
        }
    }

    /* 4. Basic auth: "as user alice", "-u alice", "basic auth alice" */
    {
        size_t k = 0;
        if (strcmp(lw, "-u") == 0 || strcmp(lw, "--user") == 0 || strcmp(lw, "username") == 0)
            k = i + 1;
        else if (strcmp(lw, "user") == 0 && prev &&
                 (strcmp(prev, "as") == 0 || strcmp(prev, "with") == 0))
            k = i + 1;
        else if (strcmp(lw, "basic") == 0 && next && strcmp(next, "auth") == 0)
            k = i + 2;
        if (k && k < count && toks[k].text[0] && !url_scheme_len(toks[k].text)) {
            const char *v = toks[k].text;
            bool ok = true;
            for (const char *p = v; *p && ok; p++)
                if (!(isalnum((unsigned char)*p) || strchr("._@:+-", *p))) ok = false;
            if (ok || toks[k].quoted) { add_slot(ctx, SLOT_AUTHOR, v); return k + 1 - i; }
        }
    }

    /* 5. Inline bodies: '{"name":"bob"}', name=bob */
    if (curl_inline_body(t)) { add_slot(ctx, SLOT_QUOTED_STR, t); return 1; }

    /* 6. Other quoted text: a path, or else a raw body */
    if (toks[i].quoted) {
        if (looks_like_file(t, TOOL_GREP)) curl_add_path(ctx, toks, count, i, st);
        else add_slot(ctx, SLOT_QUOTED_STR, t);
        return 1;
    }

    /* 7. Modifiers -> flags */
    if (strcmp(lw, "follow") == 0 || strcmp(lw, "following") == 0 || strcmp(lw, "-l") == 0 ||
        strcmp(lw, "--location") == 0) {
        add_flag(ctx, 'L');
        return next && strncmp(next, "redirect", 8) == 0 ? 2 : 1;
    }
    if (strncmp(lw, "redirect", 8) == 0 && strlen(lw) <= 12) { add_flag(ctx, 'L'); return 1; }
    if (strcmp(lw, "insecure") == 0 || strcmp(lw, "insecurely") == 0 ||
        strcmp(lw, "self-signed") == 0 || strcmp(lw, "-k") == 0 || strcmp(lw, "--insecure") == 0) {
        add_flag(ctx, 'k');
        return 1;
    }
    if (in_list(lw, CURL_TLS_SKIP)) {
        size_t k = i + 1;
        if (k < count && strcmp(toks[k].lower, "the") == 0) k++;
        if (k < count && !toks[k].quoted && in_list(toks[k].lower, CURL_TLS_WORDS)) {
            k++;
            if (k < count && in_list(toks[k].lower, CURL_TLS_WORDS)) k++;  /* "ssl cert errors" */
            if (k < count && in_list(toks[k].lower, CURL_TLS_TAIL)) k++;
            add_flag(ctx, 'k');
            return k - i;
        }
    }
    if (strcmp(lw, "compressed") == 0 || strcmp(lw, "--compressed") == 0 ||
        strcmp(lw, "gzip") == 0 || (strcmp(lw, "compression") == 0 && prev &&
                                    strcmp(prev, "with") == 0)) {
        add_flag(ctx, 'c');
        return 1;
    }

    /* 8. Retries: "retry 3 times", "3 retries", "--retry 3" */
    if ((strcmp(lw, "retry") == 0 || strcmp(lw, "retries") == 0 || strcmp(lw, "--retry") == 0 ||
         strcmp(lw, "retrying") == 0) &&
        next && all_digits(next, strlen(next)) && strlen(next) <= 3) {
        add_slot(ctx, SLOT_NUM, next);
        return i + 2 < count && strncmp(toks[i + 2].lower, "time", 4) == 0 &&
               strlen(toks[i + 2].lower) <= 5 ? 3 : 2;
    }
    if (all_digits(lw, strlen(lw)) && strlen(lw) <= 3 && next &&
        (strcmp(next, "retries") == 0 || strcmp(next, "retry") == 0 || strcmp(next, "attempts") == 0 ||
         strcmp(next, "tries") == 0)) {
        add_slot(ctx, SLOT_NUM, lw);
        return 2;
    }

    /* 9. Timeouts: "timeout 10s", "timeout of 2 minutes", "time out after 5 seconds",
     *    "max time 30", "10 second timeout" */
    {
        size_t k = 0;
        if (in_list(lw, CURL_TIMEOUT_WORDS)) k = i + 1;
        else if ((strcmp(lw, "time") == 0 && next && strcmp(next, "out") == 0) ||
                 (strcmp(lw, "max") == 0 && next && strcmp(next, "time") == 0))
            k = i + 2;
        if (k) {
            if (k < count && (strcmp(toks[k].lower, "of") == 0 || strcmp(toks[k].lower, "after") == 0 ||
                              strcmp(toks[k].lower, "in") == 0))
                k++;
            size_t used = curl_seconds(toks, count, k, buf, sizeof buf);
            if (used) { add_slot(ctx, SLOT_TIME, buf); return k + used - i; }
        }
        size_t used = curl_seconds(toks, count, i, buf, sizeof buf);
        if (used && i + used < count && in_list(toks[i + used].lower, CURL_TIMEOUT_WORDS)) {
            add_slot(ctx, SLOT_TIME, buf);
            return used + 1;
        }
    }

    /* 10. Numbers mean nothing else */
    if (is_number(lw, false)) {
        if (!ctx->stray) ctx->stray = arena_dup(ctx, t);
        return 1;
    }

    /* 11. Paths and file names */
    if (strchr(t, '/') || looks_like_file(t, TOOL_GREP)) {
        curl_add_path(ctx, toks, count, i, st);
        return 1;
    }

    /* 12. Glue words carry no intent */
    if (in_list(lw, CURL_FILLER) && !is_keyword(TOOL_CURL, lw)) return 1;
    return 0;
}

void nl_lex_curl(ParseContext *ctx, const RawTok *toks, size_t count, const JsonInfo *json) {
    (void)json;
    CurlState cst = { false };
    for (size_t i = 0; i < count; i++)
        if (!toks[i].quoted && in_list(toks[i].lower, CURL_SAVE_VERBS)) cst.saving = true;

    for (size_t i = 0; i < count && !ctx->overflow; i++) {
        size_t used = classify_curl(ctx, toks, count, i, &cst);
        if (used) { i += used - 1; continue; }
        lex_intent(ctx, toks[i].lower);
    }
}
