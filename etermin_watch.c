/*
 * etermin_watch - poll an eTermin booking endpoint for free appointment slots.
 *
 * Build:  make
 * Usage:  ./etermin_watch [config.ini]
 *
 * See README.md and config.ini.example.
 */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#define MAX_HEADERS 16
#define MAX_TARGETS 32

typedef struct {
    char  *name;
    char  *url;                 /* may contain {date} {from} {to} {days} */
    char  *headers[MAX_HEADERS];
    int    nheaders;
    char  *start_field;         /* JSON field holding slot start time */
    char  *array_key;           /* optional: key of array inside root object */
    char  *time_format;         /* optional strptime format */
    double min_hours_ahead;     /* ignore slots sooner than this */
    double max_days_ahead;      /* ignore slots later than this */
    char **seen;                /* already reported slots */
    size_t nseen;
} Target;

typedef struct {
    int     interval_minutes;   /* 0 = run once */
    char   *notify_cmd;         /* optional shell command, msg in $ETERMIN_MSG */
    Target  targets[MAX_TARGETS];
    int     ntargets;
} Config;

/* ---------- small helpers ---------- */

static char *xstrdup(const char *s) {
    char *p = strdup(s);
    if (!p) { perror("strdup"); exit(1); }
    return p;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static char *replace_all(const char *src, const char *key, const char *val) {
    size_t klen = strlen(key), vlen = strlen(val), n = 0;
    for (const char *p = src; (p = strstr(p, key)); p += klen) n++;
    char *out = malloc(strlen(src) + n * (vlen > klen ? vlen - klen : 0) + 1);
    if (!out) { perror("malloc"); exit(1); }
    char *w = out;
    const char *p = src, *q;
    while ((q = strstr(p, key))) {
        memcpy(w, p, q - p); w += q - p;
        memcpy(w, val, vlen); w += vlen;
        p = q + klen;
    }
    strcpy(w, p);
    return out;
}

/* ---------- config ---------- */

static void target_defaults(Target *t, const char *name) {
    memset(t, 0, sizeof *t);
    t->name = xstrdup(name);
    t->start_field = xstrdup("start");
    t->min_hours_ahead = 0;
    t->max_days_ahead = 14;
}

static int load_config(const char *path, Config *cfg) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    memset(cfg, 0, sizeof *cfg);
    cfg->interval_minutes = 0;

    char line[2048];
    Target *cur = NULL;
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;

        if (*s == '[') {
            char *end = strchr(s, ']');
            if (!end) { fprintf(stderr, "%s:%d: bad section\n", path, lineno); return -1; }
            *end = '\0';
            char *sec = trim(s + 1);
            if (!strcmp(sec, "global")) {
                cur = NULL;
            } else if (!strncmp(sec, "target:", 7)) {
                if (cfg->ntargets >= MAX_TARGETS) { fprintf(stderr, "too many targets\n"); return -1; }
                cur = &cfg->targets[cfg->ntargets++];
                target_defaults(cur, trim(sec + 7));
            } else {
                fprintf(stderr, "%s:%d: unknown section [%s]\n", path, lineno, sec);
                return -1;
            }
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) { fprintf(stderr, "%s:%d: expected key = value\n", path, lineno); return -1; }
        *eq = '\0';
        char *key = trim(s), *val = trim(eq + 1);

        if (!cur) {
            if (!strcmp(key, "interval_minutes")) cfg->interval_minutes = atoi(val);
            else if (!strcmp(key, "notify_cmd")) cfg->notify_cmd = xstrdup(val);
            else fprintf(stderr, "%s:%d: unknown global key '%s'\n", path, lineno, key);
        } else {
            if (!strcmp(key, "url")) cur->url = xstrdup(val);
            else if (!strcmp(key, "header")) {
                if (cur->nheaders < MAX_HEADERS) cur->headers[cur->nheaders++] = xstrdup(val);
            }
            else if (!strcmp(key, "start_field")) { free(cur->start_field); cur->start_field = xstrdup(val); }
            else if (!strcmp(key, "array_key")) cur->array_key = xstrdup(val);
            else if (!strcmp(key, "time_format")) cur->time_format = xstrdup(val);
            else if (!strcmp(key, "min_hours_ahead")) cur->min_hours_ahead = atof(val);
            else if (!strcmp(key, "max_days_ahead")) cur->max_days_ahead = atof(val);
            else fprintf(stderr, "%s:%d: unknown target key '%s'\n", path, lineno, key);
        }
    }
    fclose(f);

    for (int i = 0; i < cfg->ntargets; i++)
        if (!cfg->targets[i].url) {
            fprintf(stderr, "target '%s' has no url\n", cfg->targets[i].name);
            return -1;
        }
    if (cfg->ntargets == 0) { fprintf(stderr, "no [target:...] sections in config\n"); return -1; }
    return 0;
}

/* ---------- HTTP ---------- */

typedef struct { char *data; size_t len; } Buf;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    Buf *b = ud;
    size_t n = size * nmemb;
    char *p = realloc(b->data, b->len + n + 1);
    if (!p) return 0;
    b->data = p;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static char *http_get(const char *url, char **headers, int nheaders) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    Buf b = {0};
    struct curl_slist *hl = NULL;
    for (int i = 0; i < nheaders; i++) hl = curl_slist_append(hl, headers[i]);

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "etermin-watch/0.1 (personal use)");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");

    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hl);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        fprintf(stderr, "curl: %s\n", curl_easy_strerror(rc));
        free(b.data);
        return NULL;
    }
    if (code >= 400) {
        fprintf(stderr, "HTTP %ld\n", code);
        free(b.data);
        return NULL;
    }
    return b.data;
}

/* ---------- slot handling ---------- */

static int parse_time(const char *s, const char *fmt, time_t *out) {
    static const char *defaults[] = {
        "%Y-%m-%dT%H:%M:%S", "%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", NULL
    };
    struct tm tm;
    const char *fmts[2] = { fmt, NULL };
    const char **list = fmt ? fmts : defaults;
    for (; *list; list++) {
        memset(&tm, 0, sizeof tm);
        char *end = strptime(s, *list, &tm);
        if (end) {
            tm.tm_isdst = -1;           /* let mktime decide (local time) */
            *out = mktime(&tm);
            return *out == (time_t)-1 ? -1 : 0;
        }
    }
    return -1;
}

static int already_seen(Target *t, const char *key) {
    for (size_t i = 0; i < t->nseen; i++)
        if (!strcmp(t->seen[i], key)) return 1;
    return 0;
}

static void remember(Target *t, const char *key) {
    char **p = realloc(t->seen, (t->nseen + 1) * sizeof *p);
    if (!p) return;
    t->seen = p;
    t->seen[t->nseen++] = xstrdup(key);
}

static void notify(const Config *cfg, const char *msg) {
    if (!cfg->notify_cmd) return;
    setenv("ETERMIN_MSG", msg, 1);
    if (system(cfg->notify_cmd) != 0)
        fprintf(stderr, "notify_cmd failed\n");
}

static void check_target(const Config *cfg, Target *t) {
    time_t now = time(NULL);
    time_t win_from = now + (time_t)(t->min_hours_ahead * 3600.0);
    time_t win_to   = now + (time_t)(t->max_days_ahead * 86400.0);

    char d_from[16], d_to[16], days[16];
    strftime(d_from, sizeof d_from, "%Y-%m-%d", localtime(&win_from));
    strftime(d_to,   sizeof d_to,   "%Y-%m-%d", localtime(&win_to));
    snprintf(days, sizeof days, "%d", (int)(t->max_days_ahead + 0.999));

    char *u1 = replace_all(t->url, "{date}", d_from);
    char *u2 = replace_all(u1, "{from}", d_from);
    char *u3 = replace_all(u2, "{to}", d_to);
    char *url = replace_all(u3, "{days}", days);
    free(u1); free(u2); free(u3);

    char *body = http_get(url, t->headers, t->nheaders);
    free(url);
    if (!body) { fprintf(stderr, "[%s] request failed\n", t->name); return; }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { fprintf(stderr, "[%s] response is not valid JSON\n", t->name); return; }

    cJSON *arr = root;
    if (t->array_key) arr = cJSON_GetObjectItemCaseSensitive(root, t->array_key);
    if (!cJSON_IsArray(arr)) {
        fprintf(stderr, "[%s] expected a JSON array (check array_key)\n", t->name);
        cJSON_Delete(root);
        return;
    }

    int total = 0, fresh = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        cJSON *f = cJSON_IsObject(item)
                 ? cJSON_GetObjectItemCaseSensitive(item, t->start_field) : item;
        if (!cJSON_IsString(f) || !f->valuestring) continue;

        time_t st;
        if (parse_time(f->valuestring, t->time_format, &st) != 0) continue;
        if (st < win_from || st > win_to) continue;
        total++;

        if (already_seen(t, f->valuestring)) continue;
        remember(t, f->valuestring);
        fresh++;

        char when[64], msg[256];
        strftime(when, sizeof when, "%a %d.%m.%Y %H:%M", localtime(&st));
        printf("[%s] free: %s\n", t->name, when);
        snprintf(msg, sizeof msg, "%s: free appointment %s", t->name, when);
        notify(cfg, msg);
    }
    if (fresh == 0)
        printf("[%s] no new slots (%d in window)\n", t->name, total);

    cJSON_Delete(root);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "config.ini";
    Config cfg;
    if (load_config(path, &cfg) != 0) return 1;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    for (;;) {
        for (int i = 0; i < cfg.ntargets; i++) {
            check_target(&cfg, &cfg.targets[i]);
            if (i + 1 < cfg.ntargets) sleep(2);   /* be polite to the server */
        }
        fflush(stdout);
        if (cfg.interval_minutes <= 0) break;
        sleep((unsigned)cfg.interval_minutes * 60);
    }
    curl_global_cleanup();
    return 0;
}
