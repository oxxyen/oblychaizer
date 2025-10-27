#include <asm-generic/errno.h>
#include <asm-generic/socket.h>
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

// #include "../../common/protocol.h"

int tcp_connect_timeout(const char *ip_address, int port, int timeout_sec) {

    int sockfd;
    struct sockaddr_in server_addr;

    int flags, res, error;
    socklen_t len;
    fd_set write_fds;

    struct timeval tv;

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {

        perror("socket");
        close(sockfd);
        return -1;

    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip_address, &server_addr.sin_addr) <= 0) {

        perror("inet_pton");
        close(sockfd);
        return -1;
    }
    // set the socket to non blocking mode

    flags = fcntl(sockfd, F_GETFL, NULL);
    
    if (flags < 0) {

        perror("flags");
        close(sockfd);
        return -1;

    }

    res = connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr));

    if (res < 0) {

        if(errno != EINPROGRESS) {

            perror("connect");
            close(sockfd);
            return -1;

        }

    }

    if (res == 0) {

        fcntl(sockfd, F_SETFL, flags);
        printf("connection established immediately\n");
        return sockfd;

    }

    FD_ZERO(&write_fds);
    FD_SET(sockfd, &write_fds);
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    res = select(sockfd + 1, NULL, &write_fds, NULL, &tv);

    if (res <= 0) {
        if(res == 0) errno = ETIMEDOUT;

        else perror("select");
        close(sockfd);

        return -1;

    }

    len = sizeof(error);

    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
    
        perror("getsockopt");
        close(sockfd);

        return 01;

    }

    if (error != 0) {

        errno = error;
        close(sockfd);
        return -1;

    }

    fcntl(sockfd, F_SETFL, flags);

    return sockfd;

}

int main(void) {

    const char *ip = "127.0.0.1";
    int port = 8080;
    int sock = tcp_connect_timeout(ip, port, 3);


    if (sock >= 0) {

        printf("connected %s:%d\n", ip, port);
        close(sock);

    } else {

        perror("fail to conn");
    }

    return 0;

}