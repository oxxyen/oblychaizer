
// main.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>

#define IP_MAXLEN 64
#define PENDING_MAX 10000
#define ALIVE_MAX 1024
#define AWAIT_MAX 4096

// Настройки
const char *IP_LIST_FILE = "ip_list.txt";
const int PARSER_INTERVAL = 30; // сек
const int CHECKER_THREADS = 4;
const int PING_TIMEOUT_SEC = 1;
const int TCP_CONNECT_TIMEOUT_MS = 800;
const int ACTIVE_CHECK_INTERVAL = 10; // сек
const int AWAIT_RETRY_INTERVAL = 60; // сек

// Параметры качества
const int MIN_RTT_MS = 200; // допустимое RTT
const double MIN_SPEED_MBPS = 0.5; // placeholder

typedef enum { S_PENDING, S_ALIVE, S_AWAIT } state_t;

typedef struct server {
    char ip[IP_MAXLEN];
    state_t state;
    time_t last_checked;
    int last_rtt_ms; // -1 если нет данных
    double last_speed_mbps; // placeholder
} server_t;

/* Очередь pending (простая) */
server_t *pending[PENDING_MAX];
int pending_head = 0, pending_tail = 0;
pthread_mutex_t pending_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t pending_cond = PTHREAD_COND_INITIALIZER;

/* alive */
server_t *alive[ALIVE_MAX];
int alive_count = 0;
pthread_mutex_t alive_mtx = PTHREAD_MUTEX_INITIALIZER;

/* await */
server_t *await_list[AWAIT_MAX];
int await_count = 0;
pthread_mutex_t await_mtx = PTHREAD_MUTEX_INITIALIZER;

/* текущий активный сервер */
server_t *active = NULL;
pthread_mutex_t active_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Утилиты очереди */
int pending_push(server_t *s) {
    pthread_mutex_lock(&pending_mtx);
    int next = (pending_tail + 1) % PENDING_MAX;
    if (next == pending_head) { pthread_mutex_unlock(&pending_mtx); return -1; } // full
    pending[pending_tail] = s;
    pending_tail = next;
    pthread_cond_signal(&pending_cond);
    pthread_mutex_unlock(&pending_mtx);
    return 0;
}
server_t* pending_pop_blocking() {
    pthread_mutex_lock(&pending_mtx);
    while (pending_head == pending_tail) pthread_cond_wait(&pending_cond, &pending_mtx);
    server_t *s = pending[pending_head];
    pending_head = (pending_head + 1) % PENDING_MAX;
    pthread_mutex_unlock(&pending_mtx);
    return s;
}

/* Добавляем в alive */
int alive_add(server_t *s) {
    pthread_mutex_lock(&alive_mtx);
    if (alive_count >= ALIVE_MAX) { pthread_mutex_unlock(&alive_mtx); return -1; }
    alive[alive_count++] = s;
    s->state = S_ALIVE;
    pthread_mutex_unlock(&alive_mtx);
    return 0;
}

/* Добавляем в await с меткой времени */
int await_add(server_t *s) {
    pthread_mutex_lock(&await_mtx);
    if (await_count >= AWAIT_MAX) { pthread_mutex_unlock(&await_mtx); return -1; }
    s->state = S_AWAIT;
    s->last_checked = time(NULL);
    await_list[await_count++] = s;
    pthread_mutex_unlock(&await_mtx);
    return 0;
}

/* Удалить из alive (по индексу) */
void alive_remove_index(int idx) {
    pthread_mutex_lock(&alive_mtx);
    if (idx < 0 || idx >= alive_count) { pthread_mutex_unlock(&alive_mtx); return; }
    for (int i = idx; i < alive_count - 1; ++i) alive[i] = alive[i+1];
    alive_count--;
    pthread_mutex_unlock(&alive_mtx);
}

/* Простые проверки */

/* ping через системный ping, возвращает RTT ms в out_rtt или -1 */
int quick_ping_rtt(const char *ip, int *out_rtt_ms) {
    char cmd[256];
    // Используем одно эхо с таймаутом (совместимо с busybox/suid-less ping)
    snprintf(cmd, sizeof(cmd), "ping -c 1 -W %d %s 2>/dev/null", PING_TIMEOUT_SEC, ip);
    int rc = system(cmd);
    if (rc != 0) { if (out_rtt_ms) *out_rtt_ms = -1; return -1; }
    // parse output RTT через ping - можно выполнить снова с -q, но для простоты парсим с grep
    // более корректно: выполнить popen и искать 'time='
    FILE *f = popen(cmd, "r");
    if (!f) { if (out_rtt_ms) *out_rtt_ms = -1; return 0; }
    char buf[512];
    int rtt = -1;
    while (fgets(buf, sizeof(buf), f)) {
        char *p = strstr(buf, "time=");
        if (p) {
            p += 5;
            double t = atof(p);
            rtt = (int) (t + 0.5);
            break;
        }
    }
    pclose(f);
    if (out_rtt_ms) *out_rtt_ms = rtt;
    return rtt >= 0 ? 0 : 0; // success but may not get rtt
}

/* TCP connect to port with timeout, returns 0 if connected */
int tcp_connect_timeout(const char *ip, int port, int timeout_ms) {
    struct addrinfo hints = {0}, *res = NULL, *rp;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char sport[16];
    snprintf(sport, sizeof(sport), "%d", port);
    if (getaddrinfo(ip, sport, &hints, &res) != 0) return -1;
    int ok = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        int s = socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK, rp->ai_protocol);
        if (s < 0) continue;
        int rc = connect(s, rp->ai_addr, rp->ai_addrlen);
        if (rc == 0) { ok = 0; close(s); break; }
        if (errno == EINPROGRESS) {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(s, &wf);
            struct timeval tv = { timeout_ms/1000, (timeout_ms%1000)*1000 };
            int sel = select(s+1, NULL, &wf, NULL, &tv);
            if (sel > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0) { ok = 0; close(s); break; }
            }
        }
        close(s);
    }
    freeaddrinfo(res);
    return ok;
}

/* quick_check: ping + try TCP to common VPN ports */
int quick_check(server_t *s) {
    int rtt = -1;
    quick_ping_rtt(s->ip, &rtt); // best-effort
    s->last_rtt_ms = rtt;
    // try common VPN ports: 1194 (openvpn/udp TCP maybe), 443, 51820 (wireguard/udp)
    int ports[] = {1194, 443, 80/*HTTP maybe*/, 22/*ssh maybe*/, 0};
    int connected = -1;
    for (int i=0; ports[i]; ++i) {
        if (tcp_connect_timeout(s->ip, ports[i], TCP_CONNECT_TIMEOUT_MS) == 0) { connected = 0; break; }
    }
    // Quality decision: if ping responded or tcp connect succeeded -> accept
    if ( (rtt >= 0 && rtt <= MIN_RTT_MS) || connected == 0 ) {
        s->last_checked = time(NULL);
        s->last_speed_mbps = 0.0; // placeholder
        return 0;
    }
    s->last_checked = time(NULL);
    return -1;
}

/* Placeholder speed check — реализация: запуск iperf3 через туннель. Тут просто эмуляция. */
int speed_check(server_t *s) {
    // TODO: реализовать реальную проверку скорости, например:
    // 1) запустить iperf3 client на удалённый тестовый сервер по туннелю
    // 2) измерить throughput и вернуть 0 если >= MIN_SPEED_MBPS
    // Пока: считаем скорость ок, если last_rtt_ms положительный и < threshold
    if (s->last_rtt_ms > 0 && s->last_rtt_ms < (MIN_RTT_MS * 2)) return 0;
    return -1;
}

/* Вызов внешних скриптов для старта/стопа VPN — пользователь должен реализовать их */
int start_vpn_for_ip(const char *ip) {
    // скрипт должен запустить VPN в фоне и вернуть 0 если успешно
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "./start_vpn.sh %s", ip);
    int rc = system(cmd);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}
int stop_vpn() {
    int rc = system("./stop_vpn.sh");
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

/* Checker thread */
void *checker_thread_fn(void *arg) {
    (void)arg;
    while (1) {
        server_t *s = pending_pop_blocking();
        if (!s) { sleep(1); continue; }
        if (quick_check(s) == 0) {
            // прошел quick_check -> в alive
            if (alive_add(s) != 0) {
                // если нет места в alive, отправим в await
                await_add(s);
            }
        } else {
            await_add(s);
        }
    }
    return NULL;
}

/* Парсер: читает ip_list.txt и добавляет новые IP в pending */
void *parser_thread_fn(void *arg) {
    (void)arg;
    char **seen = NULL;
    size_t seen_n = 0;
    while (1) {
        FILE *f = fopen(IP_LIST_FILE, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                char *p = line;
                while (*p && (*p==' '||*p=='\t'||*p=='\r'||*p=='\n')) p++;
                if (*p==0) continue;
                char ip[IP_MAXLEN];
                sscanf(p, "%63s", ip);
                // check seen
                int found = 0;
                for (size_t i=0;i<seen_n;i++) if (strcmp(seen[i], ip)==0) { found = 1; break; }
                if (!found) {
                    seen = realloc(seen, sizeof(char*)*(seen_n+1));
                    seen[seen_n++] = strdup(ip);
                    server_t *s = calloc(1, sizeof(server_t));
                    strncpy(s->ip, ip, sizeof(s->ip)-1);
                    s->state = S_PENDING;
                    s->last_rtt_ms = -1;
                    pending_push(s);
                }
            }
            fclose(f);
        }
        // recheck await_list: если прошло AWAIT_RETRY_INTERVAL с last_checked, обратно в pending
        pthread_mutex_lock(&await_mtx);
        time_t now = time(NULL);
        for (int i=0;i<await_count;) {
            server_t *as = await_list[i];
            if (now - as->last_checked >= AWAIT_RETRY_INTERVAL) {
                // перемещаем в pending
                // remove from await
                for (int j=i;j<await_count-1;j++) await_list[j]=await_list[j+1];
                await_count--;
                // push to pending
                as->state = S_PENDING;
                pending_push(as);
            } else ++i;
        }
        pthread_mutex_unlock(&await_mtx);
        sleep(PARSER_INTERVAL);
    }
    return NULL;
}

/* Монитор: следит за active, периодически проверяет качество и переключает при падении */
void *monitor_thread_fn(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&active_mtx);
        server_t *cur = active;
        pthread_mutex_unlock(&active_mtx);
        if (!cur) {
            // нет активного - попробовать взять из alive
            pthread_mutex_lock(&alive_mtx);
            if (alive_count > 0) {
                server_t *pick = alive[0];
                // remove from alive
                alive_remove_index(0);
                pthread_mutex_unlock(&alive_mtx);
                // start vpn for pick
                if (start_vpn_for_ip(pick->ip) == 0) {
                    pthread_mutex_lock(&active_mtx);
                    active = pick;
                    pthread_mutex_unlock(&active_mtx);
                    printf("[monitor] activated %s\n", pick->ip);
                } else {
                    // move to await
                    await_add(pick);
                }
            } else {
                pthread_mutex_unlock(&alive_mtx);
            }
        } else {
            // есть активный: проверяем качество
            if (quick_check(cur) != 0 || speed_check(cur) != 0) {
                printf("[monitor] active %s degraded -> switching\n", cur->ip);
                // stop vpn
                stop_vpn();
                // move current to await
                await_add(cur);
                pthread_mutex_lock(&active_mtx);
                active = NULL;
                pthread_mutex_unlock(&active_mtx);
                // возьмём следующий в следующей итерации
            } else {
                // ok
                // можно обновить last_checked
            }
        }
        sleep(ACTIVE_CHECK_INTERVAL);
    }
    return NULL;
}

/* main */
int main(int argc, char **argv) {
    printf("vpn-monitor daemon start\n");
    pthread_t parser, monitor;
    pthread_t checkers[CHECKER_THREADS];

    pthread_create(&parser, NULL, parser_thread_fn, NULL);
    pthread_create(&monitor, NULL, monitor_thread_fn, NULL);
    for (int i=0;i<CHECKER_THREADS;i++) pthread_create(&checkers[i], NULL, checker_thread_fn, NULL);

    // main thread может ещё логировать статус или ждать сигналов
    while (1) {
        sleep(60);
        // dump simple status
        pthread_mutex_lock(&alive_mtx);
        printf("[status] alive_count=%d\n", alive_count);
        for (int i=0;i<alive_count;i++) printf("  alive[%d]=%s rtt=%d\n", i, alive[i]->ip, alive[i]->last_rtt_ms);
        pthread_mutex_unlock(&alive_mtx);
        pthread_mutex_lock(&await_mtx);
        printf("[status] await_count=%d\n", await_count);
        for (int i=0;i<await_count;i++) printf("  await[%d]=%s last_checked=%ld\n", i, await_list[i]->ip, (long)await_list[i]->last_checked);
        pthread_mutex_unlock(&await_mtx);
        pthread_mutex_lock(&active_mtx);
        if (active) printf("[status] active=%s\n", active->ip);
        else printf("[status] active=(none)\n");
        pthread_mutex_unlock(&active_mtx);
    }

    return 0;
}
