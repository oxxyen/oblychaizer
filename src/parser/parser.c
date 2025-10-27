// parser.c
// Компиляция: gcc -std=c11 -O2 -Wall parser.c -o parser -pthread -lcurl -lhiredis
// Запуск: ./parser sources.txt [num_workers]
// Требования: libcurl, hiredis

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <regex.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#include <curl/curl.h>
#include <hiredis/hiredis.h>

#define MAX_SOURCES 2048
#define MAX_LINE 2048
#define DEFAULT_WORKERS 6
#define FETCH_TIMEOUT 20   // увеличен таймаут
#define FETCH_RETRIES 3
#define RATE_LIMIT_SEC 1

// Redis keys
const char *REDIS_HOST = "127.0.0.1";
const int REDIS_PORT = 6379;
const char *IP_SET_KEY = "ip:set";
const char *IP_QUEUE_KEY = "ip_queue";
const char *METRICS_PREFIX = "ip:metrics:";

// thread-safe sources container
typedef struct {
    char *arr[MAX_SOURCES];
    size_t n;
    size_t idx;
    pthread_mutex_t lock;
} sources_t;

static sources_t sources = {.n = 0, .idx = 0, .lock = PTHREAD_MUTEX_INITIALIZER};

// curl memory buffer
struct mem {
    char *buf;
    size_t len;
};

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t add = size * nmemb;
    struct mem *m = (struct mem*) userdata;
    char *tmp = realloc(m->buf, m->len + add + 1);
    if (!tmp) return 0;
    m->buf = tmp;
    memcpy(m->buf + m->len, ptr, add);
    m->len += add;
    m->buf[m->len] = '\0';
    return add;
}

static void mem_init(struct mem *m) { m->buf = NULL; m->len = 0; }
static void mem_free(struct mem *m) { free(m->buf); m->buf = NULL; m->len = 0; }

int load_sources_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        perror("open sources file");
        return -1;
    }
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char *s = line;
        while (*s && isspace((unsigned char)*s)) s++;
        if (*s == '#' || *s == '\0' || *s == '\n') continue;
        char *e = s + strlen(s) - 1;
        while (e >= s && isspace((unsigned char)*e)) { *e = '\0'; e--; }
        if (s[0] == '\0') continue;
        if (sources.n >= MAX_SOURCES) break;
        // фильтр JS/капчи сайтов
        if (strstr(s, "hide.me") || strstr(s, "protonvpn") || strstr(s, "windscribe")) continue;
        sources.arr[sources.n++] = strdup(s);
    }
    fclose(f);
    return 0;
}

char *next_source() {
    pthread_mutex_lock(&sources.lock);
    char *ret = NULL;
    if (sources.idx < sources.n) {
        ret = sources.arr[sources.idx++];
    }
    pthread_mutex_unlock(&sources.lock);
    return ret;
}

int redis_sadd_and_push_new(redisContext *r, const char *item) {
    redisReply *reply = redisCommand(r, "SADD %s %s", IP_SET_KEY, item);
    if (!reply) return -1;
    int added = 0;
    if (reply->type == REDIS_REPLY_INTEGER && reply->integer == 1) {
        added = 1;
        redisReply *r2 = redisCommand(r, "LPUSH %s %s", IP_QUEUE_KEY, item);
        if (r2) freeReplyObject(r2);
        char key[512];
        snprintf(key, sizeof(key), "%s%s", METRICS_PREFIX, item);
        redisReply *r3 = redisCommand(r, "HSET %s state new first_seen %lld last_seen %lld fail_count 0",
                                      key, (long long)time(NULL), (long long)time(NULL));
        if (r3) freeReplyObject(r3);
    }
    freeReplyObject(reply);
    return added;
}

int valid_ipv4_octets(const char *ip) {
    int a,b,c,d;
    if (sscanf(ip, "%d.%d.%d.%d", &a,&b,&c,&d) != 4) return 0;
    if (a<0||a>255||b<0||b>255||c<0||c>255||d<0||d>255) return 0;
    return 1;
}

int valid_port(const char *p) {
    if (!p) return 0;
    char *end;
    long v = strtol(p, &end, 10);
    if (*end != '\0') return 0;
    return (v >= 1 && v <= 65535);
}

void normalize_ip(char *dst, const char *src, size_t sz) {
    if (src[0] == '[') {
        size_t len = strlen(src);
        if (len >= 2 && src[len-1] == ']') {
            snprintf(dst, sz, "%.*s", (int)(len-2), src+1);
            return;
        }
    }
    strncpy(dst, src, sz-1);
    dst[sz-1] = '\0';
}

void parse_buffer_and_store(redisContext *r, const char *buf) {
    if (!buf || buf[0] == '\0') return;
    regex_t re_v4, re_v6;
    const char *pat_v4 = "([0-9]{1,3}(?:\\.[0-9]{1,3}){3}):([0-9]{1,5})";
    const char *pat_v6 = "(\\[[0-9A-Fa-f:]+\\]):([0-9]{1,5})";
    if (regcomp(&re_v4, pat_v4, REG_EXTENDED) != 0) return;
    if (regcomp(&re_v6, pat_v6, REG_EXTENDED) != 0) { regfree(&re_v4); return; }

    const char *p = buf;
    regmatch_t m[3];
    char ipraw[256], portraw[16], ipnorm[256], item[400];

    while (regexec(&re_v4, p, 3, m, 0) == 0) {
        int iplen = m[1].rm_eo - m[1].rm_so;
        int portlen = m[2].rm_eo - m[2].rm_so;
        if (iplen < (int)sizeof(ipraw) && portlen < (int)sizeof(portraw)) {
            memcpy(ipraw, p + m[1].rm_so, iplen); ipraw[iplen] = '\0';
            memcpy(portraw, p + m[2].rm_so, portlen); portraw[portlen] = '\0';
            if (!valid_ipv4_octets(ipraw) || !valid_port(portraw)) { p += m[0].rm_eo; continue; }
            normalize_ip(ipnorm, ipraw, sizeof(ipnorm));
            snprintf(item, sizeof(item), "%s:%s", ipnorm, portraw);
            redis_sadd_and_push_new(r, item);
        }
        p += m[0].rm_eo;
    }

    p = buf;
    while (regexec(&re_v6, p, 3, m, 0) == 0) {
        int iplen = m[1].rm_eo - m[1].rm_so;
        int portlen = m[2].rm_eo - m[2].rm_so;
        if (iplen < (int)sizeof(ipraw) && portlen < (int)sizeof(portraw)) {
            memcpy(ipraw, p + m[1].rm_so, iplen); ipraw[iplen] = '\0';
            memcpy(portraw, p + m[2].rm_so, portlen); portraw[portlen] = '\0';
            normalize_ip(ipnorm, ipraw, sizeof(ipnorm));
            if (!valid_port(portraw)) { p += m[0].rm_eo; continue; }
            snprintf(item, sizeof(item), "%s:%s", ipnorm, portraw);
            redis_sadd_and_push_new(r, item);
        }
        p += m[0].rm_eo;
    }

    regfree(&re_v4);
    regfree(&re_v6);
}

char *fetch_url(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    struct mem m; mem_init(&m);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, FETCH_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        fprintf(stderr, "curl fetch %s failed: %s\n", url, curl_easy_strerror(rc));
        mem_free(&m);
        curl_easy_cleanup(curl);
        return NULL;
    }
    curl_easy_cleanup(curl);
    return m.buf;
}

void process_source(const char *url) {
    if (!url) return;
    redisContext *r = redisConnect(REDIS_HOST, REDIS_PORT);
    if (!r || r->err) {
        if (r) fprintf(stderr, "Redis connect error: %s\n", r->errstr);
        else fprintf(stderr, "Redis alloc error\n");
        if (r) redisFree(r);
        return;
    }

    int attempt;
    int ok = 0;
    for (attempt = 1; attempt <= FETCH_RETRIES; ++attempt) {
        char *body = fetch_url(url);
        if (body) {
            parse_buffer_and_store(r, body);
            free(body);
            ok = 1;
            break;
        }
        sleep(1 << (attempt - 1));
    }
    if (!ok) fprintf(stderr, "Failed to fetch source: %s\n", url);
    redisFree(r);
}

void *worker_thread(void *arg) {
    (void)arg;
    char *url;
    while ((url = next_source()) != NULL) {
        fprintf(stdout, "worker %lu processing %s\n", (unsigned long)pthread_self(), url);
        process_source(url);
        sleep(RATE_LIMIT_SEC);
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *srcfile = "sources.txt";
    int workers = DEFAULT_WORKERS;
    if (argc >= 2) srcfile = argv[1];
    if (argc >= 3) workers = atoi(argv[2]) > 0 ? atoi(argv[2]) : DEFAULT_WORKERS;

    if (load_sources_file(srcfile) != 0) return 1;
    if (sources.n == 0) {
        fprintf(stderr, "No sources in %s\n", srcfile);
        return 1;
    }

    curl_global_init(CURL_GLOBAL_ALL);

    pthread_t threads[workers];
    for (int i = 0; i < workers; ++i) {
        if (pthread_create(&threads[i], NULL, worker_thread, NULL) != 0) perror("pthread_create");
    }
    for (int i = 0; i < workers; ++i) pthread_join(threads[i], NULL);

    curl_global_cleanup();
    fprintf(stdout, "parser finished\n");
    return 0;
}
