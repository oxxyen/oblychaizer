#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include <hiredis/hiredis.h>

#define REDIS_HOST "127.0.0.1"
#define REDIS_PORT 6379

#define IP_QUEUE_KEY "ip_queue"
#define ALIVE_SET_KEY "alive"
#define AWAIT_LIST_KEY "await_list"
#define METRICS_PREFIX "ip:metrics:"

#define MAX_WORKERS 6
#define TCP_TIMEOUT_SEC 3
#define TESTER_TIMEOUT_SEC 10

typedef struct {

// todo:

} ip_state_t;


typedef struct {

    char ip[64];
    int port;
    ip_state_t state; 
    double rtt_ms;
    double packet_loss;
    double throughput_kbps;
    time_t last_checked;
    int fail_count;

} vpn_server_t;