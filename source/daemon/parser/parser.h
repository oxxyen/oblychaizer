#ifndef PARSER_H
#define PARSER_H

#include <hiredis/hiredis.h>

typedef struct {

    const char *p;
    int port;
    const char *protocol;

    const char *country;

    double score;
    const char *config_url;
    const char *source;

} vpn_server_t;

int parse_vpn_page(const char *html, const char *source_name, redisContext *redis_ctx);

#endif