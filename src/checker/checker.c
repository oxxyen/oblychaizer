// checker.c
// Компиляция: gcc -std=c11 -O2 -Wall checker.c -o checker -pthread -lcurl -lhiredis
// Запуск: ./checker [num_workers]
// Требования: libcurl, hiredis

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#include <hiredis/hiredis.h>
#include <curl/curl.h>

const char *REDIS_HOST = "127.0.0.1";
const int REDIS_PORT = 6379;

const char *IP_QUEUE_KEY = "ip_queue";
const char *AWAIT_LIST_KEY = "await_list";
const char *ALIVE_ZKEY = "alive";
const char *METRICS_PREFIX = "ip:metrics:";

#define DEFAULT_WORKERS 6
#define CONNECT_TIMEOUT_SEC 3
#define HTTP_TIMEOUT_SEC 5

// ---------------- TCP connect with timeout ----------------
int tcp_connect_timeout(const char *ip, int port, int timeout_sec) {
    int sock = -1;
    struct sockaddr_in sa;
    socklen_t len;
    int flags, res, so_err;
    fd_set wfds;
    struct timeval tv;

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) return -1;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) <= 0) { close(sock); return -1; }

    if ((flags = fcntl(sock, F_GETFL, 0)) < 0) { close(sock); return -1; }
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) { close(sock); return -1; }

    res = connect(sock, (struct sockaddr*)&sa, sizeof(sa));
    if (res == 0) { fcntl(sock, F_SETFL, flags); return sock; }
    if (errno != EINPROGRESS) { close(sock); return -1; }

    FD_ZERO(&wfds); FD_SET(sock, &wfds);
    tv.tv_sec = timeout_sec; tv.tv_usec = 0;
    res = select(sock + 1, NULL, &wfds, NULL, &tv);
    if (res <= 0) { close(sock); return -1; }

    len = sizeof(so_err);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &len) < 0) { close(sock); return -1; }
    if (so_err != 0) { close(sock); errno = so_err; return -1; }

    fcntl(sock, F_SETFL, flags);
    return sock;
}

// ---------------- HTTP GET through proxy ----------------
long http_check_latency(const char *ip, int port) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    char proxy[256];
    snprintf(proxy, sizeof(proxy), "%s:%d", ip, port);

    curl_easy_setopt(curl, CURLOPT_URL, "http://example.com/"); // короткий GET
    curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, HTTP_TIMEOUT_SEC);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); // HEAD request быстрее
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    CURLcode res = curl_easy_perform(curl);
    clock_gettime(CLOCK_MONOTONIC, &end);

    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return -1;

    long latency_ms = (end.tv_sec - start.tv_sec) * 1000 +
                      (end.tv_nsec - start.tv_nsec) / 1000000;
    return latency_ms;
}

// ---------------- Redis helpers ----------------
void mark_alive(redisContext *r, const char *ipport, long latency_ms) {
    time_t now = time(NULL);
    redisReply *rep = redisCommand(r, "ZADD %s %lld %s", ALIVE_ZKEY, (long long)now, ipport);
    if (rep) freeReplyObject(rep);

    char key[512];
    snprintf(key, sizeof(key), "%s%s", METRICS_PREFIX, ipport);
    rep = redisCommand(r, "HSET %s state alive last_seen %lld fail_count 0 latency_ms %ld",
                       key, (long long)now, latency_ms);
    if (rep) freeReplyObject(rep);
}

void mark_await(redisContext *r, const char *ipport) {
    time_t now = time(NULL);
    redisReply *rep = redisCommand(r, "LPUSH %s %s", AWAIT_LIST_KEY, ipport);
    if (rep) freeReplyObject(rep);

    char key[512];
    snprintf(key, sizeof(key), "%s%s", METRICS_PREFIX, ipport);
    rep = redisCommand(r, "HINCRBY %s fail_count 1", key); if (rep) freeReplyObject(rep);
    rep = redisCommand(r, "HSET %s state await last_seen %lld", key, (long long)now); if (rep) freeReplyObject(rep);
}

// ---------------- Worker ----------------
void *checker_worker(void *arg) {
    (void)arg;
    redisContext *r = redisConnect(REDIS_HOST, REDIS_PORT);
    if (!r || r->err) { if (r) fprintf(stderr, "Redis error: %s\n", r->errstr); if (r) redisFree(r); return NULL; }

    while (1) {
        redisReply *rep = redisCommand(r, "RPOP %s", IP_QUEUE_KEY);
        if (!rep) { sleep(1); continue; }
        if (rep->type == REDIS_REPLY_NIL) { freeReplyObject(rep); sleep(1); continue; }
        if (rep->type != REDIS_REPLY_STRING) { freeReplyObject(rep); continue; }

        char item[512]; strncpy(item, rep->str, sizeof(item)-1); item[sizeof(item)-1] = '\0';
        freeReplyObject(rep);

        char ip[256]; int port;
        if (sscanf(item, "%255[^:]:%d", ip, &port) != 2) continue;

        int s = tcp_connect_timeout(ip, port, CONNECT_TIMEOUT_SEC);
        if (s >= 0) {
            close(s);
            long latency = http_check_latency(ip, port);
            if (latency >= 0) mark_alive(r, item, latency);
            else mark_await(r, item);
            printf("[CHECK] %s TCP ok, latency=%ldms\n", item, latency);
        } else {
            mark_await(r, item);
            printf("[CHECK] %s TCP fail\n", item);
        }
    }
    redisFree(r);
    return NULL;
}

int main(int argc, char **argv) {
    curl_global_init(CURL_GLOBAL_ALL);

    int workers = DEFAULT_WORKERS;
    if (argc >= 2) workers = atoi(argv[1]) > 0 ? atoi(argv[1]) : DEFAULT_WORKERS;

    pthread_t th[workers];
    for (int i = 0; i < workers; ++i)
        if (pthread_create(&th[i], NULL, checker_worker, NULL) != 0) perror("pthread_create");

    for (int i = 0; i < workers; ++i) pthread_join(th[i], NULL);

    curl_global_cleanup();
    return 0;
}
