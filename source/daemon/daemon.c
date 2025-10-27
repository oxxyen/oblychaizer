#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "daemon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>          // вместо <bits/fcntl-linux.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <limits.h>

#include "parser/sites.h"
#include "../../database/redis/utils/redis_store.h"
#include "parser/extractor.c"

#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>

// Буфер для ответа от сервера
struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;  
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "[-] realloc() failed\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// Безопасное сохранение файла в разрешённую директорию
int save_file_safe(const char *filename, const char *content, size_t len) {
    char target_path[PATH_MAX];
    char real_target[PATH_MAX];

    // Ограничиваем длину имени файла
    if (strlen(filename) > 200) {
        fprintf(stderr, "[-] Filename too long\n");
        return -1;
    }

    // Формируем путь
    int res = snprintf(target_path, sizeof(target_path),
                       "source/daemon/resource/%s", filename);
    if (res < 0 || (size_t)res >= sizeof(target_path)) {
        fprintf(stderr, "[-] Path too long\n");
        return -1;
    }

    // Получаем канонический путь
    if (!realpath(target_path, real_target)) {
        // Если файла ещё нет, realpath может вернуть NULL — создадим родительскую директорию
        // Но сначала проверим базовый путь
        char base_dir[PATH_MAX];
        if (!realpath("source/daemon/resource/", base_dir)) {
            fprintf(stderr, "[-] Base resource directory not found\n");
            return -1;
        }

        // Проверка на path traversal: убедимся, что целевой путь будет внутри base_dir
        char test_path[PATH_MAX];
        snprintf(test_path, sizeof(test_path), "%s/%s", base_dir, filename);

        if (!realpath(test_path, real_target)) {
            // Всё ещё нет — возможно, файл не существует, но путь валиден
            strncpy(real_target, test_path, sizeof(real_target) - 1);
        }
    }

    // Проверяем, что путь начинается с нужной директории
    char base_dir[PATH_MAX];
    if (!realpath("source/daemon/resource/", base_dir)) {
        fprintf(stderr, "[-] Cannot resolve base directory\n");
        return -1;
    }

    if (strncmp(real_target, base_dir, strlen(base_dir)) != 0) {
        fprintf(stderr, "[-] Path traversal attempt blocked: %s\n", real_target);
        return -1;
    }

    FILE *fp = fopen(real_target, "wb");
    if (!fp) {
        perror("[-] fopen");
        return -1;
    }

    fwrite(content, 1, len, fp);
    fclose(fp);
    printf("[+] Saved: %s\n", real_target);
    return 0;
}

// Парсим HTML и ищем .ovpn ссылки
void parse_html_for_ovpn(const char *html_content, const char *site_name) {
    htmlDocPtr doc = htmlReadDoc((xmlChar*)html_content, NULL, NULL,
                                 HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc) {
        fprintf(stderr, "[-] Failed to parse HTML from %s\n", site_name);
        return;
    }

    xmlXPathContextPtr xpathCtx = xmlXPathNewContext(doc);
    xmlXPathObjectPtr xpathObj = xmlXPathEvalExpression(
        (xmlChar*)"//a[@href and contains(@href, '.ovpn')]", xpathCtx);

    if (xpathObj && xpathObj->nodesetval && xpathObj->nodesetval->nodeNr > 0) {
        for (int i = 0; i < xpathObj->nodesetval->nodeNr; i++) {
            xmlNodePtr node = xpathObj->nodesetval->nodeTab[i];
            xmlChar *href = xmlGetProp(node, (xmlChar*)"href");

            if (href) {
                char full_url[2048];
                if (strncmp((char*)href, "http", 4) == 0) {
                    // Абсолютная ссылка
                    strncpy(full_url, (char*)href, sizeof(full_url) - 1);
                    full_url[sizeof(full_url) - 1] = '\0';
                } else {
                    // Относительная ссылка
                    if (strcmp(site_name, "vpngate") == 0) {
                        snprintf(full_url, sizeof(full_url), "https://www.vpngate.net%s", href);
                    } else if (strcmp(site_name, "vpnbook") == 0) {
                        snprintf(full_url, sizeof(full_url), "https://www.vpnbook.com%s", href);
                    } else {
                        snprintf(full_url, sizeof(full_url), "https://%s%s", site_name, href);
                    }
                }

                printf("[*] Found OVPN: %s\n", full_url);

                // Скачиваем сам файл
                CURL *curl;
                CURLcode res;
                struct MemoryStruct chunk = {0};

                chunk.memory = malloc(1);
                if (!chunk.memory) {
                    xmlFree(href);
                    continue;
                }

                curl = curl_easy_init();
                if (curl) {
                    curl_easy_setopt(curl, CURLOPT_URL, full_url);
                    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
                    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
                    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (compatible; VPNParser/1.0)");
                    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

                    res = curl_easy_perform(curl);
                    if (res == CURLE_OK) {
                        // Извлекаем имя файла из URL
                        char *last_slash = strrchr(full_url, '/');
                        const char *fname = last_slash ? last_slash + 1 : "config.ovpn";
                        save_file_safe(fname, chunk.memory, chunk.size);
                    } else {
                        fprintf(stderr, "[-] Download failed: %s\n", curl_easy_strerror(res));
                    }

                    curl_easy_cleanup(curl);
                }
                free(chunk.memory);
                xmlFree(href);
            }
        }
    }

    xmlXPathFreeObject(xpathObj);
    xmlXPathFreeContext(xpathCtx);
    xmlFreeDoc(doc);
}

// Функция для парсинга одного сайта
int fetch_site(const char *url, const char *site_name) {
    CURL *curl;
    CURLcode res;
    struct MemoryStruct chunk = {0};

    chunk.memory = malloc(1);
    if (!chunk.memory) return -1;

    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (compatible; VPNParser/1.0)");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            printf("[+] Fetched %s successfully.\n", url);
            parse_html_for_ovpn(chunk.memory, site_name);
        } else {
            fprintf(stderr, "[-] Failed to fetch %s: %s\n", url, curl_easy_strerror(res));
        }

        curl_easy_cleanup(curl);
    }

    free(chunk.memory);
    return 0;
}

// Главная функция парсинга сайтов
int fetch_and_parse_vpn_sites(void) {
    printf("[*] Starting VPN config parser...\n");

    struct {
        const char *url;
        const char *name;
    } sites[] = {
        {"https://www.vpngate.net/en/", "vpngate"},
        {"https://www.vpnbook.com/freevpn", "vpnbook"},
    };

    for (size_t i = 0; i < sizeof(sites)/sizeof(sites[0]); i++) {
        fetch_site(sites[i].url, sites[i].name);
    }

    return 0;
}

// Запуск демона (фоновый режим)
int start_daemon(void) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        printf("[+] Daemon started with PID: %d\n", (int)pid);
        exit(EXIT_SUCCESS);
    }

    // Дочерний процесс — демон
    if (setsid() < 0) {
        perror("setsid");
        exit(EXIT_FAILURE);
    }

    umask(0); 

    // Закрываем стандартные дескрипторы
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    // Лог в файл
    int logfd = open("/tmp/vpn_parser.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logfd >= 0) {
        dup2(logfd, STDOUT_FILENO);
        dup2(logfd, STDERR_FILENO);
        close(logfd);
    }

    // Основной цикл
    while (1) {
        time_t now = time(NULL);
        printf("[*] Running at %s", ctime(&now));

        fetch_and_parse_vpn_sites();

        sleep(3600); // каждые 60 минут
    }

    return 0;
}