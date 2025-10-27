// source/daemon/storage/redis_store.c
#include "redis_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

redisContext* redis_connect(void) {
    redisContext *c = redisConnect("127.0.0.1", 6379);
    if (!c || c->err) {
        if (c) {
            fprintf(stderr, "[-] Redis connection error: %s\n", c->errstr);
            redisFree(c);
        } else {
            fprintf(stderr, "[-] Cannot allocate Redis context\n");
        }
        return NULL;
    }
    printf("[+] Connected to Redis\n");
    return c;
}

int redis_save_vpn_server(
    redisContext *c,
    const char *site,
    const char *ip,
    int port,
    const char *proto,
    const char *country,
    double score,
    const char *config_url
) {
    if (!c || !site || !ip || !proto || !config_url) {
        return -1;
    }

    // Генерируем уникальный хеш (можно заменить на SHA1, но для демо — достаточно)
    char key[256];
    snprintf(key, sizeof(key), "vpn:servers:%s:%s:%d:%s", site, ip, port, proto);

    // Временная метка
    time_t now = time(NULL);

    // Формируем команду HMSET (Redis 6+ поддерживает, но лучше использовать HSET)
    redisReply *reply = redisCommand(c,
        "HSET %s ip %s port %d protocol %s country %s score %f last_seen %ld config_url %s",
        key, ip, port, proto, country ? country : "??", score, now, config_url
    );

    if (!reply) {
        fprintf(stderr, "[-] Redis command failed\n");
        return -1;
    }

    if (reply->type == REDIS_REPLY_ERROR) {
        fprintf(stderr, "[-] Redis error: %s\n", reply->str);
        freeReplyObject(reply);
        return -1;
    }

    // Устанавливаем TTL = 24 часа (серверы устаревают)
    redisCommand(c, "EXPIRE %s 86400", key);
    freeReplyObject(reply);
    return 0;
}