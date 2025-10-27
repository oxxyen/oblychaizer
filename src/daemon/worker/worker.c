#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <hiredis/hiredis.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

int tcp_connect_timeout(const char *ip_address, int port, int timeout_sec) {
    int sockfd;
    struct sockaddr_in server_addr;
    int flags, res, error;
    socklen_t len;
    fd_set write_fds;
    struct timeval tv;

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return -1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip_address, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sockfd);
        return -1;
    }

    if ((flags = fcntl(sockfd, F_GETFL, 0)) < 0) {
        perror("fcntl F_GETFL");
        close(sockfd);
        return -1;
    }
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl F_SETFL");
        close(sockfd);
        return -1;
    }

    res = connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (res < 0 && errno != EINPROGRESS) {
        perror("connect");
        close(sockfd);
        return -1;
    }

    if (res == 0) {
        fcntl(sockfd, F_SETFL, flags);
        return sockfd;
    }

    FD_ZERO(&write_fds);
    FD_SET(sockfd, &write_fds);
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    res = select(sockfd + 1, NULL, &write_fds, NULL, &tv);
    if (res <= 0) {
        if (res == 0) errno = ETIMEDOUT;
        else perror("select");
        close(sockfd);
        return -1;
    }

    len = sizeof(error);
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
        perror("getsockopt");
        close(sockfd);
        return -1;
    }
    if (error != 0) {
        errno = error;
        close(sockfd);
        return -1;
    }

    fcntl(sockfd, F_SETFL, flags);
    return sockfd;
}

void worker_loop(redisContext *c) {
    while (1) {
        // Получаем IP из ip_queue
        redisReply *reply = redisCommand(c, "RPOP ip_queue");
        if (!reply) {
            fprintf(stderr, "Redis error: %s\n", c->errstr);
            sleep(1);
            continue;
        }

        if (reply->type == REDIS_REPLY_NIL) {
            // очередь пуста
            freeReplyObject(reply);
            sleep(1);
            continue;
        }

        char *ip_port = strdup(reply->str);
        freeReplyObject(reply);

        char ip[64];
        int port;
        if (sscanf(ip_port, "%63[^:]:%d", ip, &port) != 2) {
            fprintf(stderr, "Invalid IP:Port format: %s\n", ip_port);
            free(ip_port);
            continue;
        }
        free(ip_port);

        printf("Checking %s:%d...\n", ip, port);

        int sock = tcp_connect_timeout(ip, port, 3);
        time_t now = time(NULL);

        if (sock >= 0) {
            printf("%s:%d alive\n", ip, port);
            close(sock);

            // Добавляем в alive ZSET с текущим timestamp как score
            redisCommand(c, "ZADD alive %lld %s:%d", (long long)now, ip, port);

            // Сохраняем метрики в hash
            redisCommand(c, "HMSET ip:metrics:%s:%d state alive last_checked %lld fail_count 0",
                         ip, port, (long long)now);
        } else {
            printf("%s:%d failed\n", ip, port);
            // 4. Добавляем в await_list
            redisCommand(c, "LPUSH await_list %s:%d", ip, port);

            redisCommand(c, "HMSET ip:metrics:%s:%d state await last_checked %lld fail_count 1",
                         ip, port, (long long)now);
        }
    }
}

int main() {
    // Подключение к Redis
    redisContext *c = redisConnect("127.0.0.1", 6379);
    if (c == NULL || c->err) {
        if (c) {
            fprintf(stderr, "Redis connection error: %s\n", c->errstr);
            redisFree(c);
        } else {
            fprintf(stderr, "Can't allocate redis context\n");
        }
        return 1;
    }

    printf("Connected to Redis.\n");
    worker_loop(c);

    redisFree(c);
    return 0;
}
