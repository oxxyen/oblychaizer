#include <hiredis/hiredis.h>

#include <hiredis/read.h>
#include <stdlib.h>
#include <stdio.h>

#include "configs/redis_utils.h"


int hiredis_connect(const char *ip, int port) {

    redis_connect_t r_conn;

    redisContext *c = redisConnect(r_conn.redis_ip, r_conn.redis_port);

    if (c == NULL | c->err) {

        if (c) {

            printf("error connect: %s\n", c->errstr);
            redisFree(c);

        } else {

            printf("failed context Redis\n");

        }
        exit(1);

    }

    printf("connect redis success!\n");

    redisReply *reply = redisCommand(c, "LPUSH ip_queue %s", "1.2.3.4:1194");
    printf("LPUSH reply: %lld\n", reply->integer);

    freeReplyObject(reply);

    reply = redisCommand(c, "RPOP ip_queue");

    if (reply->type == REDIS_REPLY_STRING) {

        printf("get out: %s\n", reply->str);

    } else {

        printf("empty\n");

    }

    freeReplyObject(reply);



}