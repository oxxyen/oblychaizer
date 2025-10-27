#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include "../../database/redis/utils/redis_store.h"
#include "parser.h"
#include "../../config/config.h"
#include <time.h>

static int is_valid_ip(const char *ip) {
    struct in_addr addr;

    return inet_aton(ip, &addr) != 0;
}

static int is_port_reachable(const char *ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0) return 0;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if(!inet_ntoa(ip, &addr.sin_addr)) {
        close(sock);
        return 0;
    }

    struct timeval tv;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    int res = connect(sock, (struct sockaddr*)&addr, sizeof(addr));

    close(sock);
    return res == 0;
}

static double safe_atof(const char *str) {
    if(!str) return 0.0;
    double *end;
    double val = strtod(str, &end);
    return (end == str || *end != '\0') ? 0.0 : val;
}

int parse_vpngate_page(const char *html, redisContext *redis_ctx) {
    htmlDocPtr doc = htmlReadDoc((xmlChar*)html, NULL, NULL,

    HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING 

);

    xmlPathContextPtr xpathCtx = xmlPathNewContext(doc);
    xmlPathObjectPtr xpathObj = xmlPathEvalExpression(

        (xmlChar*)"//table[@id='vg_hosts_table_id']//tr[position()>1]", xpathCtx

    );

    int count = 0;
    if(xpathObj && xpathObj->nodesetval) {
        for(int i = 0; i < xpathObj->nodesetval->nodeNr && count < MAX_SERVERS_PER_SITE; i++) {
            // todo:
        }
    }


}