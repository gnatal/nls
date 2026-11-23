/*
 * inspect.c — read-only peek at a jq input file.
 *
 * Learns two things from at most NL_INSPECT_BYTES of the file:
 *   - the top-level shape: '[' (array) or '{' (object)
 *   - the field names one level in: keys of the root object, or keys of the
 *     objects inside the root array. Only identifier-like keys are kept, so
 *     ".<key>" is always a valid, safe jq path.
 * This lets prompts say "emails" instead of ".email". It is a tiny scanner,
 * not a JSON parser: malformed input just yields fewer (or no) fields.
 */
#include "nl.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define NL_INSPECT_BYTES (64 * 1024)
#define MAX_DEPTH 64

static bool is_identifier(const char *s, size_t n) {
    if (n == 0 || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (size_t i = 1; i < n; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return false;
    return true;
}

static void add_field(JsonInfo *info, const char *s, size_t n) {
    if (n >= sizeof info->fields[0] || !is_identifier(s, n)) return;
    for (size_t i = 0; i < info->field_count; i++)
        if (strlen(info->fields[i]) == n && strncmp(info->fields[i], s, n) == 0) return;
    if (info->field_count >= NL_MAX_FIELDS) return;
    memcpy(info->fields[info->field_count], s, n);
    info->fields[info->field_count][n] = '\0';
    info->field_count++;
}

bool nl_inspect_json(const char *path, JsonInfo *info) {
    static char buf[NL_INSPECT_BYTES];
    memset(info, 0, sizeof *info);

    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t len = fread(buf, 1, sizeof buf, f);
    fclose(f);

    size_t i = 0;
    if (len >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF)
        i = 3;
    while (i < len && isspace((unsigned char)buf[i])) i++;
    if (i >= len || (buf[i] != '[' && buf[i] != '{')) return false;
    info->shape = buf[i];

    /* keys live at depth 1 for an object root, depth 2 for an array root */
    int key_depth = info->shape == '{' ? 1 : 2;
    char stack[MAX_DEPTH];
    int depth = 0;

    for (; i < len; i++) {
        char c = buf[i];
        if (c == '{' || c == '[') {
            if (depth < MAX_DEPTH) stack[depth] = c;
            depth++;
        } else if (c == '}' || c == ']') {
            if (depth > 0) depth--;
        } else if (c == '"') {
            size_t start = ++i;
            while (i < len && buf[i] != '"') {
                if (buf[i] == '\\') i++;
                i++;
            }
            if (i >= len) break;
            size_t end = i;
            size_t j = i + 1;
            while (j < len && isspace((unsigned char)buf[j])) j++;
            if (j < len && buf[j] == ':' && depth == key_depth && depth <= MAX_DEPTH &&
                stack[depth - 1] == '{')
                add_field(info, buf + start, end - start);
        }
    }
    return true;
}
