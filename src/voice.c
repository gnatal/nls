/*
 * voice.c — spoken requests: nls --listen.
 *
 * nls links no library for this. It records with a program that is
 * already installed (sox's rec, arecord, or ffmpeg) and transcribes offline
 * with whisper.cpp (whisper-cli and a ggml model). Either step can be
 * replaced by a shell command:
 *
 *   NLS_RECORD   records "$NLS_WAV" (16 kHz mono WAV) for at most
 *                $NLS_LISTEN_SECONDS; it gets SIGINT when a key is pressed
 *   NLS_STT      prints the text spoken in "$NLS_WAV" on stdout
 *
 * The transcript is then rewritten into the words the lexer knows
 * (nl_voice_normalize()): spelled-out numbers become digits, "users dot
 * json" becomes users.json, "restore slash" becomes restore/, and fillers
 * and non-speech tags ("[BLANK_AUDIO]") go.
 */
#define _POSIX_C_SOURCE 200809L   /* fork, select, mkdtemp, setenv, sigaction */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE          /* macOS declares mkdtemp only with this */
#endif
#include "nl.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Normalizing the transcript                                         */
/* ------------------------------------------------------------------ */

#define NV_MAX_WORDS 256

static bool in_list(const char *w, const char *const *list) {
    for (; *list; list++) if (strcmp(w, *list) == 0) return true;
    return false;
}

static const char *const FILLERS[] = {
    "um", "umm", "uh", "uhh", "uhm", "erm", "er", "hmm", "mm", NULL
};

/* Extensions a spoken "dot" joins to the word before it: "users dot json".
 * Before any other word "dot" starts a JSON path: "get dot name". */
static const char *const EXTS[] = {
    "mp4", "mov", "mkv", "avi", "webm", "m4v", "mp3", "wav", "flac", "aac", "m4a",
    "ogg", "opus", "gif", "png", "jpg", "jpeg", "webp", "avif", "heic", "tif", "tiff",
    "bmp", "ico", "svg", "pdf", "json", "txt", "md", "log", "csv", "tsv", "xml", "yaml",
    "yml", "toml", "html", "css", "js", "ts", "py", "c", "h", "cpp", "go", "rs", "rb",
    "java", "sh", "tar", "gz", "tgz", "bz2", "xz", "zst", "zip", "conf", "cfg", "ini",
    "env", "lock", "sql", "bin", "tmp", "bak", NULL
};

/* Words a spoken "slash" doesn't join: "in slash tmp" is "in /tmp". */
static const char *const LOOSE[] = {
    "the", "a", "an", "in", "into", "to", "from", "of", "under", "inside", "at", "on",
    "with", "and", "or", "for", "by", "as", "is", "all", "every", "each", "my", "this",
    "that", "than", "named", "called", NULL
};

static const char *const UNITS[] = {
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
    "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
    "seventeen", "eighteen", "nineteen", NULL
};
static const char *const TENS[] = {
    "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety", NULL
};

static int list_index(const char *w, const char *const *list) {
    for (int i = 0; list[i]; i++) if (strcmp(w, list[i]) == 0) return i;
    return -1;
}

typedef enum { NW_NONE, NW_UNIT, NW_TENS, NW_HUNDRED, NW_THOUSAND, NW_AND } NumKind;

/* What kind of number word `w` is, and its value: "seven" 7, "forty" 40,
 * "twenty-five" 25 (a tens). */
static NumKind num_word(const char *w, int *val) {
    int i;
    if ((i = list_index(w, UNITS)) >= 0) { *val = i; return NW_UNIT; }
    if ((i = list_index(w, TENS)) >= 0) { *val = 20 + 10 * i; return NW_TENS; }
    if (strcmp(w, "hundred") == 0) return NW_HUNDRED;
    if (strcmp(w, "thousand") == 0) return NW_THOUSAND;
    if (strcmp(w, "and") == 0) return NW_AND;
    const char *dash = strchr(w, '-');
    if (dash && dash - w < 16) {
        char tens[16];
        memcpy(tens, w, (size_t)(dash - w));
        tens[dash - w] = '\0';
        int t = list_index(tens, TENS), u = list_index(dash + 1, UNITS);
        if (t >= 0 && u >= 1 && u <= 9) { *val = 20 + 10 * t + u; return NW_TENS; }
    }
    return NW_NONE;
}

typedef struct {
    char *raw[NV_MAX_WORDS];      /* the words as transcribed              */
    char bare[NV_MAX_WORDS][32];  /* lowercased, trailing punctuation off  */
    const char *tail[NV_MAX_WORDS];/* that trailing punctuation, in raw    */
    size_t n;
} Words;

/* The number spelled out from word i on ("two hundred and fifty"): its
 * value and how many words it takes, 0 if word i starts none. A word
 * followed by punctuation ends the number. */
static size_t spelled_number(const Words *ws, size_t i, long *value) {
    long total = 0, cur = 0;
    NumKind last = NW_NONE;
    size_t j = i;
    for (; j < ws->n; j++) {
        int v = 0;
        NumKind k = num_word(ws->bare[j], &v);
        bool open = last == NW_NONE || last == NW_HUNDRED || last == NW_THOUSAND || last == NW_AND;
        if (k == NW_UNIT && (open || (last == NW_TENS && cur % 10 == 0 && v >= 1 && v <= 9)))
            cur += v;
        else if (k == NW_TENS && open)
            cur += v;
        else if (k == NW_HUNDRED && (last == NW_UNIT || last == NW_TENS) && cur > 0 && cur < 100)
            cur *= 100;
        else if (k == NW_THOUSAND && last != NW_NONE && last != NW_AND && total == 0 && cur > 0)
            { total = cur * 1000; cur = 0; }
        else if (k == NW_AND && (last == NW_HUNDRED || last == NW_THOUSAND) && j + 1 < ws->n &&
                 ws->tail[j][0] == '\0') {
            int nv;
            NumKind nk = num_word(ws->bare[j + 1], &nv);
            if (nk != NW_UNIT && nk != NW_TENS) break;
        } else break;
        last = k;
        if (ws->tail[j][0]) { j++; break; }
    }
    if (last == NW_NONE) return 0;
    *value = total + cur;
    return j - i;
}

typedef struct {
    char *buf;
    size_t size, len;
    size_t last;        /* where the last word starts */
    bool glue;          /* the next word joins the last one */
    bool full;
} Out;

static void put(Out *o, const char *s, size_t n) {
    if (o->full || o->len + n + 1 > o->size) { o->full = true; return; }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

/* Start a word (or continue the last one when glued). */
static void word(Out *o, const char *s, size_t n) {
    if (!o->glue && o->len > 0) put(o, " ", 1);
    if (!o->glue) o->last = o->len;
    o->glue = false;
    put(o, s, n);
}

static void words(Out *o, const char *s) { word(o, s, strlen(s)); }

/* Is the last word written one a "slash" can join ("restore", not "into")? */
static bool last_joins(const Out *o) {
    if (o->len == 0) return false;
    const char *w = o->buf + o->last;
    char low[32];
    size_t n = strlen(w);
    if (n >= sizeof low) return true;
    for (size_t i = 0; i <= n; i++) low[i] = (char)tolower((unsigned char)w[i]);
    return !in_list(low, LOOSE) && !strpbrk(low, ",;:!?");
}

/* Drop "[BLANK_AUDIO]", "[ Silence ]" and "(upbeat music)": what whisper
 * writes for sounds that aren't speech. */
static void drop_tags(char *s) {
    char *w = s;
    for (char *p = s; *p; ) {
        char close = *p == '[' ? ']' : *p == '(' ? ')' : 0;
        char *end = close ? strchr(p + 1, close) : NULL;
        bool tag = end != NULL;
        if (tag && close == ')')
            for (char *q = p + 1; q < end; q++)
                if (!isalpha((unsigned char)*q) && *q != ' ') { tag = false; break; }
        if (tag) { *w++ = ' '; p = end + 1; continue; }
        *w++ = *p++;
    }
    *w = '\0';
}

bool nl_voice_normalize(const char *in, char *out, size_t size) {
    static char copy[NL_MAX_INPUT * 2];
    static Words ws;
    if (size == 0) return false;
    out[0] = '\0';
    size_t inlen = strlen(in);
    if (inlen >= sizeof copy) return false;
    memcpy(copy, in, inlen + 1);
    for (char *p = copy; *p; p++)
        if (isspace((unsigned char)*p) || (unsigned char)*p < 0x20) *p = ' ';
    drop_tags(copy);

    ws.n = 0;
    for (char *p = strtok(copy, " "); p; p = strtok(NULL, " ")) {
        if (ws.n == NV_MAX_WORDS) return false;
        size_t len = strlen(p), core = len;
        while (core > 0 && strchr(",.!?;:", p[core - 1])) core--;
        ws.raw[ws.n] = p;
        ws.tail[ws.n] = p + core;
        size_t b = core < sizeof ws.bare[0] - 1 ? core : 0;   /* too long: no spoken word */
        for (size_t i = 0; i < b; i++) ws.bare[ws.n][i] = (char)tolower((unsigned char)p[i]);
        ws.bare[ws.n][b] = '\0';
        ws.n++;
    }

    Out o = { out, size, 0, 0, false, false };
    char num[64];
    for (size_t i = 0; i < ws.n; i++) {
        const char *b = ws.bare[i], *tail = ws.tail[i];
        const char *next = i + 1 < ws.n ? ws.bare[i + 1] : NULL;

        if (in_list(b, FILLERS)) continue;

        long v;
        size_t used = spelled_number(&ws, i, &v);
        if (used > 0) {
            snprintf(num, sizeof num, "%ld", v);
            size_t end = i + used;
            /* "one point five": 1.5 */
            int d;
            if (ws.tail[end - 1][0] == '\0' && end + 1 < ws.n && strcmp(ws.bare[end], "point") == 0 &&
                num_word(ws.bare[end + 1], &d) == NW_UNIT && d <= 9) {
                size_t nl = strlen(num);
                num[nl++] = '.';
                for (end++; end < ws.n && nl + 1 < sizeof num; end++) {
                    if (num_word(ws.bare[end], &d) != NW_UNIT || d > 9) break;
                    num[nl++] = (char)('0' + d);
                    if (ws.tail[end][0]) { end++; break; }
                }
                num[nl] = '\0';
            }
            words(&o, num);
            put(&o, ws.tail[end - 1], strlen(ws.tail[end - 1]));
            i = end - 1;
            continue;
        }

        if (strcmp(b, "dot") == 0 && tail[0] == '\0' && next && next[0]) {
            if (in_list(next, EXTS) && last_joins(&o)) {
                put(&o, ".", 1);                      /* onto the last word */
                put(&o, next, strlen(next));         /* "JSON" -> json */
                put(&o, ws.tail[i + 1], strlen(ws.tail[i + 1]));
                o.glue = false;
                i++;
            } else {
                words(&o, ".");                       /* ".name" */
                o.glue = true;
            }
            continue;
        }
        if (strcmp(b, "forward") == 0 && next && strcmp(next, "slash") == 0) continue;
        if (strcmp(b, "slash") == 0 || strcmp(b, "underscore") == 0 ||
            strcmp(b, "dash") == 0 || strcmp(b, "hyphen") == 0 || strcmp(b, "tilde") == 0) {
            bool slash = b[0] == 's', tilde = b[0] == 't';
            const char *c = slash ? "/" : tilde ? "~" : b[0] == 'u' ? "_" : "-";
            if (!tilde && o.len > 0 && (!slash || last_joins(&o))) o.glue = true;
            words(&o, c);
            put(&o, tail, strlen(tail));
            /* join the next word, unless it's an ordinary one after a slash */
            o.glue = tail[0] == '\0' && next && !(slash && in_list(next, LOOSE));
            continue;
        }

        /* a tool said first: "Git, undo the last commit" */
        if (o.len == 0 && nl_tool_from_name(b) != TOOL_UNKNOWN) {
            words(&o, b);
            continue;
        }

        const char *w = ws.raw[i];
        size_t n = strlen(w);
        /* a pause after the first word: "Docker, list containers" */
        if (o.len == 0 && tail[0] && strspn(tail, ",;:") == strlen(tail)) n = (size_t)(tail - w);
        /* the transcript's capital at the start: "Users dot json" */
        if (o.len == 0 && n < 32 && isupper((unsigned char)w[0])) {
            char low[32];
            bool title = true;
            for (size_t k = 1; k < n && title; k++)
                if (isalpha((unsigned char)w[k]) && !islower((unsigned char)w[k])) title = false;
            if (title) {
                memcpy(low, w, n);
                low[n] = '\0';
                low[0] = (char)tolower((unsigned char)low[0]);
                word(&o, low, n);
                continue;
            }
        }
        word(&o, w, n);
    }
    if (o.full) { out[0] = '\0'; return false; }
    /* a full stop at the end ("... into restore/.") */
    while (o.len > 0 && strchr(".!? ", out[o.len - 1])) out[--o.len] = '\0';
    return true;
}

/* ------------------------------------------------------------------ */
/* Recording and transcribing                                         */
/* ------------------------------------------------------------------ */

#ifdef _WIN32

int nl_listen(bool verbose, char *out, size_t size) {
    (void)verbose; (void)out; (void)size;
    fprintf(stderr, "nls: --listen isn't supported on Windows yet\n");
    return 2;
}

#else

#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NL_LISTEN_SECONDS 10
#define NL_MODEL_URL "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin"

/* A hint for whisper: the words and the way of writing them (file names
 * with their dot, times with a colon) nls understands best. */
#define NL_STT_PROMPT \
    "Requests for ffmpeg, jq, grep, find, tar, git, curl and magick: " \
    "cut input.mp4 from 00:10 to 00:25. get .name from users.json. " \
    "extract backup.tar.gz into restore/."

static volatile sig_atomic_t interrupted;

static void on_sigint(int sig) { (void)sig; interrupted = 1; }

/* `name` as found on PATH, in `path` (or false). */
static bool on_path(const char *name, char *path, size_t size) {
    const char *p = getenv("PATH");
    if (!p) p = "/usr/bin:/bin";
    while (*p) {
        size_t n = strcspn(p, ":");
        int k = snprintf(path, size, "%.*s/%s", (int)n, n ? p : ".", name);
        if (k > 0 && (size_t)k < size && access(path, X_OK) == 0) return true;
        p += n;
        if (*p == ':') p++;
    }
    return false;
}

/* The whisper.cpp model: $NLS_WHISPER_MODEL, or the first of a few in
 * ${XDG_DATA_HOME:-~/.local/share}/nls/. */
static bool find_model(char *path, size_t size) {
    path[0] = '\0';
    const char *env = getenv("NLS_WHISPER_MODEL");
    if (env && env[0]) {
        int k = snprintf(path, size, "%s", env);
        return k > 0 && (size_t)k < size && access(path, R_OK) == 0;
    }
    static const char *const MODELS[] = {
        "ggml-base.en.bin", "ggml-small.en.bin", "ggml-tiny.en.bin", "ggml-medium.en.bin",
        "ggml-base.bin", "ggml-small.bin", "ggml-tiny.bin", "ggml-large-v3-turbo.bin", NULL
    };
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    char dir[1024];
    int d;
    if (xdg && xdg[0]) d = snprintf(dir, sizeof dir, "%s/nls", xdg);
    else if (home && home[0]) d = snprintf(dir, sizeof dir, "%s/.local/share/nls", home);
    else return false;
    if (d < 0 || (size_t)d >= sizeof dir) return false;
    for (int i = 0; MODELS[i]; i++) {
        int k = snprintf(path, size, "%s/%s", dir, MODELS[i]);
        if (k > 0 && (size_t)k < size && access(path, R_OK) == 0) return true;
    }
    int k = snprintf(path, size, "%s/%s", dir, MODELS[0]);   /* where to put one */
    if (k < 0 || (size_t)k >= size) path[0] = '\0';
    return false;
}

/* Print up to 12 lines of a log file, indented, to stderr. */
static void show_log(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    for (int i = 0; i < 12 && fgets(line, sizeof line, f); i++) {
        fprintf(stderr, "  %s", line);
        if (!strchr(line, '\n')) fputc('\n', stderr);
    }
    fclose(f);
}

static void cleanup(const char *dir, const char *const *files) {
    for (; *files; files++) unlink(*files);
    rmdir(dir);
}

/* Run the recorder in its own process group until it ends, a key is
 * pressed, Ctrl+C, or the time is up (with a margin, for an NLS_RECORD
 * that doesn't stop on its own). Returns its wait status, -1 if it couldn't
 * start; `*stopped` says it was interrupted on purpose. */
static int record(const char *cmd, const char *log, int seconds, bool *stopped) {
    *stopped = false;
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setpgid(0, 0);
        int nul = open("/dev/null", O_RDONLY);
        int lg = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (nul >= 0) dup2(nul, 0);
        if (lg >= 0) { dup2(lg, 1); dup2(lg, 2); }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid);

    int tty = open("/dev/tty", O_RDONLY | O_NONBLOCK);
    time_t start = time(NULL), stop_at = 0;
    int status = 0;
    for (;;) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid || (w < 0 && errno != EINTR)) break;
        time_t now = time(NULL);
        if (!*stopped && (interrupted || now - start > seconds + 2)) {
            kill(-pid, SIGINT);
            *stopped = true;
            stop_at = now;
        } else if (*stopped && now - stop_at > 3) {
            kill(-pid, SIGKILL);
        }
        /* select(), not poll(): macOS's poll() doesn't take a terminal */
        fd_set rd;
        FD_ZERO(&rd);
        if (tty >= 0) FD_SET(tty, &rd);
        struct timeval tv = { 0, 100000 };
        int r = select(tty >= 0 ? tty + 1 : 0, &rd, NULL, NULL, &tv);
        if (r > 0 && tty >= 0 && FD_ISSET(tty, &rd)) {
            char buf[64];
            ssize_t k = read(tty, buf, sizeof buf);
            if (k > 0 && !*stopped) {
                kill(-pid, SIGINT);      /* any key, Enter in a cooked terminal */
                *stopped = true;
                stop_at = time(NULL);
            } else if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR)) {
                close(tty);                 /* the terminal went away */
                tty = -1;
            }
        }
    }
    if (tty >= 0) close(tty);
    return status;
}

int nl_listen(bool verbose, char *out, size_t size) {
    static char raw[NL_MAX_INPUT * 2];
    char rec_cmd[1024], stt_cmd[1024], bin[1024], model[1024];
    out[0] = '\0';

    int seconds = NL_LISTEN_SECONDS;
    const char *secs = getenv("NLS_LISTEN_SECONDS");
    if (secs && secs[0]) {
        char *end;
        long s = strtol(secs, &end, 10);
        if (*end != '\0' || s < 1 || s > 300) {
            fprintf(stderr, "nls: NLS_LISTEN_SECONDS must be a number of seconds, 1 to 300\n");
            return 2;
        }
        seconds = (int)s;
    }

    /* What transcribes: checked first, so nobody talks for nothing. */
    const char *stt = getenv("NLS_STT");
    if (stt && stt[0]) {
        int k = snprintf(stt_cmd, sizeof stt_cmd, "{ %s\n} 2>\"$NLS_STT_LOG\"", stt);
        if (k < 0 || (size_t)k >= sizeof stt_cmd) {
            fprintf(stderr, "nls: NLS_STT is too long (max %zu bytes)\n", sizeof stt_cmd - 32);
            return 2;
        }
    } else {
        bool have_bin = on_path("whisper-cli", bin, sizeof bin) ||
                        on_path("whisper-cpp", bin, sizeof bin);
        bool have_model = find_model(model, sizeof model);
        if (!have_bin || !have_model) {
            fprintf(stderr, "nls: --listen transcribes offline with whisper.cpp, and %s.\n",
                    !have_bin ? "whisper-cli isn't installed" : "no model was found");
            if (!have_bin)
                fprintf(stderr, "  install it:  brew install whisper-cpp   (Linux: build it from\n"
                                "               https://github.com/ggml-org/whisper.cpp)\n");
            if (!have_model) {
                const char *env = getenv("NLS_WHISPER_MODEL");
                const char *slash = strrchr(model, '/');
                if (env && env[0])
                    fprintf(stderr, "  NLS_WHISPER_MODEL is set to %s, which can't be read\n", env);
                else if (slash)
                    fprintf(stderr,
                        "  get a model (about 150 MB, once):\n"
                        "    mkdir -p '%.*s' && curl -fL -o '%s' \\\n"
                        "      " NL_MODEL_URL "\n",
                        (int)(slash - model), model, model);
                else
                    fprintf(stderr, "  get a model (" NL_MODEL_URL ")\n"
                                    "  and set NLS_WHISPER_MODEL to its path\n");
            }
            fprintf(stderr, "  or set NLS_STT to a command that prints the text spoken in \"$NLS_WAV\"\n");
            return 2;
        }
        setenv("NLS_WHISPER", bin, 1);
        setenv("NLS_WHISPER_MODEL", model, 1);
        setenv("NLS_STT_PROMPT", NL_STT_PROMPT, 1);
        snprintf(stt_cmd, sizeof stt_cmd, "%s",
                 "exec \"$NLS_WHISPER\" -m \"$NLS_WHISPER_MODEL\" -f \"$NLS_WAV\" -l en -nt -np "
                 "--prompt \"$NLS_STT_PROMPT\" 2>\"$NLS_STT_LOG\"");
    }

    /* What records. */
    const char *recorder = getenv("NLS_RECORD");
    if (recorder && recorder[0]) {
        int k = snprintf(rec_cmd, sizeof rec_cmd, "%s", recorder);
        if (k < 0 || (size_t)k >= sizeof rec_cmd) {
            fprintf(stderr, "nls: NLS_RECORD is too long (max %zu bytes)\n", sizeof rec_cmd - 1);
            return 2;
        }
    } else if (on_path("rec", bin, sizeof bin)) {
        snprintf(rec_cmd, sizeof rec_cmd, "%s",
                 "exec rec -q -c 1 -r 16000 -b 16 \"$NLS_WAV\" trim 0 \"$NLS_LISTEN_SECONDS\"");
#ifndef __APPLE__
    } else if (on_path("arecord", bin, sizeof bin)) {
        snprintf(rec_cmd, sizeof rec_cmd, "%s",
                 "exec arecord -q -f S16_LE -c 1 -r 16000 -d \"$NLS_LISTEN_SECONDS\" \"$NLS_WAV\"");
#endif
    } else if (on_path("ffmpeg", bin, sizeof bin)) {
#ifdef __APPLE__
        const char *input = "-f avfoundation -i :0";
#else
        const char *input = "-f alsa -i default";
#endif
        snprintf(rec_cmd, sizeof rec_cmd,
                 "exec ffmpeg -nostdin -hide_banner -loglevel error %s -t \"$NLS_LISTEN_SECONDS\" "
                 "-ac 1 -ar 16000 -y \"$NLS_WAV\"", input);
    } else {
        fprintf(stderr, "nls: --listen needs a program to record with: sox (rec), "
#ifndef __APPLE__
                        "arecord, "
#endif
                        "or ffmpeg;\n  or set NLS_RECORD to a command that records \"$NLS_WAV\"\n");
        return 2;
    }

    const char *tmp = getenv("TMPDIR");
    char dir[1024], wav[1100], rec_log[1100], stt_log[1100];
    int k = snprintf(dir, sizeof dir, "%s/nls-voice.XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    if (k < 0 || (size_t)k >= sizeof dir) {
        fprintf(stderr, "nls: TMPDIR is too long\n");
        return 1;
    }
    if (!mkdtemp(dir)) {
        fprintf(stderr, "nls: can't make a temporary folder: %s\n", strerror(errno));
        return 1;
    }
    snprintf(wav, sizeof wav, "%s/request.wav", dir);
    snprintf(rec_log, sizeof rec_log, "%s/record.log", dir);
    snprintf(stt_log, sizeof stt_log, "%s/stt.log", dir);
    const char *const files[] = { wav, rec_log, stt_log, NULL };
    char nsecs[16];
    snprintf(nsecs, sizeof nsecs, "%d", seconds);
    setenv("NLS_WAV", wav, 1);
    setenv("NLS_LISTEN_SECONDS", nsecs, 1);
    setenv("NLS_STT_LOG", stt_log, 1);

    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    interrupted = 0;
    sigaction(SIGINT, &sa, &old);

    int rc = 0;
    if (isatty(2))
        fprintf(stderr, "nls: listening... press Enter to stop (up to %d s)\n", seconds);
    bool stopped;
    int st = record(rec_cmd, rec_log, seconds, &stopped);
    struct stat sb;
    if (interrupted) {
        fprintf(stderr, "nls: aborted\n");
        rc = 130;
    } else if (st < 0) {
        fprintf(stderr, "nls: can't start the recorder: %s\n", strerror(errno));
        rc = 1;
    } else if (!stopped && !(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
        fprintf(stderr, "nls: recording failed (%s %d):\n",
                WIFEXITED(st) ? "exit" : "signal", WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
        show_log(rec_log);
        rc = 1;
    } else if (stat(wav, &sb) != 0 || sb.st_size == 0) {
        fprintf(stderr, "nls: nothing was recorded\n");
        show_log(rec_log);
        rc = 1;
    }

    if (rc == 0) {
        FILE *p = popen(stt_cmd, "r");
        size_t len = 0;
        if (p) {
            size_t k;
            while ((k = fread(raw + len, 1, sizeof raw - 1 - len, p)) > 0) len += k;
            raw[len] = '\0';
            int pst = pclose(p);
            if (interrupted) {
                fprintf(stderr, "nls: aborted\n");
                rc = 130;
            } else if (pst != 0) {
                fprintf(stderr, "nls: transcription failed (exit %d):\n",
                        WIFEXITED(pst) ? WEXITSTATUS(pst) : 128 + WTERMSIG(pst));
                show_log(stt_log);
                rc = 1;
            }
        } else {
            fprintf(stderr, "nls: can't start the transcription: %s\n", strerror(errno));
            rc = 1;
        }
        if (rc == 0) {
            if (verbose) fprintf(stderr, "transcript: %s\n", raw);
            if (!nl_voice_normalize(raw, out, size)) {
                fprintf(stderr, "nls: the request is too long (max %zu bytes)\n", size - 1);
                rc = 2;
            } else if (out[0] == '\0') {
                fprintf(stderr, "nls: didn't hear anything");
#ifdef __APPLE__
                fprintf(stderr, " (if the microphone is blocked, allow your terminal in\n"
                                "  System Settings > Privacy & Security > Microphone)");
#endif
                fputc('\n', stderr);
                rc = 1;
            }
        }
    }

    sigaction(SIGINT, &old, NULL);
    cleanup(dir, files);
    unsetenv("NLS_WAV");
    unsetenv("NLS_STT_LOG");
    unsetenv("NLS_STT_PROMPT");
    unsetenv("NLS_WHISPER");
    return rc;
}

#endif
