/*
 * expander_curl.c — curl: refusals and the {URL} {OPTS} {HEADERS} {BODY} {UPLOAD} {OUTPUT} builders.
 */
#include "expander_int.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

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

static int curl_ph(const ParseContext *ctx, const Placeholder *ph, Out *o,
                   char *err, size_t err_size) {
    return curl_placeholder(ctx, ph->name, ph->len, ph->rest, ph->close, o, err, err_size);
}

const ToolExpander nl_expander_curl = { curl_check, NULL, curl_ph };
