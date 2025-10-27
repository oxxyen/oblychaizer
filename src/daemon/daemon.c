/* vpn_manager.c
 *
 * Демон для поиска бесплатных VPN (VPN Gate), запуска OpenVPN по конфидам,
 * мониторинга throughput на туннельном интерфейсе и управления списками
 * alive / await.
 *
 * Требования: openvpn, curl.
 *
 * Компиляция:
 *   gcc -pthread -O2 vpn_manager.c -o vpn_manager
 *
 * Запуск (обычно требуется root для создания tun-интерфейса):
 *   sudo ./vpn_manager
 *
 * Это прототип; используй с осторожностью.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

#define MAX_IP_LEN 64
#define MAX_CFG_PATH 256
#define MAX_LIST 20000

// Настройки (можно править)
#define VGATE_API "http://www.vpngate.net/api/iphone/" // CSV-like API
#define IP_LIST_FILE "ip_list.txt"
#define CFG_DIR "ovpn_cfgs"
#define FETCH_INTERVAL 300           // сек между обновлениями VPN Gate
#define CONNECT_TIMEOUT 20           // сек ожидания поднятия интерфейса
#define INITIAL_CHECK_WINDOW_SEC 4   // сек для измерения initial throughput
#define MIN_KBPS 50.0                // минимальная допустимая скорость (KB/s) для alive
#define ACTIVE_CHECK_INTERVAL 5      // сек между проверками активного туннеля
#define AWAIT_RETRY_SEC 60           // сек перед повторной попыткой из await

typedef struct cfg_node {
    char ip[MAX_IP_LEN];
    char cfg_path[MAX_CFG_PATH];
    pid_t pid;            // pid запущенного openvpn, 0 если не запущен
    char iface[32];       // tun interface, если известно
    time_t last_failed;
    int failures;
} cfg_node_t;

typedef struct cfg_list {
    cfg_node_t arr[MAX_LIST];
    int count;
} cfg_list_t;

// Глобальные списки
cfg_list_t pool;   // копилка (cfg путь + ip)
cfg_list_t alive;
cfg_list_t awaitl;

pthread_mutex_t pool_m = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t alive_m = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t await_m = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t current_m = PTHREAD_MUTEX_INITIALIZER;

int current_active = -1;
volatile bool should_stop = false;

/* ---------- Утилиты: базовый base64 decode ---------- */
/* Простой base64 decode; подходит для конфигов VPN Gate */
static const unsigned char b64_table[256] = {
    [0 ... 255] = 0x80,
    ['A'] = 0,  ['B'] = 1,  ['C'] = 2,  ['D'] = 3,  ['E'] = 4,  ['F'] = 5,  ['G'] = 6,  ['H'] = 7,
    ['I'] = 8,  ['J'] = 9,  ['K'] = 10, ['L'] = 11, ['M'] = 12, ['N'] = 13, ['O'] = 14, ['P'] = 15,
    ['Q'] = 16, ['R'] = 17, ['S'] = 18, ['T'] = 19, ['U'] = 20, ['V'] = 21, ['W'] = 22, ['X'] = 23,
    ['Y'] = 24, ['Z'] = 25,
    ['a'] = 26, ['b'] = 27, ['c'] = 28, ['d'] = 29, ['e'] = 30, ['f'] = 31, ['g'] = 32, ['h'] = 33,
    ['i'] = 34, ['j'] = 35, ['k'] = 36, ['l'] = 37, ['m'] = 38, ['n'] = 39, ['o'] = 40, ['p'] = 41,
    ['q'] = 42, ['r'] = 43, ['s'] = 44, ['t'] = 45, ['u'] = 46, ['v'] = 47, ['w'] = 48, ['x'] = 49,
    ['y'] = 50, ['z'] = 51,
    ['0'] = 52, ['1'] = 53, ['2'] = 54, ['3'] = 55, ['4'] = 56, ['5'] = 57, ['6'] = 58, ['7'] = 59,
    ['8'] = 60, ['9'] = 61, ['+'] = 62, ['/'] = 63, ['='] = 0
};

int base64_decode(const char *in, unsigned char **out, size_t *out_len) {
    int len = strlen(in);
    int i=0,j=0;
    unsigned char *buf = malloc((len*3)/4 + 3);
    if (!buf) return -1;
    unsigned char quad[4];
    int qlen = 0;
    int outpos = 0;
    while (i < len) {
        unsigned char ch = in[i++];
        unsigned char v = b64_table[ch];
        if (v & 0x80) continue; // skip invalid / whitespace
        quad[qlen++] = v;
        if (qlen == 4) {
            buf[outpos++] = (quad[0]<<2) | (quad[1]>>4);
            if (in[i-2] != '=') buf[outpos++] = (quad[1]<<4) | (quad[2]>>2);
            if (in[i-1] != '=') buf[outpos++] = (quad[2]<<6) | quad[3];
            qlen = 0;
        }
    }
    *out = buf;
    *out_len = outpos;
    return 0;
}

/* ---------- Работа с файлами/списками ---------- */

void list_init(cfg_list_t *l) { l->count = 0; }
bool list_contains(cfg_list_t *l, const char *cfg_path) {
    for (int i=0;i<l->count;i++) if (strcmp(l->arr[i].cfg_path, cfg_path)==0) return true;
    return false;
}
bool add_to_list(cfg_list_t *l, const char *ip, const char *cfg_path) {
    if (l->count >= MAX_LIST) return false;
    if (list_contains(l, cfg_path)) return false;
    strncpy(l->arr[l->count].ip, ip, MAX_IP_LEN-1);
    strncpy(l->arr[l->count].cfg_path, cfg_path, MAX_CFG_PATH-1);
    l->arr[l->count].pid = 0;
    l->arr[l->count].iface[0] = 0;
    l->arr[l->count].last_failed = 0;
    l->arr[l->count].failures = 0;
    l->count++;
    return true;
}
bool remove_by_index(cfg_list_t *l, int idx) {
    if (idx < 0 || idx >= l->count) return false;
    if (idx != l->count-1) l->arr[idx] = l->arr[l->count-1];
    l->count--;
    return true;
}

/* ---------- Fetch VPN Gate CSV, извлечение base64 .ovpn ---------- */
/* Формат VPN Gate API: CSV lines, где последняя колонка OpenVPN_ConfigData_Base64 (base64 конфиг) */
void fetch_vpngate_and_store() {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "curl -L --max-time 20 -s \"%s\"", VGATE_API);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "fetch: curl failed\n");
        return;
    }

    // Убедимся что директория для конфигов существует
    mkdir(CFG_DIR, 0755);

    // Читаем весь вывод в буфер и парсим построчно
    char line[8192];
    int line_no = 0;
    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        // Первая строка — заголовки, можно пропускать (но безопасно проверять)
        if (line_no == 1) continue;
        // Строка заканчивается \r\n, почистим
        char *p = line;
        while (*p && (*p == '\r' || *p == '\n')) *p++ = '\0';

        // Разделим CSV по запятым, но учтём что поля могут содержать запятые. В VPN Gate API final column is base64 (без кавычек), а имена и т.д. могут содержать запятые.
        // Подход: найдем последнюю запятую — всё после неё это base64 конфиг.
        char *last_comma = strrchr(line, ',');
        if (!last_comma) continue;
        char *b64 = last_comma + 1;
        if (strlen(b64) < 10) continue;

        // Найдём IP: это второй столбец (после HostName). проще — разобьём первые N столбцов.
        // Разберём до 2-й запятой:
        char tmp[8192];
        strncpy(tmp, line, sizeof(tmp)-1); tmp[sizeof(tmp)-1]=0;
        char *tok1 = strtok(tmp, ","); // HostName
        char *tok2 = strtok(NULL, ","); // IP
        if (!tok2) continue;
        char ipstr[MAX_IP_LEN];
        strncpy(ipstr, tok2, sizeof(ipstr)-1); ipstr[sizeof(ipstr)-1]=0;

        // Декодируем base64
        unsigned char *decoded = NULL;
        size_t dec_len = 0;
        if (base64_decode(b64, &decoded, &dec_len) != 0 || dec_len < 10) {
            if (decoded) free(decoded);
            continue;
        }

        // Сохраним конфиг в файл
        char cfg_path[MAX_CFG_PATH];
        time_t t = time(NULL);
        snprintf(cfg_path, sizeof(cfg_path), CFG_DIR"/ovpn_%ld_%s.ovpn", t, ipstr);
        FILE *cf = fopen(cfg_path, "wb");
        if (!cf) { free(decoded); continue; }
        fwrite(decoded, 1, dec_len, cf);
        fclose(cf);
        free(decoded);

        // Добавим путь в ip_list.txt (копилка) и в pool (в памяти)
        // Запишем ip|cfg_path строкой в файл
        FILE *lf = fopen(IP_LIST_FILE, "a");
        if (lf) {
            fprintf(lf, "%s %s\n", ipstr, cfg_path);
            fclose(lf);
        }

        // И добавим в pool (потокобезопасно)
        pthread_mutex_lock(&pool_m);
        add_to_list(&pool, ipstr, cfg_path);
        pthread_mutex_unlock(&pool_m);
        printf("[FETCH] added %s -> %s\n", ipstr, cfg_path);
    }

    pclose(fp);
}

/* ---------- Вспомогательные: чтение /proc/net/dev и statistics ---------- */

unsigned long long read_uint_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long long v = 0;
    if (fscanf(f, "%llu", &v) != 1) v = 0;
    fclose(f);
    return v;
}

bool iface_exists(const char *ifname) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s", ifname);
    return access(path, F_OK) == 0;
}

unsigned long long iface_rx_bytes(const char *ifname) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_bytes", ifname);
    return read_uint_file(path);
}
unsigned long long iface_tx_bytes(const char *ifname) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/tx_bytes", ifname);
    return read_uint_file(path);
}

/* Находим первый интерфейс с именем начинающимся на "tun" или "tap" */
bool find_tun_interface(char *out_iface, size_t out_len) {
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return false;
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        if (lineno <= 2) continue;
        // строка формата "  iface: ..." — извлечём имя до :
        char *col = strchr(line, ':');
        if (!col) continue;
        char ifname[64];
        int n = col - line;
        // trim
        int i=0, j=0;
        while (i < n && isspace((unsigned char)line[i])) i++;
        while (i < n && !isspace((unsigned char)line[i])) {
            if (j < (int)sizeof(ifname)-1) ifname[j++] = line[i];
            i++;
        }
        ifname[j]=0;
        if (strncmp(ifname, "tun", 3) == 0 || strncmp(ifname, "tap", 3) == 0) {
            strncpy(out_iface, ifname, out_len-1);
            out_iface[out_len-1]=0;
            fclose(f);
            return true;
        }
    }
    fclose(f);
    return false;
}

/* ---------- Запуск OpenVPN (fork/exec) и ожидание интерфейса ---------- */

/* Запускаем openvpn в foreground (без --daemon), чтобы получить pid процесса.
   Возвращает pid процесса или -1 при ошибке. */
pid_t start_openvpn(const char *cfg_path) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        // child
        // перенаправим stdout/stderr в /dev/null чтобы не засорять логи
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > 2) close(fd);
        }
        execlp("openvpn", "openvpn", "--config", cfg_path, "--verb", "3", (char*)NULL);
        // если exec не выполнится
        perror("execlp openvpn");
        _exit(1);
    }
    // parent
    // Не ждём тут; return pid
    return pid;
}

/* Останавливаем openvpn (по pid) */
void stop_openvpn(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    // ждём чуть-чуть; затем SIGKILL если не завершился
    int waited = 0;
    while (waited < 5) {
        int st;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return;
        sleep(1); waited++;
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

/* Ждём, пока появится tun интерфейс (или пока child умер), с таймаутом.
   Возвращает имя интерфейса в out_iface (если найдено). */
bool wait_for_tun(pid_t child_pid, char *out_iface, int timeout_sec) {
    int elapsed = 0;
    while (elapsed < timeout_sec) {
        // если child неожиданно завершился — вернуть false
        int status;
        pid_t r = waitpid(child_pid, &status, WNOHANG);
        if (r == child_pid) {
            // child exited
            return false;
        }
        // найти интерфейс tun*
        if (find_tun_interface(out_iface, 32)) return true;
        sleep(1);
        elapsed++;
    }
    return false;
}

/* ---------- Измерение скорости на интерфейсе ---------- */
/* Возвращает kbps (KB/s), измеряем усреднением по window_sec */
double measure_iface_kbps(const char *iface, int window_sec) {
    if (!iface || !iface[0]) return 0.0;
    unsigned long long rx1 = iface_rx_bytes(iface);
    unsigned long long tx1 = iface_tx_bytes(iface);
    sleep(window_sec);
    unsigned long long rx2 = iface_rx_bytes(iface);
    unsigned long long tx2 = iface_tx_bytes(iface);
    unsigned long long delta = (rx2 - rx1) + (tx2 - tx1);
    double kb = (double)delta / 1024.0;
    double kbps = kb / (double)window_sec;
    return kbps;
}

/* ---------- Основная логика демона ---------- */

/* Попытка подключиться по cfg: запускаем openvpn, ждём интерфейс, меряем initial throughput.
   Если скорость >= MIN_KBPS — считаем успешным и добавляем в alive.
   Иначе зовём в await и убираем процесс.
*/
void try_connect_and_classify(cfg_node_t *node) {
    printf("[TRY] connecting %s -> %s\n", node->ip, node->cfg_path);
    pid_t pid = start_openvpn(node->cfg_path);
    if (pid <= 0) {
        printf("[ERR] failed to start openvpn for %s\n", node->cfg_path);
        node->last_failed = time(NULL);
        node->failures++;
        pthread_mutex_lock(&await_m);
        add_to_list(&awaitl, node->ip, node->cfg_path);
        pthread_mutex_unlock(&await_m);
        return;
    }
    // ждём интерфейс
    char iface[32] = {0};
    bool got = wait_for_tun(pid, iface, CONNECT_TIMEOUT);
    if (!got) {
        printf("[FAIL] no tun iface for %s (pid %d)\n", node->cfg_path, (int)pid);
        stop_openvpn(pid);
        node->last_failed = time(NULL);
        node->failures++;
        pthread_mutex_lock(&await_m);
        add_to_list(&awaitl, node->ip, node->cfg_path);
        pthread_mutex_unlock(&await_m);
        return;
    }
    printf("[OK] got iface %s for %s\n", iface, node->cfg_path);
    // short throughput check
    double kbps = measure_iface_kbps(iface, INITIAL_CHECK_WINDOW_SEC);
    printf("[SPEED] %s iface=%s initial=%.1f KB/s\n", node->cfg_path, iface, kbps);
    if (kbps >= MIN_KBPS) {
        // success: добавляем в alive; сохраняем pid/iface
        pthread_mutex_lock(&alive_m);
        if (add_to_list(&alive, node->ip, node->cfg_path)) {
            int idx = alive.count - 1;
            alive.arr[idx].pid = pid;
            strncpy(alive.arr[idx].iface, iface, sizeof(alive.arr[idx].iface)-1);
            printf("[ALIVE] %s (%.1f KB/s)\n", node->cfg_path, kbps);
        } else {
            // не добавили — остановим
            stop_openvpn(pid);
        }
        pthread_mutex_unlock(&alive_m);
    } else {
        // не прошёл порог — move to await и остановим клиента
        stop_openvpn(pid);
        node->last_failed = time(NULL);
        node->failures++;
        pthread_mutex_lock(&await_m);
        add_to_list(&awaitl, node->ip, node->cfg_path);
        pthread_mutex_unlock(&await_m);
        printf("[AWAIT] %s (%.1f KB/s)\n", node->cfg_path, kbps);
    }
}

/* Парсер-поток: периодически дергает VPN Gate API и пополняет pool */
void *parser_thread(void *arg) {
    while (!should_stop) {
        printf("[PARSER] fetching VPN Gate list...\n");
        fetch_vpngate_and_store();
        // спать длительнее — обновлять раз в FETCH_INTERVAL
        for (int i=0; i < FETCH_INTERVAL && !should_stop; i++) sleep(1);
    }
    return NULL;
}

/* Демон-поток: берет элементы из pool и пытается классифицировать */
void *daemon_thread(void *arg) {
    while (!should_stop) {
        pthread_mutex_lock(&pool_m);
        if (pool.count == 0) {
            pthread_mutex_unlock(&pool_m);
            // если pool пуст — попробуем через await реанимировать старые
            pthread_mutex_lock(&await_m);
            time_t now = time(NULL);
            for (int i=0;i<awaitl.count;) {
                cfg_node_t node = awaitl.arr[i];
                double age = difftime(now, node.last_failed);
                if (age >= AWAIT_RETRY_SEC) {
                    // удаляем из await и пробуем
                    // копируем cfg_path потому что структура может измениться
                    char cfg_path[MAX_CFG_PATH]; strncpy(cfg_path, node.cfg_path, MAX_CFG_PATH);
                    char ip[MAX_IP_LEN]; strncpy(ip, node.ip, MAX_IP_LEN);
                    // удаляем
                    remove_by_index(&awaitl, i);
                    pthread_mutex_unlock(&await_m);
                    // try connect
                    cfg_node_t temp; strncpy(temp.ip, ip, MAX_IP_LEN); strncpy(temp.cfg_path, cfg_path, MAX_CFG_PATH);
                    try_connect_and_classify(&temp);
                    pthread_mutex_lock(&await_m);
                    // start over (safe)
                    now = time(NULL);
                    i = 0;
                    continue;
                } else {
                    i++;
                }
            }
            pthread_mutex_unlock(&await_m);
            sleep(3);
            continue;
        }
        // берем первый элемент pool
        cfg_node_t node = pool.arr[0];
        // удалим из pool
        remove_by_index(&pool, 0);
        pthread_mutex_unlock(&pool_m);

        // Попробуем подключиться и классифицировать
        try_connect_and_classify(&node);

        usleep(100000); // 100ms
    }
    return NULL;
}

/* Монитор активной сессии: проверяет throughput текущего active; при падении переключает */
void *active_monitor_thread(void *arg) {
    while (!should_stop) {
        pthread_mutex_lock(&current_m);
        int ai = current_active;
        pthread_mutex_unlock(&current_m);

        if (ai == -1) {
            pthread_mutex_lock(&alive_m);
            if (alive.count > 0) {
                pthread_mutex_lock(&current_m);
                current_active = 0;
                ai = 0;
                pthread_mutex_unlock(&current_m);
                printf("[MONITOR] set active to 0 (%s)\n", alive.arr[0].cfg_path);
            }
            pthread_mutex_unlock(&alive_m);
            sleep(1);
            continue;
        }

        pthread_mutex_lock(&alive_m);
        if (ai >= alive.count) {
            // устарел индекс
            pthread_mutex_unlock(&alive_m);
            pthread_mutex_lock(&current_m); current_active = -1; pthread_mutex_unlock(&current_m);
            sleep(1);
            continue;
        }

        cfg_node_t node = alive.arr[ai];
        pthread_mutex_unlock(&alive_m);

        // Проверим, жив ли pid
        if (node.pid <= 0) {
            printf("[MON] no pid for active %s, removing\n", node.cfg_path);
            pthread_mutex_lock(&alive_m);
            remove_by_index(&alive, ai);
            if (alive.count > 0) {
                pthread_mutex_lock(&current_m);
                current_active = 0;
                pthread_mutex_unlock(&current_m);
            } else {
                pthread_mutex_lock(&current_m);
                current_active = -1;
                pthread_mutex_unlock(&current_m);
            }
            pthread_mutex_unlock(&alive_m);
            continue;
        }
        // измерим скорость
        double kbps = measure_iface_kbps(node.iface, ACTIVE_CHECK_INTERVAL);
        printf("[MON] active %s iface=%s kbps=%.1f\n", node.cfg_path, node.iface, kbps);
        if (kbps < MIN_KBPS) {
            // переводим в await: останавливаем openvpn, перемещаем
            printf("[MON] active fell below threshold -> moving to await: %s\n", node.cfg_path);
            stop_openvpn(node.pid);

            pthread_mutex_lock(&alive_m);
            // убедимся что он всё ещё там и удалим
            int idx = -1;
            for (int i=0;i<alive.count;i++) if (strcmp(alive.arr[i].cfg_path, node.cfg_path)==0) { idx=i; break; }
            cfg_node_t removed;
            if (idx != -1) {
                removed = alive.arr[idx];
                remove_by_index(&alive, idx);
            }
            pthread_mutex_unlock(&alive_m);

            if (idx != -1) {
                removed.last_failed = time(NULL);
                removed.failures += 1;
                pthread_mutex_lock(&await_m);
                add_to_list(&awaitl, removed.ip, removed.cfg_path);
                pthread_mutex_unlock(&await_m);
            }

            // переключаем current_active на следующий alive если есть
            pthread_mutex_lock(&alive_m);
            if (alive.count > 0) {
                pthread_mutex_lock(&current_m);
                current_active = 0;
                pthread_mutex_unlock(&current_m);
                printf("[MON] switched to %s\n", alive.arr[0].cfg_path);
            } else {
                pthread_mutex_lock(&current_m);
                current_active = -1;
                pthread_mutex_unlock(&current_m);
                printf("[MON] no alive left\n");
            }
            pthread_mutex_unlock(&alive_m);
        } else {
            // всё ок — оставляем
        }

        sleep(1);
    }
    return NULL;
}

/* Логгер/статус поток */
void *logger_thread(void *arg) {
    while (!should_stop) {
        pthread_mutex_lock(&pool_m); int pc = pool.count; pthread_mutex_unlock(&pool_m);
        pthread_mutex_lock(&alive_m); int ac = alive.count; pthread_mutex_unlock(&alive_m);
        pthread_mutex_lock(&await_m); int wc = awaitl.count; pthread_mutex_unlock(&await_m);
        pthread_mutex_lock(&current_m); int ci = current_active; pthread_mutex_unlock(&current_m);
        printf("[STATUS] pool=%d alive=%d await=%d current=%d\n", pc, ac, wc, ci);
        sleep(10);
    }
    return NULL;
}

/* Обработчик сигнала для аккуратного завершения (посылаем SIGTERM всем openvpn и выходим) */
void handle_sigint(int sig) {
    should_stop = true;
    printf("Signal caught, stopping...\n");
    // остановим все alive
    pthread_mutex_lock(&alive_m);
    for (int i=0;i<alive.count;i++) {
        if (alive.arr[i].pid > 0) {
            printf("[STOP] killing pid %d (%s)\n", alive.arr[i].pid, alive.arr[i].cfg_path);
            stop_openvpn(alive.arr[i].pid);
        }
    }
    pthread_mutex_unlock(&alive_m);
    // очистка не обязательна, процесс завершится
}

int main(int argc, char **argv) {
    printf("VPN Manager starting... (requires openvpn & curl)\n");
    // Создать директории/файлы
    mkdir(CFG_DIR, 0755);
    FILE *f = fopen(IP_LIST_FILE, "a"); if (f) fclose(f);

    list_init(&pool); list_init(&alive); list_init(&awaitl);

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    pthread_t p_thread, d_thread, m_thread, l_thread;
    pthread_create(&p_thread, NULL, parser_thread, NULL);
    pthread_create(&d_thread, NULL, daemon_thread, NULL);
    pthread_create(&m_thread, NULL, active_monitor_thread, NULL);
    pthread_create(&l_thread, NULL, logger_thread, NULL);

    // Главный поток ждёт завершения (Ctrl+C)
    while (!should_stop) sleep(1);

    // join threads (не обязательно, т.к. on SIGINT мы уже убили процессы)
    pthread_join(p_thread, NULL);
    pthread_join(d_thread, NULL);
    pthread_join(m_thread, NULL);
    pthread_join(l_thread, NULL);

    printf("VPN Manager stopped.\n");
    return 0;
}
