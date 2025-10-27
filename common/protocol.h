#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


typedef struct {

    int client_port = 7677;
    char client_ip[10] = "127.0.0.1";

    struct sockaddr_in sock_addr;

} client_sock_t;

typedef struct {

    int p;

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