#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <hiredis/hiredis.h>

#define MAX_LINE 256

int is_valid_ip_port(char *line) {
    int dots = 0;
    int colon = 0;
    for (int i = 0; line[i]; i++) {
        if (isdigit(line[i])) continue;
        if (line[i] == '.') dots++;
        else if (line[i] == ':') colon++;
        else return 0;
    }
    return (dots == 3 && colon == 1);
}

int main(void) {
    FILE *file = fopen("ip_list.txt", "r");
    if (!file) {
        perror("ip_list.txt");
        return 1;
    }

    redisContext *c = redisConnect("127.0.0.1", 6379);
    if (c == NULL || c->err) {
        if (c)
            fprintf(stderr, "Redis connection error: %s\n", c->errstr);
        else
            fprintf(stderr, "Can't allocate redis context\n");
        return 1;
    }

    char line[MAX_LINE];
    int added = 0;

    while (fgets(line, sizeof(line), file)) {
        char *p = strchr(line, '\n');
        if (p) *p = '\0';

        if (!is_valid_ip_port(line)) {
            printf("skip invalid: %s\n", line);
            continue;
        }

        redisReply *reply = redisCommand(c, "LPUSH ip_queue %s", line);
        if (reply) freeReplyObject(reply);

        added++;
    }

    fclose(file);
    redisFree(c);

    printf("Done. Added %d servers.\n", added);
    return 0;
}
