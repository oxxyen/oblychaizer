#ifndef REDIS_UTILS_H
#define REDIS_UTILS_H

typedef struct {

    char redis_ip[10] = "127.0.0.1";
    int redis_port = 6379;

} redis_connect_t;

#endif